/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Diagnostics.h"
#include "CaptureBridge.h"

#include "Hotkeys.h"

#include <algorithm>
#include <cstdarg>

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
std::wstring Diagnostics::BuildSummary(bool capturing, bool testPattern, bool gameplayOutput) const
{
    if(!capturing)
        return L"Prism - Idle";

    const wchar_t* state = gameplayOutput ? L"Gameplay Output" : (testPattern ? L"Test Pattern" : L"Capturing");

    wchar_t buffer[160];
    swprintf(buffer, 160, L"Prism - %ls  %.0f in / %.0f out fps", state, m_incomingFps, m_presentedFps);
    return buffer;
}

namespace
{
std::wstring Utf8ToWideText(const char* text)
{
    if(!text || !*text)
        return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if(needed <= 1)
        return std::wstring();
    std::wstring result(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), needed);
    return result;
}

void AppendLine(std::wstring& text, const wchar_t* format, ...)
{
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    vswprintf(buffer, 1024, format, args);
    va_end(args);
    text += buffer;
    text += L"\r\n";
}

const wchar_t* ShortcutStateName(unsigned state)
{
    switch(state)
    {
    case PRISM_SHORTCUTS_PENDING: return L"waiting for the compositor";
    case PRISM_SHORTCUTS_BOUND: return L"bound";
    case PRISM_SHORTCUTS_DENIED: return L"denied by the user";
    case PRISM_SHORTCUTS_ERROR: return L"error";
    default: return L"unavailable";
    }
}
} // namespace

/*
 * The diagnostics report lives in its own window rather than being drawn onto
 * the back buffer, because anything on the back buffer would be processed by
 * ReShade along with the captured image.
 */
