/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Common.h"
#include "PrismCapture.h"

class CaptureBridge;

/* Prism's two global actions. The IDs are the strings the compositor stores
 * against our app id, so they must stay stable across releases. */
enum class PrismShortcut
{
    ToggleMode = 0, /* gameplay <-> configuration */
    HideOutput = 1, /* drop the gameplay overlay immediately */
    Count      = 2,
};

const char* PrismShortcutId(PrismShortcut shortcut);

/*
 * Global hotkeys, the Wayland way.
 *
 * Under Wayland the compositor owns the keyboard: an application cannot grab a
 * key, and there is no X11 path worth taking. The only sanctioned mechanism is
 * the XDG GlobalShortcuts portal, which the bridge drives on Prism's behalf and
 * which KWin surfaces as a one-time approval dialog.
 *
 * RegisterHotKey remains as a fallback for the cases the portal cannot serve -
 * a session whose portal has no GlobalShortcuts backend, or Prism running on
 * X11/XWayland where Wine's key handling still works. It is armed only after
 * the portal has definitively failed, so a bound shortcut never fires twice.
 */
class Hotkeys
{
public:
    ~Hotkeys();

    /* `message` is posted to `window` with the PrismShortcut index in wParam. */
    void Start(CaptureBridge& bridge, HWND window, UINT message, const std::wstring& toggleTrigger,
               const std::wstring& hideTrigger);
    void Stop();

    /* Called from the UI timer: promotes the Win32 fallback if the portal has
     * given up, and retires it if the portal later succeeds. */
    void Poll();

    /* Asks the compositor to show its own shortcut editor for our session. */
    void Configure();

    unsigned     PortalState() const;
    std::wstring PortalStatusText() const;
    std::wstring BindingsText() const;
    bool         FallbackArmed() const { return m_fallbackArmed; }

    /* Parses "CTRL+SHIFT+F11" into RegisterHotKey arguments. Returns false for
     * anything it does not recognise. */
    static bool ParseTrigger(const std::wstring& trigger, UINT& modifiers, UINT& virtualKey);

private:
    static void __stdcall OnActivatedThunk(const char* shortcutId, void* context);
    void                  OnActivated(const char* shortcutId);

    void ArmFallback();
    void DisarmFallback();

    CaptureBridge* m_bridge  = nullptr;
    HWND           m_window  = nullptr;
    UINT           m_message = 0;

    std::wstring m_triggers[static_cast<size_t>(PrismShortcut::Count)];
    bool         m_started       = false;
    bool         m_fallbackArmed = false;
};
