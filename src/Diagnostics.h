/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Common.h"
#include "Renderer.h"
#include "Settings.h"

class CaptureBridge;

/* What Prism knows about the ReShade installation sitting next to Prism.exe. */
struct ReShadeInfo
{
    bool         loaded = false;
    std::wstring proxyName;  /* dxgi.dll, d3d11.dll, ...                  */
    std::wstring modulePath; /* full path of the loaded proxy             */
    bool         addonApi = false; /* exports ReShadeRegisterAddon         */
};

ReShadeInfo DetectReShade();

/*
 * Rolling capture and presentation statistics.
 *
 * Incoming and presented rates are measured over a sliding one-second window
 * rather than smoothed, so a source running at 93 unique frames per second
 * reads as 93 and not as a filtered approximation of 90.
 */
class Diagnostics
{
public:
    void NoteFrameReceived(uint64_t nowUs);
    void NotePresented(uint64_t nowUs);
    void NoteFrameAge(double ageMs);
    void Reset();

    double IncomingFps() const { return m_incomingFps; }
    double PresentedFps() const { return m_presentedFps; }
    double FrameAgeMs() const { return m_frameAgeMs; }
    double PeakFrameAgeMs() const { return m_peakFrameAgeMs; }
    double CaptureLatencyMs() const { return m_captureLatencyMs; }
    void   NoteCaptureLatency(double ms) { m_captureLatencyMs = ms; }

    /* Multi-line report shown in the diagnostics window. */
    std::wstring BuildReport(const CaptureBridge& bridge, const Renderer& renderer, const Settings& settings,
                             const ReShadeInfo& reshade, bool testPattern) const;

    /* One-line summary for the title bar and the tray tooltip. */
    std::wstring BuildSummary(bool capturing, bool testPattern) const;

private:
    static double UpdateRate(uint64_t nowUs, uint64_t& windowStartUs, unsigned& counter, double& rate);

    uint64_t m_incomingWindowUs  = 0;
    unsigned m_incomingCount     = 0;
    double   m_incomingFps       = 0.0;

    uint64_t m_presentedWindowUs = 0;
    unsigned m_presentedCount    = 0;
    double   m_presentedFps      = 0.0;

    double m_frameAgeMs      = 0.0;
    double m_peakFrameAgeMs  = 0.0;
    double m_captureLatencyMs = 0.0;
};