std::wstring Diagnostics::BuildReport(const ReportContext& context) const
{
    const CaptureBridge& bridge   = *context.bridge;
    const Renderer&      renderer = *context.renderer;
    const Settings&      settings = *context.settings;
    const ReShadeInfo&   reshade  = *context.reshade;

    const PrismBridgeStats stats   = bridge.Stats();
    const RenderTimings&   timings = renderer.Timings();
    const AdapterInfo&     adapter = renderer.ActiveAdapter();

    const wchar_t* stateText = L"Idle";
    switch(bridge.State())
    {
    case PRISM_STATE_NEGOTIATING: stateText = L"Selecting source"; break;
    case PRISM_STATE_ACTIVE: stateText = L"Active"; break;
    case PRISM_STATE_ERROR: stateText = L"Error"; break;
    default: break;
    }

    const wchar_t* formatText = L"unknown";
    switch(stats.source_format)
    {
    case PRISM_FORMAT_BGRX: formatText = L"BGRx"; break;
    case PRISM_FORMAT_BGRA: formatText = L"BGRA"; break;
    case PRISM_FORMAT_RGBX: formatText = L"RGBx"; break;
    case PRISM_FORMAT_RGBA: formatText = L"RGBA"; break;
    default: break;
    }

    const wchar_t* pixelPathText = L"direct, no conversion (B8G8R8A8)";
    switch(renderer.CurrentPixelPath())
    {
    case PixelPath::DirectRgba: pixelPathText = L"direct, no conversion (R8G8B8A8)"; break;
    case PixelPath::CpuSwizzle: pixelPathText = L"CPU channel swap (no shader compiler)"; break;
    default: break;
    }

    const wchar_t* modeText = L"Fit";
    switch(settings.displayMode)
    {
    case DisplayMode::OriginalSize: modeText = L"Original Size"; break;
    case DisplayMode::Stretch: modeText = L"Stretch"; break;
    case DisplayMode::IntegerScale: modeText = L"Integer Scale"; break;
    default: break;
    }

    std::wstring text;
    text.reserve(6000);

    AppendLine(text, L"Capture");
    AppendLine(text, L"  Bridge loaded:        %ls", bridge.IsLoaded() ? L"Yes" : L"No");
    AppendLine(text, L"  ScreenCast portal:    %ls", bridge.CaptureAvailable() ? L"Available" : L"Unavailable");
    AppendLine(text, L"  Capture active:       %ls (%ls)", bridge.IsActive() ? L"Yes" : L"No", stateText);
    AppendLine(text, L"  Status:               %ls",
               bridge.StatusMessage().empty() ? L"-" : bridge.StatusMessage().c_str());
    AppendLine(text, L"  Source resolution:    %u x %u", stats.source_width, stats.source_height);
    AppendLine(text, L"  PipeWire format:      %ls%ls", formatText,
               stats.needs_swizzle ? L"  (channel order differs from DXGI's default)" : L"");
    AppendLine(text, L"  Stride / buffer size: %u bytes / %u bytes", stats.source_stride, stats.source_max_size);
    AppendLine(text, L"  Row padding:          %d bytes",
               static_cast<int>(stats.source_stride) - static_cast<int>(stats.source_width) * 4);
    AppendLine(text, L"  Negotiated framerate: %u/%u", stats.source_fps_num, stats.source_fps_den);
    AppendLine(text, L"  Incoming FPS:         %.1f", m_incomingFps);
    AppendLine(text, L"  Consumed FPS:         %.1f", m_presentedFps);
    AppendLine(text, L"");

    AppendLine(text, L"Frame flow");
    AppendLine(text, L"  PipeWire buffers:     %llu received, %llu delivered",
               (unsigned long long)stats.frames_received, (unsigned long long)stats.frames_delivered);
    AppendLine(text, L"  Dropped at bridge:    %llu (ceiling or superseded)",
               (unsigned long long)stats.frames_throttled);
    AppendLine(text, L"  Dropped unmappable:   %llu", (unsigned long long)stats.frames_corrupt);
    AppendLine(text, L"  Rejected as short:    %llu (geometry did not fit the buffer)",
               (unsigned long long)bridge.Mailbox().RejectedFrames());
    AppendLine(text, L"  Dropped as stale:     %llu (mailbox overwrites)",
               (unsigned long long)bridge.Mailbox().StaleDrops());
    AppendLine(text, L"  Mailbox depth:        1 of 3 buffers (latest-frame, never queued)");
    AppendLine(text, L"  Frame ceiling:        %ls",
               settings.maxFps == 0 ? L"Match source" : std::to_wstring(settings.maxFps).c_str());
    AppendLine(text, L"");

    AppendLine(text, L"Timing");
    AppendLine(text, L"  Capture -> present:   %.2f ms (peak %.2f ms)", m_frameAgeMs, m_peakFrameAgeMs);
    AppendLine(text, L"  PipeWire receive lag: %.2f ms", m_captureLatencyMs);
    AppendLine(text, L"  PipeWire callback:    %.3f ms", stats.callback_ms);
    AppendLine(text, L"  Mailbox channel swap: %.3f ms", bridge.Mailbox().SwizzleMs());
    AppendLine(text, L"  Texture upload:       %.3f ms", timings.uploadMs);
    AppendLine(text, L"  Draw:                 %.3f ms", timings.drawMs);
    AppendLine(text, L"  Present:              %.3f ms", timings.presentMs);
    AppendLine(text, L"");

    AppendLine(text, L"Renderer");
    AppendLine(text, L"  Direct3D API:         D3D11");
    AppendLine(text, L"  GPU:                  %ls",
               adapter.description.empty() ? L"-" : adapter.description.c_str());
    AppendLine(text, L"  Vendor / device:      %ls  %04x:%04x", adapter.VendorName(), adapter.vendorId,
               adapter.deviceId);
    AppendLine(text, L"  Dedicated VRAM:       %llu MB",
               (unsigned long long)(adapter.dedicatedVideoMemory / (1024ull * 1024ull)));
    AppendLine(text, L"  Adapter LUID:         %08lx:%08lx", (unsigned long)adapter.luid.HighPart,
               (unsigned long)adapter.luid.LowPart);
    AppendLine(text, L"  Adapter index:        %d%ls", renderer.AdapterIndex(),
               settings.adapterIndex < 0 ? L"  (automatic)" : L"  (explicit)");
    AppendLine(text, L"  Pixel path:           %ls", pixelPathText);
    AppendLine(text, L"  Present path:         %ls",
               renderer.UsesShaderPath() ? L"Fullscreen triangle" : L"Copy blit (no compiler)");
    AppendLine(text, L"  Display mode:         %ls", modeText);
    AppendLine(text, L"  V-Sync:               %ls", settings.vsync ? L"On" : L"Off");
    AppendLine(text, L"  Presented FPS:        %.1f", m_presentedFps);
    AppendLine(text, L"");

    AppendLine(text, L"Output");
    AppendLine(text, L"  Gameplay output:      %ls", context.gameplayOutput ? L"On" : L"Off");
    AppendLine(text, L"  Interaction mode:     %ls",
               context.interaction == InteractionMode::Gameplay ? L"Gameplay (click-through, non-activating)"
                                                                : L"Configuration (interactive)");
    AppendLine(text, L"  Target monitor:       %ls",
               settings.outputMonitorIndex < 0 ? L"follows the Prism window"
                                               : std::to_wstring(settings.outputMonitorIndex).c_str());
    AppendLine(text, L"");

    if(context.hotkeys)
    {
        const unsigned state = context.hotkeys->PortalState();
        AppendLine(text, L"Global shortcuts");
        AppendLine(text, L"  Portal state:         %ls", ShortcutStateName(state));
        AppendLine(text, L"  Detail:               %ls",
                   context.hotkeys->PortalStatusText().empty() ? L"-"
                                                               : context.hotkeys->PortalStatusText().c_str());
        AppendLine(text, L"  Bound:                %ls", context.hotkeys->BindingsText().c_str());
        AppendLine(text, L"  Requested:            %ls / %ls", settings.hotkeyToggleMode.c_str(),
                   settings.hotkeyHideOutput.c_str());
        AppendLine(text, L"  RegisterHotKey:       %ls",
                   context.hotkeys->FallbackArmed() ? L"armed (focus-bound fallback)" : L"not needed");
        AppendLine(text, L"");
    }

    AppendLine(text, L"ReShade");
    AppendLine(text, L"  Loaded:               %ls", reshade.loaded ? L"Yes" : L"No");
    AppendLine(text, L"  Proxy:                %ls", reshade.proxyName.empty() ? L"-" : reshade.proxyName.c_str());
    AppendLine(text, L"  Path:                 %ls",
               reshade.modulePath.empty() ? L"-" : reshade.modulePath.c_str());
    AppendLine(text, L"  Add-on API:           %ls", reshade.addonApi ? L"Yes" : L"No");
    AppendLine(text, L"");

    /* The Linux side. DXGI knows none of this: no PCI address, no kernel
     * driver, and no idea what the PCIe link actually trained at. */
    PrismSystemInfo system {};
    if(bridge.QuerySystemInfo(system))
    {
        AppendLine(text, L"System (via the capture bridge)");
        AppendLine(text, L"  Session:              %ls on %ls", Utf8ToWideText(system.session_type).c_str(),
                   Utf8ToWideText(system.desktop).c_str());
        AppendLine(text, L"  Portal app id:        %ls", Utf8ToWideText(system.app_id).c_str());
        AppendLine(text, L"  GPU selection env:    %ls", Utf8ToWideText(system.gpu_env).c_str());
        AppendLine(text, L"  GPUs detected:        %u", system.gpu_count);

        for(unsigned i = 0; i < system.gpu_count; ++i)
        {
            const PrismGpuInfo& gpu    = system.gpus[i];
            const bool          inUse  = (gpu.vendor_id == adapter.vendorId && gpu.device_id == adapter.deviceId);
            const bool          narrow = gpu.link_width_max != 0 && gpu.link_width_cur < gpu.link_width_max;

            AppendLine(text, L"    [%u] %ls", i, Utf8ToWideText(gpu.name).c_str());
            AppendLine(text, L"         PCI %ls  driver %ls  %ls / %ls",
                       Utf8ToWideText(gpu.pci_address).c_str(), Utf8ToWideText(gpu.driver).c_str(),
                       Utf8ToWideText(gpu.drm_card).c_str(), Utf8ToWideText(gpu.drm_render).c_str());
            AppendLine(text, L"         by-path: %ls", Utf8ToWideText(gpu.by_path).c_str());
            AppendLine(text, L"         PCIe link: %ls x%u  (max %ls x%u)%ls",
                       Utf8ToWideText(gpu.link_speed_cur).c_str(), gpu.link_width_cur,
                       Utf8ToWideText(gpu.link_speed_max).c_str(), gpu.link_width_max,
                       narrow ? L"   <-- trained below its maximum" : L"");
            AppendLine(text, L"         Used by Prism: %ls%ls", inUse ? L"Yes" : L"No",
                       gpu.boot_vga ? L"   (firmware boot GPU)" : L"");
        }
        AppendLine(text, L"");
    }

    if(!renderer.LastError().empty())
        AppendLine(text, L"Notes\r\n  %ls", renderer.LastError().c_str());
    if(!bridge.IsLoaded() && !bridge.LoadError().empty())
        AppendLine(text, L"Notes\r\n  %ls", bridge.LoadError().c_str());
    if(bridge.IsLoaded() && !bridge.CaptureAvailable() && !bridge.CaptureError().empty())
        AppendLine(text, L"Notes\r\n  %ls", bridge.CaptureError().c_str());
    if(context.testPattern)
        AppendLine(text, L"Notes\r\n  Synthetic test pattern is running (--test-pattern); "
                         L"no application is being captured.");

    return text;
}
