/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "CaptureBridge.h"
#include "Common.h"
#include "Diagnostics.h"
#include "Hotkeys.h"
#include "Renderer.h"
#include "Settings.h"
#include "TestPattern.h"

class App
{
public:
    bool Initialize(HINSTANCE instance, int showCommand);
    int  Run();
    void Shutdown();

private:
    static LRESULT CALLBACK OutputWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK DiagnosticsWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void    OnCommand(int commandId);

    bool CreateOutputWindow(int showCommand);
    void BuildMenus();
    void RefreshMenuChecks();

    bool IsCapturing() const;
    void ParseCommandLine();
    void StartCapture();
    void StopCapture();

    void ApplyWindowMode(WindowMode mode);
    void ShowOutput(bool visible);

    /* Gameplay output: fullscreen on the chosen monitor, on top of the game. */
    void EnterGameplayOutput();
    void ExitGameplayOutput();
    void SetInteractionMode(InteractionMode mode);
    void ApplyInteractionStyles();
    void OnShortcut(PrismShortcut shortcut);

    void                    RefreshMonitors();
    RECT                    TargetMonitorRect() const;
    static BOOL CALLBACK    MonitorEnumThunk(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM userData);

    void CreateTrayIcon();
    void UpdateTrayIcon();
    void DestroyTrayIcon();
    void ShowTrayMenu();

    void ToggleDiagnostics(bool visible);
    void UpdateDiagnostics();
    Diagnostics::ReportContext BuildReportContext() const;
    void                       SaveDiagnosticsReport();

    void PumpMessages();
    void RenderOnce();
    void WaitForWork();

    HINSTANCE m_instance = nullptr;
    HWND      m_window   = nullptr;
    HMENU     m_menuBar  = nullptr;
    HWND      m_diagnosticsWindow = nullptr;
    HWND      m_diagnosticsEdit   = nullptr;

    Settings      m_settings;
    Renderer      m_renderer;
    CaptureBridge m_bridge;
    Diagnostics   m_diagnostics;
    ReShadeInfo   m_reshade;
    TestPatternSource m_testPattern;
    Hotkeys           m_hotkeys;

    std::vector<AdapterInfo> m_adapters;

    struct MonitorEntry
    {
        RECT         bounds {};
        std::wstring name;
        bool         primary = false;
    };
    std::vector<MonitorEntry> m_monitors;

    InteractionMode m_interaction    = InteractionMode::Configuration;
    bool            m_gameplayOutput = false;
    HWND            m_previousForeground = nullptr;

    bool     m_running        = true;
    bool     m_outputVisible  = true;
    bool     m_trayIconAdded  = false;
    bool     m_testPatternRequested = false;
    bool     m_gameplayRequested    = false;
    bool     m_dumpDiagnostics      = false;
    uint64_t m_lastPresentUs  = 0;

    WINDOWPLACEMENT m_savedPlacement {};
    LONG_PTR        m_savedStyle = 0;
};
