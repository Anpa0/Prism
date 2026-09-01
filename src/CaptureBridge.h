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

    /* Maps the module and resolves its exports. Succeeding here is enough for
     * global shortcuts and system introspection; capture additionally needs the
     * ScreenCast portal, which CaptureAvailable() reports separately. Keeping
     * the two apart matters: a session with no ScreenCast backend should still
     * get working hotkeys. */
    bool Load();
    bool IsLoaded() const { return m_loaded; }
    bool CaptureAvailable() const { return m_captureReady; }

    const std::wstring& LoadError() const { return m_loadError; }
    const std::wstring& CaptureError() const { return m_captureError; }

    bool Start(unsigned sourceTypes, bool captureCursor, unsigned maxFps);
    void Stop();
    void SetMaxFps(unsigned maxFps);
    void Shutdown();

    bool IsActive() const { return m_active.load(std::memory_order_relaxed); }

    FrameMailbox&       Mailbox() { return m_mailbox; }
    const FrameMailbox& Mailbox() const { return m_mailbox; }

    PrismBridgeStats Stats() const;

    /* Global shortcuts, forwarded to the XDG GlobalShortcuts portal. */
    bool         StartShortcuts(const PrismShortcutSpec* shortcuts, unsigned count,
                                PrismShortcutCallback callback, void* context);
    void         StopShortcuts();
    void         ConfigureShortcuts();
    unsigned     ShortcutState(char* message, unsigned messageBytes) const;
    unsigned     ShortcutBindings(PrismShortcutBinding* out, unsigned max) const;

    /* Linux GPU and session facts Prism.exe cannot see through DXGI. */
    bool QuerySystemInfo(PrismSystemInfo& out) const;

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
    using ShortcutsStartFn       = long(__stdcall*)(const PrismShortcutSpec*, unsigned, PrismShortcutCallback,
                                             void*);
    using ShortcutsStatusFn      = unsigned(__stdcall*)(char*, unsigned);
    using ShortcutsBindingsFn    = unsigned(__stdcall*)(PrismShortcutBinding*, unsigned);
    using ShortcutsConfigureFn   = void(__stdcall*)();
    using ShortcutsStopFn        = void(__stdcall*)();
    using SystemInfoQueryFn      = void(__stdcall*)(PrismSystemInfo*);

    HMODULE      m_module       = nullptr;
    bool         m_loaded       = false;
    bool         m_captureReady = false;
    std::wstring m_loadError;
    std::wstring m_captureError;

    VersionFn   m_version   = nullptr;
    InitFn      m_init      = nullptr;
    StartFn     m_start     = nullptr;
    StopFn      m_stop      = nullptr;
    SetMaxFpsFn m_setMaxFps = nullptr;
    GetStatsFn  m_getStats  = nullptr;
    ShutdownFn  m_shutdown  = nullptr;

    ShortcutsStartFn     m_shortcutsStart     = nullptr;
    ShortcutsStatusFn    m_shortcutsStatus    = nullptr;
    ShortcutsBindingsFn  m_shortcutsBindings  = nullptr;
    ShortcutsConfigureFn m_shortcutsConfigure = nullptr;
    ShortcutsStopFn      m_shortcutsStop      = nullptr;
    SystemInfoQueryFn    m_systemInfoQuery    = nullptr;

    FrameMailbox          m_mailbox;
    std::atomic<bool>     m_active {false};
    std::atomic<unsigned> m_state {PRISM_STATE_IDLE};

    mutable std::mutex m_statusMutex;
    std::wstring       m_statusMessage;

    HWND m_statusWindow  = nullptr;
    UINT m_statusMessageId = 0;
};
