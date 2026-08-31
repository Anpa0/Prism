/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Diagnostics.h"
#include "CaptureBridge.h"

#include <algorithm>

namespace
{
/* ReShade installs itself as an API proxy next to the host executable. Prism
 * looks for a module with one of those names that was actually loaded from its
 * own directory, which distinguishes a real ReShade install from the system
 * dxgi.dll that every D3D application loads anyway. */
const wchar_t* const kProxyNames[] = {L"dxgi.dll", L"d3d11.dll", L"d3d10.dll", L"d3d9.dll", L"opengl32.dll"};

bool PathIsInModuleDirectory(const std::wstring& path)
{
    const std::wstring& directory = PrismModuleDirectory();
    if(path.size() < directory.size())
        return false;
    return _wcsnicmp(path.c_str(), directory.c_str(), directory.size()) == 0;
}
} // namespace

ReShadeInfo DetectReShade()
{
    ReShadeInfo info;

    for(const wchar_t* name : kProxyNames)
    {
        HMODULE module = GetModuleHandleW(name);
        if(!module)
            continue;

        wchar_t path[MAX_PATH] = {};
        if(GetModuleFileNameW(module, path, MAX_PATH) == 0)
            continue;

        const bool addonApi = GetProcAddress(module, "ReShadeRegisterAddon") != nullptr;
        if(!addonApi && !PathIsInModuleDirectory(path))
            continue;

        info.loaded     = true;
        info.proxyName  = name;
        info.modulePath = path;
        info.addonApi   = addonApi;
        break;
    }
    return info;
}

double Diagnostics::UpdateRate(uint64_t nowUs, uint64_t& windowStartUs, unsigned& counter, double& rate)
{
    if(windowStartUs == 0)
        windowStartUs = nowUs;
    counter++;

    const uint64_t elapsed = nowUs - windowStartUs;
    if(elapsed >= 500000ull) /* refresh twice a second */
    {
        rate          = static_cast<double>(counter) * 1000000.0 / static_cast<double>(elapsed);
        counter       = 0;
        windowStartUs = nowUs;
    }
    return rate;
}

void Diagnostics::NoteFrameReceived(uint64_t nowUs)
{
    UpdateRate(nowUs, m_incomingWindowUs, m_incomingCount, m_incomingFps);
}

void Diagnostics::NotePresented(uint64_t nowUs)
{
    UpdateRate(nowUs, m_presentedWindowUs, m_presentedCount, m_presentedFps);
}

void Diagnostics::NoteFrameAge(double ageMs)
{
    m_frameAgeMs = ageMs;
    if(ageMs > m_peakFrameAgeMs)
        m_peakFrameAgeMs = ageMs;
}

void Diagnostics::Reset()
{
    *this = Diagnostics();
}

/* %ls, not %s: MinGW's swprintf follows the msvcrt convention where %s in a
 * wide format string means char*, which would print one byte of a wchar_t*. */
std::wstring Diagnostics::BuildSummary(bool capturing, bool testPattern) const
{
    if(!capturing)
        return L"Prism - Idle";

    wchar_t buffer[160];
    swprintf(buffer, 160, L"Prism - %ls  %.0f in / %.0f out fps", testPattern ? L"Test Pattern" : L"Capturing",
             m_incomingFps, m_presentedFps);
    return buffer;
}

