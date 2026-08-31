/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Common.h"
#include "FrameMailbox.h"
#include "PrismCapture.h"

#include <functional>

/*
 * PE-side owner of PrismCapture.dll.
 *
 * Load() is deliberately tolerant: on real Windows, or on a Linux session with
 * no ScreenCast portal, the module simply is not there and Prism keeps running
 * with an empty feed instead of dying. Everything the bridge hands back lands
 * in the mailbox; nothing touches Direct3D from the capture thread.
 */
class CaptureBridge
{
public:
    ~CaptureBridge();

    bool Load();
    bool IsLoaded() const { return m_ready; }
    const std::wstring& LoadError() const { return m_loadError; }

    bool Start(unsigned sourceTypes, bool captureCursor, unsigned maxFps);
    void Stop();
    void SetMaxFps(unsigned maxFps);
    void Shutdown();

    bool IsActive() const { return m_active.load(std::memory_order_relaxed); }

    FrameMailbox&       Mailbox() { return m_mailbox; }
    const FrameMailbox& Mailbox() const { return m_mailbox; }

    PrismBridgeStats Stats() const;

    /* Latest status pushed by the bridge, in UTF-16 for the UI. */
    unsigned     State() const { return m_state.load(std::memory_order_relaxed); }
    std::wstring StatusMessage() const;

    /* Posted (not sent) to the UI thread whenever the state changes. */
    void SetStatusWindow(HWND window, UINT message);

private:
    static void __stdcall OnFrameThunk(const PrismFrame* frame, void* context);
    static void __stdcall OnStatusThunk(unsigned state, const char* message, void* context);

    void OnFrame(const PrismFrame* frame);
    void OnStatus(unsigned state, const char* message);

    using VersionFn   = unsigned(__stdcall*)();
    using InitFn      = long(__stdcall*)();
    using StartFn     = long(__stdcall*)(unsigned, unsigned, unsigned, PrismFrameCallback, PrismStatusCallback, void*);
    using StopFn      = long(__stdcall*)();
    using SetMaxFpsFn = void(__stdcall*)(unsigned);
    using GetStatsFn  = void(__stdcall*)(PrismBridgeStats*);
    using ShutdownFn  = void(__stdcall*)();

    HMODULE      m_module = nullptr;
    bool         m_ready  = false;
    std::wstring m_loadError;

    VersionFn   m_version   = nullptr;
    InitFn      m_init      = nullptr;
    StartFn     m_start     = nullptr;
    StopFn      m_stop      = nullptr;
    SetMaxFpsFn m_setMaxFps = nullptr;
    GetStatsFn  m_getStats  = nullptr;
    ShutdownFn  m_shutdown  = nullptr;

    FrameMailbox          m_mailbox;
    std::atomic<bool>     m_active {false};
    std::atomic<unsigned> m_state {PRISM_STATE_IDLE};

    mutable std::mutex m_statusMutex;
    std::wstring       m_statusMessage;

    HWND m_statusWindow  = nullptr;
    UINT m_statusMessageId = 0;
};
