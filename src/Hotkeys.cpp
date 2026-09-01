/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Hotkeys.h"
#include "CaptureBridge.h"

#include <algorithm>

namespace
{
/* RegisterHotKey ids; arbitrary but must not collide with anything else Prism
 * registers, and Prism registers nothing else. */
constexpr int kFallbackIdBase = 0xB100;

std::wstring Utf8ToWide(const char* text)
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

std::string WideToUtf8(const std::wstring& text)
{
    if(text.empty())
        return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if(needed <= 1)
        return std::string();
    std::string result(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), needed, nullptr, nullptr);
    return result;
}
} // namespace

const char* PrismShortcutId(PrismShortcut shortcut)
{
    switch(shortcut)
    {
    case PrismShortcut::ToggleMode: return "toggle-mode";
    case PrismShortcut::HideOutput: return "hide-output";
    default: return "";
    }
}

Hotkeys::~Hotkeys()
{
    Stop();
}

bool Hotkeys::ParseTrigger(const std::wstring& trigger, UINT& modifiers, UINT& virtualKey)
{
    modifiers  = 0;
    virtualKey = 0;

    std::wstring token;
    auto         consume = [&]() -> bool {
        if(token.empty())
            return true;

        std::wstring upper = token;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t c) { return towupper(c); });

        if(upper == L"CTRL" || upper == L"CONTROL")
            modifiers |= MOD_CONTROL;
        else if(upper == L"SHIFT")
            modifiers |= MOD_SHIFT;
        else if(upper == L"ALT")
            modifiers |= MOD_ALT;
        else if(upper == L"LOGO" || upper == L"SUPER" || upper == L"WIN" || upper == L"META")
            modifiers |= MOD_WIN;
        else if(upper.size() >= 2 && upper[0] == L'F' && iswdigit(upper[1]))
        {
            const int number = _wtoi(upper.c_str() + 1);
            if(number < 1 || number > 24)
                return false;
            virtualKey = VK_F1 + static_cast<UINT>(number - 1);
        }
        else if(upper.size() == 1 && (iswalnum(upper[0])))
        {
            virtualKey = static_cast<UINT>(upper[0]);
        }
        else
        {
            return false;
        }
        token.clear();
        return true;
    };

    for(wchar_t c : trigger)
    {
        if(c == L'+')
        {
            if(!consume())
                return false;
        }
        else if(!iswspace(c))
        {
            token.push_back(c);
        }
    }
    if(!consume())
        return false;

    return virtualKey != 0;
}

void __stdcall Hotkeys::OnActivatedThunk(const char* shortcutId, void* context)
{
    static_cast<Hotkeys*>(context)->OnActivated(shortcutId);
}

void Hotkeys::OnActivated(const char* shortcutId)
{
    /* Runs on the bridge's capture thread. Everything the shortcut does touches
     * windows and Direct3D, so it is handed to the UI thread untouched. */
    for(size_t i = 0; i < static_cast<size_t>(PrismShortcut::Count); ++i)
    {
        if(strcmp(shortcutId, PrismShortcutId(static_cast<PrismShortcut>(i))) == 0)
        {
            if(m_window && m_message)
                PostMessageW(m_window, m_message, static_cast<WPARAM>(i), 0);
            return;
        }
    }
    PrismLog("hotkeys: ignoring an unknown shortcut id '%s'", shortcutId);
}

void Hotkeys::Start(CaptureBridge& bridge, HWND window, UINT message, const std::wstring& toggleTrigger,
                    const std::wstring& hideTrigger)
{
    m_bridge  = &bridge;
    m_window  = window;
    m_message = message;

    m_triggers[static_cast<size_t>(PrismShortcut::ToggleMode)] = toggleTrigger;
    m_triggers[static_cast<size_t>(PrismShortcut::HideOutput)] = hideTrigger;

    const std::string toggleUtf8 = WideToUtf8(toggleTrigger);
    const std::string hideUtf8   = WideToUtf8(hideTrigger);

    const PrismShortcutSpec specs[] = {
        {PrismShortcutId(PrismShortcut::ToggleMode), "Prism: switch between gameplay and configuration mode",
         toggleUtf8.c_str()},
        {PrismShortcutId(PrismShortcut::HideOutput), "Prism: hide the gameplay output", hideUtf8.c_str()},
    };

    m_started = bridge.StartShortcuts(specs, static_cast<unsigned>(std::size(specs)), &Hotkeys::OnActivatedThunk,
                                      this);
    if(!m_started)
    {
        PrismLog("hotkeys: no bridge, falling back to RegisterHotKey immediately");
        ArmFallback();
    }
}

