/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Prism captures a Linux window, monitor or desktop through the XDG ScreenCast
 * portal and PipeWire, presents it through Direct3D 11 while running under
 * Proton/Wine, and lets ReShade process that presentation inside Prism.exe.
 *
 * It does not record, encode, stream, or touch the captured application in any
 * way. The only channel to the source is KWin -> portal -> PipeWire.
 */

#include "App.h"

#include <cstdarg>

const std::wstring& PrismModuleDirectory()
{
    static const std::wstring directory = [] {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring full(path);
        const size_t slash = full.find_last_of(L'\\');
        return slash == std::wstring::npos ? std::wstring(L".\\") : full.substr(0, slash + 1);
    }();
    return directory;
}

void PrismLog(const char* format, ...)
{
    /* stderr rather than OutputDebugString: under Wine it lands in the same
     * terminal as the bridge's own logging, which is where anyone debugging a
     * capture problem is already looking. */
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[Prism] ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(args);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int showCommand)
{
    (void)previous;
    (void)commandLine;

    /* Prism is a single-instance tray application: a second copy would open a
     * second portal session and fight for the same ReShade configuration. */
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\PrismCaptureHostSingleInstance");
    if(mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND existing = FindWindowW(L"PrismOutputWindow", nullptr);
        if(existing)
        {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return 0;
    }

    int result = 1;
    {
        App app;
        if(app.Initialize(instance, showCommand))
            result = app.Run();
        app.Shutdown();
    }

    if(mutex)
    {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return result;
}
