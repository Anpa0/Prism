/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "CaptureBridge.h"

static std::wstring Utf8ToWide(const char* text)
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

CaptureBridge::~CaptureBridge()
{
    Shutdown();
}

bool CaptureBridge::Load()
{
    if(m_ready)
        return true;
    if(m_module)
        return false; /* mapped but unusable; the error is already recorded */

    /* Load by full path so a stray PrismCapture.dll elsewhere on the search
     * path can never be picked up, and suppress the Wine/Windows error box on
     * failure - a missing bridge is an expected state, not a crash. */
    const std::wstring path = PrismModuleDirectory() + PRISM_CAPTURE_MODULE;

    DWORD previousMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previousMode);
    m_module = LoadLibraryW(path.c_str());
    SetThreadErrorMode(previousMode, nullptr);

    if(!m_module)
    {
        m_loadError = L"PrismCapture.dll could not be loaded. Prism must run under Wine or Proton, "
                      L"and the bridge must sit next to Prism.exe.";
        PrismLog("bridge: LoadLibraryW failed (error %lu)", GetLastError());
        return false;
    }

    m_version   = PrismResolveProc<VersionFn>(m_module, "PrismCaptureVersion");
    m_init      = PrismResolveProc<InitFn>(m_module, "PrismCaptureInit");
    m_start     = PrismResolveProc<StartFn>(m_module, "PrismCaptureStart");
    m_stop      = PrismResolveProc<StopFn>(m_module, "PrismCaptureStop");
    m_setMaxFps = PrismResolveProc<SetMaxFpsFn>(m_module, "PrismCaptureSetMaxFps");
    m_getStats  = PrismResolveProc<GetStatsFn>(m_module, "PrismCaptureGetStats");
    m_shutdown  = PrismResolveProc<ShutdownFn>(m_module, "PrismCaptureShutdown");

    if(!m_version || !m_init || !m_start || !m_stop || !m_setMaxFps || !m_getStats || !m_shutdown)
    {
        m_loadError = L"PrismCapture.dll is missing exports - it does not match this build of Prism.exe.";
        FreeLibrary(m_module); /* safe: nothing in the bridge has run yet */
        m_module = nullptr;
        return false;
    }

    const unsigned abi = m_version();
    if(abi != PRISM_CAPTURE_ABI_VERSION)
    {
        wchar_t buffer[192];
        swprintf(buffer, 192, L"PrismCapture.dll reports ABI %u but Prism.exe expects %u. Rebuild both.", abi,
                 PRISM_CAPTURE_ABI_VERSION);
        m_loadError = buffer;
        FreeLibrary(m_module); /* safe: nothing in the bridge has run yet */
        m_module = nullptr;
        return false;
    }

    if(m_init() != S_OK)
    {
        m_loadError = L"The XDG ScreenCast portal is not available in this session. "
                      L"Check that xdg-desktop-portal and its KDE backend are running.";
        /* Deliberately not unloaded. PrismCaptureInit() reaches GLib, and GDBus
         * starts a worker thread the moment it touches the session bus; pulling
         * the shared object out from under that thread faults the process. The
         * module stays mapped and inert, and m_ready keeps Prism from using it. */
        m_ready = false;
        return false;
    }

    m_ready = true;
    m_loadError.clear();
    PrismLog("bridge: PrismCapture.dll loaded, ABI %u", abi);
    return true;
}

void CaptureBridge::SetStatusWindow(HWND window, UINT message)
{
    m_statusWindow    = window;
    m_statusMessageId = message;
}

void __stdcall CaptureBridge::OnFrameThunk(const PrismFrame* frame, void* context)
{
    static_cast<CaptureBridge*>(context)->OnFrame(frame);
}

void __stdcall CaptureBridge::OnStatusThunk(unsigned state, const char* message, void* context)
{
    static_cast<CaptureBridge*>(context)->OnStatus(state, message);
}

void CaptureBridge::OnFrame(const PrismFrame* frame)
{
    if(!frame || !m_active.load(std::memory_order_relaxed))
        return;

    /* The only work done on the capture thread: one copy into the mailbox.
     * Direct3D is never touched from here, which keeps the device free of
     * cross-thread traffic and keeps ReShade's view of the renderer simple. */
    m_mailbox.Publish(frame->data, frame->width, frame->height, frame->pitch, frame->format, frame->sequence,
                      frame->pts_ns, frame->recv_ns);
}

void CaptureBridge::OnStatus(unsigned state, const char* message)
{
    m_state.store(state, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_statusMessage = Utf8ToWide(message);
    }
    PrismLog("bridge status: state=%u %s", state, message ? message : "");

    if(state == PRISM_STATE_IDLE || state == PRISM_STATE_ERROR)
        m_active.store(false, std::memory_order_relaxed);

    if(m_statusWindow && m_statusMessageId)
        PostMessageW(m_statusWindow, m_statusMessageId, static_cast<WPARAM>(state), 0);
}

bool CaptureBridge::Start(unsigned sourceTypes, bool captureCursor, unsigned maxFps)
{
    if(!m_ready || m_active.load(std::memory_order_relaxed))
        return false;

    m_mailbox.ResetCounters();
    m_active.store(true, std::memory_order_relaxed);
    m_state.store(PRISM_STATE_NEGOTIATING, std::memory_order_relaxed);

    const unsigned cursor = captureCursor ? PRISM_CURSOR_EMBEDDED : PRISM_CURSOR_HIDDEN;
    if(m_start(sourceTypes, cursor, maxFps, &CaptureBridge::OnFrameThunk, &CaptureBridge::OnStatusThunk, this) !=
       S_OK)
    {
        m_active.store(false, std::memory_order_relaxed);
        m_state.store(PRISM_STATE_ERROR, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void CaptureBridge::Stop()
{
    if(!m_ready)
        return;
    m_active.store(false, std::memory_order_relaxed);
    m_stop();
    m_state.store(PRISM_STATE_IDLE, std::memory_order_relaxed);
}

void CaptureBridge::SetMaxFps(unsigned maxFps)
{
    if(m_ready)
        m_setMaxFps(maxFps);
}

void CaptureBridge::Shutdown()
{
    if(!m_module)
        return;
    m_active.store(false, std::memory_order_relaxed);
    if(m_ready)
        m_shutdown();
    m_ready = false;
    /* Same reasoning as in Load(): the bridge is left mapped. It owns a GLib
     * main loop and a GDBus worker, and the process is on its way out anyway. */
    m_module = nullptr;
}

PrismBridgeStats CaptureBridge::Stats() const
{
    PrismBridgeStats stats {};
    if(m_ready && m_getStats)
        m_getStats(&stats);
    return stats;
}

std::wstring CaptureBridge::StatusMessage() const
{
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_statusMessage;
}