void Hotkeys::Stop()
{
    DisarmFallback();
    if(m_started && m_bridge)
        m_bridge->StopShortcuts();
    m_started = false;
    m_bridge  = nullptr;
}

unsigned Hotkeys::PortalState() const
{
    if(!m_started || !m_bridge)
        return PRISM_SHORTCUTS_UNAVAILABLE;
    return m_bridge->ShortcutState(nullptr, 0);
}

std::wstring Hotkeys::PortalStatusText() const
{
    if(!m_started || !m_bridge)
        return L"not started";

    char message[512] = {};
    m_bridge->ShortcutState(message, sizeof(message));
    return Utf8ToWide(message);
}

std::wstring Hotkeys::BindingsText() const
{
    if(!m_bridge)
        return L"-";

    PrismShortcutBinding bindings[PRISM_SHORTCUT_MAX] = {};
    const unsigned       count = m_bridge->ShortcutBindings(bindings, PRISM_SHORTCUT_MAX);
    if(count == 0)
        return L"-";

    std::wstring text;
    for(unsigned i = 0; i < count; ++i)
    {
        if(i)
            text += L", ";
        text += Utf8ToWide(bindings[i].id) + L" = " + Utf8ToWide(bindings[i].trigger);
    }
    return text;
}

void Hotkeys::Configure()
{
    if(m_bridge)
        m_bridge->ConfigureShortcuts();
}

void Hotkeys::Poll()
{
    const unsigned state = PortalState();

    /* The portal answer is asynchronous, so the fallback is armed only once the
     * compositor has actually said no. Arming both would double every press. */
    const bool portalFailed =
        (state == PRISM_SHORTCUTS_UNAVAILABLE || state == PRISM_SHORTCUTS_DENIED || state == PRISM_SHORTCUTS_ERROR);

    if(portalFailed && !m_fallbackArmed)
        ArmFallback();
    else if(state == PRISM_SHORTCUTS_BOUND && m_fallbackArmed)
        DisarmFallback();
}

void Hotkeys::ArmFallback()
{
    if(m_fallbackArmed || !m_window)
        return;

    bool any = false;
    for(size_t i = 0; i < static_cast<size_t>(PrismShortcut::Count); ++i)
    {
        UINT modifiers = 0;
        UINT virtualKey = 0;
        if(!ParseTrigger(m_triggers[i], modifiers, virtualKey))
        {
            PrismLog("hotkeys: cannot parse trigger '%ls'", m_triggers[i].c_str());
            continue;
        }
        if(RegisterHotKey(m_window, kFallbackIdBase + static_cast<int>(i), modifiers | MOD_NOREPEAT, virtualKey))
            any = true;
        else
            PrismLog("hotkeys: RegisterHotKey failed for '%ls' (error %lu)", m_triggers[i].c_str(),
                     GetLastError());
    }

    m_fallbackArmed = any;
    if(any)
        PrismLog("hotkeys: RegisterHotKey fallback armed. On a Wayland session these only reach Prism while it "
                 "has focus - install the .desktop file so the portal can bind them properly.");
}

void Hotkeys::DisarmFallback()
{
    if(!m_fallbackArmed || !m_window)
        return;
    for(size_t i = 0; i < static_cast<size_t>(PrismShortcut::Count); ++i)
        UnregisterHotKey(m_window, kFallbackIdBase + static_cast<int>(i));
    m_fallbackArmed = false;
}
