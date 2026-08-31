/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Settings.h"

namespace
{
const wchar_t kSection[] = L"Prism";

std::wstring IniPath()
{
    return PrismModuleDirectory() + L"Prism.ini";
}

int ReadInt(const wchar_t* key, int fallback)
{
    return static_cast<int>(GetPrivateProfileIntW(kSection, key, fallback, IniPath().c_str()));
}

void WriteInt(const wchar_t* key, int value)
{
    wchar_t buffer[32];
    swprintf(buffer, 32, L"%d", value);
    WritePrivateProfileStringW(kSection, key, buffer, IniPath().c_str());
}

template<typename E> E ClampEnum(int value, int count, E fallback)
{
    return (value >= 0 && value < count) ? static_cast<E>(value) : fallback;
}
} // namespace

void Settings::Load()
{
    displayMode = ClampEnum<DisplayMode>(ReadInt(L"DisplayMode", 1), 4, DisplayMode::Fit);
    windowMode  = ClampEnum<WindowMode>(ReadInt(L"WindowMode", 0), 3, WindowMode::Windowed);

    const int fps = ReadInt(L"MaxFps", 0);
    maxFps        = 0;
    for(unsigned candidate : kFpsCeilings)
    {
        if(static_cast<int>(candidate) == fps)
        {
            maxFps = candidate;
            break;
        }
    }

    vsync           = ReadInt(L"VSync", 0) != 0;
    captureCursor   = ReadInt(L"CaptureCursor", 0) != 0;
    sourceTypes     = static_cast<unsigned>(ReadInt(L"SourceTypes", 0x7));
    adapterIndex    = ReadInt(L"AdapterIndex", -1);
    windowWidth     = ReadInt(L"WindowWidth", 1280);
    windowHeight    = ReadInt(L"WindowHeight", 720);
    showDiagnostics = ReadInt(L"ShowDiagnostics", 0) != 0;

    if(sourceTypes == 0 || sourceTypes > 0x7)
        sourceTypes = 0x7;
    if(windowWidth < 320)
        windowWidth = 320;
    if(windowHeight < 240)
        windowHeight = 240;
}

void Settings::Save() const
{
    WriteInt(L"DisplayMode", static_cast<int>(displayMode));
    WriteInt(L"WindowMode", static_cast<int>(windowMode));
    WriteInt(L"MaxFps", static_cast<int>(maxFps));
    WriteInt(L"VSync", vsync ? 1 : 0);
    WriteInt(L"CaptureCursor", captureCursor ? 1 : 0);
    WriteInt(L"SourceTypes", static_cast<int>(sourceTypes));
    WriteInt(L"AdapterIndex", adapterIndex);
    WriteInt(L"WindowWidth", windowWidth);
    WriteInt(L"WindowHeight", windowHeight);
    WriteInt(L"ShowDiagnostics", showDiagnostics ? 1 : 0);
}