std::wstring Diagnostics::BuildReport(const CaptureBridge& bridge, const Renderer& renderer,
                                      const Settings& settings, const ReShadeInfo& reshade,
                                      bool testPattern) const
{
    const PrismBridgeStats stats = bridge.Stats();

    const wchar_t* stateText = L"Idle";
    switch(bridge.State())
    {
    case PRISM_STATE_NEGOTIATING: stateText = L"Selecting source"; break;
    case PRISM_STATE_ACTIVE: stateText = L"Active"; break;
    case PRISM_STATE_ERROR: stateText = L"Error"; break;
    default: break;
    }

    const wchar_t* formatText = L"unknown";
    if(stats.source_format == PRISM_FORMAT_BGRX)
        formatText = L"BGRx (no conversion)";
    else if(stats.source_format == PRISM_FORMAT_BGRA)
        formatText = L"BGRA (no conversion)";

    const wchar_t* modeText = L"Fit";
    switch(settings.displayMode)
    {
    case DisplayMode::OriginalSize: modeText = L"Original Size"; break;
    case DisplayMode::Stretch: modeText = L"Stretch"; break;
    case DisplayMode::IntegerScale: modeText = L"Integer Scale"; break;
    default: break;
    }

    const RenderTimings& timings = renderer.Timings();

    std::wstring text;
    text.resize(4096);
    const int written = swprintf(
        text.data(), text.size(),
        L"Capture\r\n"
        L"  Bridge loaded:        %ls\r\n"
        L"  Capture active:       %ls (%ls)\r\n"
        L"  Status:               %ls\r\n"
        L"  Source resolution:    %u x %u\r\n"
        L"  Source pixel format:  %ls\r\n"
        L"  Negotiated framerate: %u/%u\r\n"
        L"  Incoming FPS:         %.1f\r\n"
        L"  Presented FPS:        %.1f\r\n"
        L"\r\n"
        L"Frame flow\r\n"
        L"  PipeWire buffers:     %llu received, %llu delivered\r\n"
        L"  Dropped at bridge:    %llu (ceiling or superseded)\r\n"
        L"  Dropped unmappable:   %llu\r\n"
        L"  Dropped as stale:     %llu (mailbox overwrites)\r\n"
        L"  Frame ceiling:        %ls\r\n"
        L"\r\n"
        L"Latency\r\n"
        L"  Capture -> present:   %.2f ms (peak %.2f ms)\r\n"
        L"  PipeWire receive lag: %.2f ms\r\n"
        L"  Texture upload:       %.3f ms\r\n"
        L"  Draw:                 %.3f ms\r\n"
        L"  Present:              %.3f ms\r\n"
        L"\r\n"
        L"Rendering\r\n"
        L"  Direct3D API:         D3D11\r\n"
        L"  GPU:                  %ls\r\n"
        L"  Adapter index:        %d\r\n"
        L"  Present path:         %ls\r\n"
        L"  Display mode:         %ls\r\n"
        L"  V-Sync:               %ls\r\n"
        L"\r\n"
        L"ReShade\r\n"
        L"  Loaded:               %ls\r\n"
        L"  Proxy:                %ls\r\n"
        L"  Path:                 %ls\r\n"
        L"  Add-on API:           %ls\r\n",
        bridge.IsLoaded() ? L"Yes" : L"No", bridge.IsActive() ? L"Yes" : L"No", stateText,
        bridge.StatusMessage().empty() ? L"-" : bridge.StatusMessage().c_str(), stats.source_width,
        stats.source_height, formatText, stats.source_fps_num, stats.source_fps_den, m_incomingFps, m_presentedFps,
        (unsigned long long)stats.frames_received, (unsigned long long)stats.frames_delivered,
        (unsigned long long)stats.frames_throttled, (unsigned long long)stats.frames_corrupt,
        (unsigned long long)bridge.Mailbox().StaleDrops(),
        settings.maxFps == 0 ? L"Match source" : std::to_wstring(settings.maxFps).c_str(), m_frameAgeMs,
        m_peakFrameAgeMs, m_captureLatencyMs, timings.uploadMs, timings.drawMs, timings.presentMs,
        renderer.AdapterDescription().empty() ? L"-" : renderer.AdapterDescription().c_str(),
        renderer.AdapterIndex(), renderer.UsesShaderPath() ? L"Fullscreen triangle" : L"Copy blit (no compiler)",
        modeText, settings.vsync ? L"On" : L"Off", reshade.loaded ? L"Yes" : L"No",
        reshade.proxyName.empty() ? L"-" : reshade.proxyName.c_str(),
        reshade.modulePath.empty() ? L"-" : reshade.modulePath.c_str(), reshade.addonApi ? L"Yes" : L"No");

    if(written > 0)
        text.resize(static_cast<size_t>(written));
    else
        text = L"(diagnostics unavailable)";

    if(!renderer.LastError().empty())
        text += L"\r\nNotes\r\n  " + renderer.LastError() + L"\r\n";
    if(!bridge.IsLoaded() && !bridge.LoadError().empty())
        text += L"\r\nNotes\r\n  " + bridge.LoadError() + L"\r\n";
    if(testPattern)
        text += L"\r\nNotes\r\n  Synthetic test pattern is running (--test-pattern); "
                L"no application is being captured.\r\n";

    return text;
}
