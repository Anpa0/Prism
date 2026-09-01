/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "CaptureBridge.h"
#include "Common.h"
#include "Diagnostics.h"
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

    void CreateTrayIcon();
    void UpdateTrayIcon();
    void DestroyTrayIcon();
    void ShowTrayMenu();

    void ToggleDiagnostics(bool visible);
    void UpdateDiagnostics();

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

    std::vector<AdapterInfo> m_adapters;

    bool     m_running        = true;
    bool     m_outputVisible  = true;
    bool     m_trayIconAdded  = false;
    bool     m_testPatternRequested = false;
    uint64_t m_lastPresentUs  = 0;

    WINDOWPLACEMENT m_savedPlacement {};
    LONG_PTR        m_savedStyle = 0;
};
