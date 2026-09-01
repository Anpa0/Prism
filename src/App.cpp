/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "App.h"

#include <algorithm>

namespace
{
const wchar_t kOutputClass[]      = L"PrismOutputWindow";
const wchar_t kDiagnosticsClass[] = L"PrismDiagnosticsWindow";

/* ReShade keys off the window title in a few of its presets, and the title is
 * also the cheapest always-visible status readout, so it carries the summary. */
const wchar_t kBaseTitle[] = L"Prism";

constexpr UINT WM_PRISM_TRAY     = WM_APP + 1;
constexpr UINT WM_PRISM_STATUS   = WM_APP + 2;
constexpr UINT WM_PRISM_SHORTCUT = WM_APP + 3;

constexpr UINT_PTR kStatusTimerId = 1;
constexpr UINT_PTR kDumpTimerId   = 2;

enum MenuId : int
{
    IdSelectSource = 100,
    IdStopCapture,
    IdCaptureCursor,
    IdExit,
    IdShowOutput,
    IdHideOutput,
    IdDiagnostics,
    IdAbout,
    IdVSync,

    IdGameplayOutput = 110,
    IdExitGameplay,
    IdToggleInteraction,
    IdConfigureShortcuts,
    IdShowPrism,
    IdSaveReport,

    IdDisplayModeBase = 200, /* +DisplayMode */
    IdWindowModeBase  = 220, /* +WindowMode  */
    IdFpsBase         = 300, /* +index into kFpsCeilings */
    IdSourceTypeBase  = 400, /* +0 any, +1 monitor only, +2 window only   */
    IdAdapterBase     = 500, /* +adapter index, IdAdapterBase-1 = default */
    IdMonitorBase     = 600, /* +monitor index, IdMonitorBase-1 = follow window */
};

/* How long Prism will sit on the same image before presenting it again. Keeps
 * ReShade's animated effects running and repaints after an occlusion without
 * turning the idle case into a spin. */
constexpr uint64_t kIdleRepresentUs = 100000;

std::wstring FormatFpsLabel(unsigned fps)
{
    if(fps == 0)
        return L"Unlimited / Match Source";
    return std::to_wstring(fps) + L" FPS";
}
} // namespace

/* ------------------------------------------------------------- lifetime -- */

bool App::Initialize(HINSTANCE instance, int showCommand)
{
    m_instance = instance;
    m_settings.Load();
    ParseCommandLine();
    m_adapters = Renderer::EnumerateAdapters();
    RefreshMonitors();

    if(!CreateOutputWindow(showCommand))
        return false;

    if(!m_renderer.Initialize(m_window, m_settings.adapterIndex))
    {
        MessageBoxW(m_window, m_renderer.LastError().c_str(), L"Prism - Direct3D", MB_ICONERROR | MB_OK);
        return false;
    }

    /* ReShade is detected after the device exists: its proxy DLL is pulled in
     * by the first DXGI or D3D11 call, not at process start. */
    m_reshade = DetectReShade();
    PrismLog("ReShade %s%ls%s", m_reshade.loaded ? "loaded via " : "not detected",
             m_reshade.loaded ? m_reshade.proxyName.c_str() : L"", m_reshade.addonApi ? " (add-on API)" : "");

    m_bridge.SetStatusWindow(m_window, WM_PRISM_STATUS);
    if(!m_bridge.Load())
        PrismLog("capture bridge unavailable: %ls", m_bridge.LoadError().c_str());
    else if(!m_bridge.CaptureAvailable())
        PrismLog("capture unavailable: %ls", m_bridge.CaptureError().c_str());

    BuildMenus();
    CreateTrayIcon();

    /* The window is created windowed, so let ApplyWindowMode see that as the
     * previous state and record a placement worth restoring later. */
    const WindowMode desiredWindowMode = m_settings.windowMode;
    m_settings.windowMode              = WindowMode::Windowed;
    ApplyWindowMode(desiredWindowMode);
    if(m_settings.showDiagnostics)
        ToggleDiagnostics(true);

    /* The mailbox only swaps channels when the renderer cannot: with shaders
     * available Prism picks a texture format that matches the source instead. */
    m_bridge.Mailbox().SetSwizzleRgbToBgr(!m_renderer.UsesShaderPath());

    /* Global hotkeys go through the XDG GlobalShortcuts portal, so they work
     * while the game has focus. This is the only Wayland-correct route. */
    m_hotkeys.Start(m_bridge, m_window, WM_PRISM_SHORTCUT, m_settings.hotkeyToggleMode,
                    m_settings.hotkeyHideOutput);

    if(m_testPatternRequested)
    {
        PrismLog("starting the synthetic test pattern instead of a capture session");
        m_testPattern.Start(m_bridge.Mailbox(), 1280, 720, m_settings.maxFps ? m_settings.maxFps : 60);
    }

    if(m_gameplayRequested)
        EnterGameplayOutput();

    /* --dump-diagnostics: give capture and the portal a few seconds to settle,
     * then write the report once. */
    if(m_dumpDiagnostics)
        SetTimer(m_window, kDumpTimerId, 10000, nullptr);

    SetTimer(m_window, kStatusTimerId, 500, nullptr);
    return true;
}

bool App::IsCapturing() const
{
    return m_bridge.IsActive() || m_testPattern.IsRunning();
}

void App::ParseCommandLine()
{
    int     count = 0;
    LPWSTR* argv  = CommandLineToArgvW(GetCommandLineW(), &count);
    if(!argv)
        return;

    for(int i = 1; i < count; ++i)
    {
        if(_wcsicmp(argv[i], L"--test-pattern") == 0)
            m_testPatternRequested = true;
        else if(_wcsicmp(argv[i], L"--diagnostics") == 0)
            m_settings.showDiagnostics = true;
        else if(_wcsicmp(argv[i], L"--dump-diagnostics") == 0)
        {
            m_dumpDiagnostics          = true;
            m_settings.showDiagnostics = true;
        }
        else if(_wcsicmp(argv[i], L"--gameplay") == 0)
            m_gameplayRequested = true;
        else if(_wcsnicmp(argv[i], L"--monitor=", 10) == 0)
            m_settings.outputMonitorIndex = _wtoi(argv[i] + 10);
    }
    LocalFree(argv);
}

void App::Shutdown()
{
    m_settings.Save();

    if(m_window)
        KillTimer(m_window, kStatusTimerId);

    m_hotkeys.Stop();
    m_testPattern.Stop();
    m_bridge.Stop();
    m_bridge.Shutdown();
    m_renderer.Shutdown();
    DestroyTrayIcon();

    if(m_diagnosticsWindow)
    {
        DestroyWindow(m_diagnosticsWindow);
        m_diagnosticsWindow = nullptr;
    }
}

/* --------------------------------------------------------------- window -- */

bool App::CreateOutputWindow(int showCommand)
{
    WNDCLASSEXW outputClass {};
    outputClass.cbSize        = sizeof(outputClass);
    outputClass.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    outputClass.lpfnWndProc   = &App::OutputWndProc;
    outputClass.hInstance     = m_instance;
    outputClass.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    outputClass.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    outputClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    outputClass.lpszClassName = kOutputClass;
    if(!RegisterClassExW(&outputClass))
        return false;

    WNDCLASSEXW diagnosticsClass {};
    diagnosticsClass.cbSize        = sizeof(diagnosticsClass);
    diagnosticsClass.lpfnWndProc   = &App::DiagnosticsWndProc;
    diagnosticsClass.hInstance     = m_instance;
    diagnosticsClass.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    diagnosticsClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    diagnosticsClass.lpszClassName = kDiagnosticsClass;
    RegisterClassExW(&diagnosticsClass);

    RECT desired {0, 0, m_settings.windowWidth, m_settings.windowHeight};
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, TRUE);

    m_window = CreateWindowExW(0, kOutputClass, kBaseTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               desired.right - desired.left, desired.bottom - desired.top, nullptr, nullptr,
                               m_instance, this);
    if(!m_window)
        return false;

    ShowWindow(m_window, showCommand);
    UpdateWindow(m_window);
    return true;
}

LRESULT CALLBACK App::OutputWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if(message == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self         = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if(self)
        return self->HandleMessage(window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK App::DiagnosticsWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_SIZE:
    {
        HWND edit = GetWindow(window, GW_CHILD);
        if(edit)
            MoveWindow(edit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
    case WM_SIZE:
        if(wParam != SIZE_MINIMIZED)
        {
            const UINT width  = LOWORD(lParam);
            const UINT height = HIWORD(lParam);
            m_renderer.OnResize(width, height);
            if(m_settings.windowMode == WindowMode::Windowed && width > 0 && height > 0)
            {
                m_settings.windowWidth  = static_cast<int>(width);
                m_settings.windowHeight = static_cast<int>(height);
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1; /* the renderer owns every pixel */

    case WM_PAINT:
    {
        PAINTSTRUCT paint;
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        m_lastPresentUs = 0; /* force a present so the exposed area is filled */
        return 0;
    }

    case WM_COMMAND:
        OnCommand(LOWORD(wParam));
        return 0;

    case WM_TIMER:
        if(wParam == kStatusTimerId)
        {
            SetWindowTextW(m_window, m_diagnostics.BuildSummary(IsCapturing(), m_testPattern.IsRunning(), m_gameplayOutput).c_str());
            m_hotkeys.Poll();
            UpdateTrayIcon();
            UpdateDiagnostics();
        }
        else if(wParam == kDumpTimerId)
        {
            KillTimer(m_window, kDumpTimerId);
            SaveDiagnosticsReport();
        }
        return 0;

    case WM_PRISM_STATUS:
        RefreshMenuChecks();
        UpdateTrayIcon();
        if(static_cast<unsigned>(wParam) == PRISM_STATE_ERROR)
            PrismLog("capture error: %ls", m_bridge.StatusMessage().c_str());
        return 0;

    case WM_PRISM_SHORTCUT:
        OnShortcut(static_cast<PrismShortcut>(wParam));
        return 0;

    case WM_HOTKEY:
        /* Only reached when the portal was unavailable and the RegisterHotKey
         * fallback is armed. */
        OnShortcut(static_cast<PrismShortcut>(wParam - 0xB100));
        return 0;

    case WM_PRISM_TRAY:
        if(LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
            ShowTrayMenu();
        else if(LOWORD(lParam) == WM_LBUTTONDBLCLK)
            ShowOutput(!m_outputVisible);
        return 0;

    case WM_SYSKEYDOWN:
        if(wParam == VK_RETURN && (lParam & (1 << 29)))
        {
            ApplyWindowMode(m_settings.windowMode == WindowMode::Fullscreen ? WindowMode::Windowed
                                                                           : WindowMode::Fullscreen);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if(wParam == VK_F2)
            OnCommand(IdSelectSource);
        else if(wParam == VK_ESCAPE && m_settings.windowMode == WindowMode::Fullscreen)
            ApplyWindowMode(WindowMode::Windowed);
        return 0;

    case WM_MOUSEACTIVATE:
        /* Belt to WS_EX_NOACTIVATE's braces: in gameplay mode a click must not
         * pull focus away from the game, and it must not be swallowed either. */
        if(m_interaction == InteractionMode::Gameplay)
            return MA_NOACTIVATEANDEAT;
        break;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        m_running = false;
        PostQuitMessage(0);
        return 0;

    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

/* ---------------------------------------------------------------- menus -- */

void App::BuildMenus()
{
    m_menuBar = CreateMenu();

    HMENU capture = CreatePopupMenu();
    AppendMenuW(capture, MF_STRING, IdSelectSource, L"Select Capture &Source...\tF2");
    AppendMenuW(capture, MF_STRING, IdStopCapture, L"S&top Capture");
    AppendMenuW(capture, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(capture, MF_STRING, IdSourceTypeBase + 0, L"Offer &Any Source");
    AppendMenuW(capture, MF_STRING, IdSourceTypeBase + 1, L"Offer &Monitors Only");
    AppendMenuW(capture, MF_STRING, IdSourceTypeBase + 2, L"Offer &Windows Only");
    AppendMenuW(capture, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(capture, MF_STRING, IdCaptureCursor, L"Include &Cursor");
    AppendMenuW(capture, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(capture, MF_STRING, IdExit, L"E&xit");
    AppendMenuW(m_menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(capture), L"&Capture");

    HMENU display = CreatePopupMenu();
    AppendMenuW(display, MF_STRING, IdDisplayModeBase + 0, L"&Original Size");
    AppendMenuW(display, MF_STRING, IdDisplayModeBase + 1, L"&Fit");
    AppendMenuW(display, MF_STRING, IdDisplayModeBase + 2, L"&Stretch");
    AppendMenuW(display, MF_STRING, IdDisplayModeBase + 3, L"&Integer Scale");
    AppendMenuW(display, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(display, MF_STRING, IdWindowModeBase + 0, L"&Windowed");
    AppendMenuW(display, MF_STRING, IdWindowModeBase + 1, L"&Borderless");
    AppendMenuW(display, MF_STRING, IdWindowModeBase + 2, L"F&ullscreen\tAlt+Enter");
    AppendMenuW(display, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(display, MF_STRING, IdGameplayOutput, L"&Gameplay Output");
    AppendMenuW(display, MF_STRING, IdVSync, L"&V-Sync");
    AppendMenuW(m_menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(display), L"&Display");

    HMENU output = CreatePopupMenu();
    AppendMenuW(output, MF_STRING, IdMonitorBase - 1, L"Follow the Prism &window");
    if(!m_monitors.empty())
        AppendMenuW(output, MF_SEPARATOR, 0, nullptr);
    for(size_t i = 0; i < m_monitors.size(); ++i)
        AppendMenuW(output, MF_STRING, IdMonitorBase + static_cast<int>(i), m_monitors[i].name.c_str());
    AppendMenuW(m_menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(output), L"&Output Display");

    HMENU frameRate = CreatePopupMenu();
    for(size_t i = 0; i < std::size(kFpsCeilings); ++i)
        AppendMenuW(frameRate, MF_STRING, IdFpsBase + static_cast<int>(i), FormatFpsLabel(kFpsCeilings[i]).c_str());
    AppendMenuW(m_menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(frameRate), L"&Frame Rate");

    HMENU gpu = CreatePopupMenu();
    AppendMenuW(gpu, MF_STRING, IdAdapterBase - 1, L"&Automatic (high performance)");
    if(!m_adapters.empty())
        AppendMenuW(gpu, MF_SEPARATOR, 0, nullptr);
    for(size_t i = 0; i < m_adapters.size(); ++i)
    {
        std::wstring label = m_adapters[i].description;
        if(m_adapters[i].isSoftware)
            label += L" (software)";
        AppendMenuW(gpu, MF_STRING, IdAdapterBase + static_cast<int>(i), label.c_str());
    }
    AppendMenuW(m_menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(gpu), L"&GPU");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IdHideOutput, L"&Hide Output");
    AppendMenuW(view, MF_STRING, IdToggleInteraction, L"&Toggle Gameplay / Configuration");
    AppendMenuW(view, MF_STRING, IdConfigureShortcuts, L"Configure Global &Shortcuts...");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IdDiagnostics, L"&Diagnostics...");
    AppendMenuW(view, MF_STRING, IdSaveReport, L"Save Diagnostics &Report");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IdAbout, L"&About Prism");
    AppendMenuW(m_menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

    SetMenu(m_window, m_menuBar);
    RefreshMenuChecks();
}

void App::RefreshMenuChecks()
{
    if(!m_menuBar)
        return;

    for(int i = 0; i < 4; ++i)
        CheckMenuItem(m_menuBar, IdDisplayModeBase + i,
                      MF_BYCOMMAND | (static_cast<int>(m_settings.displayMode) == i ? MF_CHECKED : MF_UNCHECKED));
    for(int i = 0; i < 3; ++i)
        CheckMenuItem(m_menuBar, IdWindowModeBase + i,
                      MF_BYCOMMAND | (static_cast<int>(m_settings.windowMode) == i ? MF_CHECKED : MF_UNCHECKED));
    for(size_t i = 0; i < std::size(kFpsCeilings); ++i)
        CheckMenuItem(m_menuBar, IdFpsBase + static_cast<int>(i),
                      MF_BYCOMMAND | (m_settings.maxFps == kFpsCeilings[i] ? MF_CHECKED : MF_UNCHECKED));

    const unsigned sourceMask = m_settings.sourceTypes;
    CheckMenuItem(m_menuBar, IdSourceTypeBase + 0,
                  MF_BYCOMMAND | (sourceMask == PRISM_SOURCE_ANY ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m_menuBar, IdSourceTypeBase + 1,
                  MF_BYCOMMAND | (sourceMask == PRISM_SOURCE_MONITOR ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m_menuBar, IdSourceTypeBase + 2,
                  MF_BYCOMMAND | (sourceMask == PRISM_SOURCE_WINDOW ? MF_CHECKED : MF_UNCHECKED));

    CheckMenuItem(m_menuBar, IdGameplayOutput, MF_BYCOMMAND | (m_gameplayOutput ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m_menuBar, IdMonitorBase - 1,
                  MF_BYCOMMAND | (m_settings.outputMonitorIndex < 0 ? MF_CHECKED : MF_UNCHECKED));
    for(size_t i = 0; i < m_monitors.size(); ++i)
        CheckMenuItem(m_menuBar, IdMonitorBase + static_cast<int>(i),
                      MF_BYCOMMAND |
                          (m_settings.outputMonitorIndex == static_cast<int>(i) ? MF_CHECKED : MF_UNCHECKED));

    CheckMenuItem(m_menuBar, IdCaptureCursor, MF_BYCOMMAND | (m_settings.captureCursor ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m_menuBar, IdVSync, MF_BYCOMMAND | (m_settings.vsync ? MF_CHECKED : MF_UNCHECKED));

    CheckMenuItem(m_menuBar, IdAdapterBase - 1,
                  MF_BYCOMMAND | (m_settings.adapterIndex < 0 ? MF_CHECKED : MF_UNCHECKED));
    for(size_t i = 0; i < m_adapters.size(); ++i)
        CheckMenuItem(m_menuBar, IdAdapterBase + static_cast<int>(i),
                      MF_BYCOMMAND |
                          (m_settings.adapterIndex == static_cast<int>(i) ? MF_CHECKED : MF_UNCHECKED));

    EnableMenuItem(m_menuBar, IdSelectSource,
                   MF_BYCOMMAND | (m_bridge.CaptureAvailable() && !m_bridge.IsActive() ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m_menuBar, IdStopCapture, MF_BYCOMMAND | (m_bridge.IsActive() ? MF_ENABLED : MF_GRAYED));

    DrawMenuBar(m_window);
}

void App::OnCommand(int commandId)
{
    if(commandId >= IdDisplayModeBase && commandId < IdDisplayModeBase + 4)
    {
        m_settings.displayMode = static_cast<DisplayMode>(commandId - IdDisplayModeBase);
        m_lastPresentUs        = 0;
        RefreshMenuChecks();
        return;
    }
    if(commandId >= IdWindowModeBase && commandId < IdWindowModeBase + 3)
    {
        ApplyWindowMode(static_cast<WindowMode>(commandId - IdWindowModeBase));
        return;
    }
    if(commandId >= IdFpsBase && commandId < IdFpsBase + static_cast<int>(std::size(kFpsCeilings)))
    {
        m_settings.maxFps = kFpsCeilings[commandId - IdFpsBase];
        m_bridge.SetMaxFps(m_settings.maxFps);
        RefreshMenuChecks();
        return;
    }
    if(commandId >= IdSourceTypeBase && commandId < IdSourceTypeBase + 3)
    {
        static const unsigned masks[] = {PRISM_SOURCE_ANY, PRISM_SOURCE_MONITOR, PRISM_SOURCE_WINDOW};
        m_settings.sourceTypes        = masks[commandId - IdSourceTypeBase];
        RefreshMenuChecks();
        return;
    }
    if(commandId >= IdMonitorBase - 1 && commandId < IdMonitorBase + static_cast<int>(m_monitors.size()))
    {
        m_settings.outputMonitorIndex = (commandId == IdMonitorBase - 1) ? -1 : commandId - IdMonitorBase;
        if(m_gameplayOutput)
        {
            const RECT bounds = TargetMonitorRect();
            SetWindowPos(m_window, m_settings.gameplayTopmost ? HWND_TOPMOST : HWND_TOP, bounds.left, bounds.top,
                         bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE);
        }
        RefreshMenuChecks();
        return;
    }
    if(commandId >= IdAdapterBase - 1 && commandId < IdAdapterBase + static_cast<int>(m_adapters.size()))
    {
        const int requested = (commandId == IdAdapterBase - 1) ? -1 : commandId - IdAdapterBase;
        if(requested == m_settings.adapterIndex)
            return;

        /* Swapping GPUs means a new device, so the capture session is paused
         * across the rebuild rather than left pointing at a dead texture. */
        const bool wasCapturing = m_bridge.IsActive();
        if(wasCapturing)
            m_bridge.Stop();

        m_settings.adapterIndex = requested;
        if(!m_renderer.Recreate(requested))
            MessageBoxW(m_window, m_renderer.LastError().c_str(), L"Prism - GPU", MB_ICONERROR | MB_OK);
        m_reshade = DetectReShade();

        if(wasCapturing)
            StartCapture();
        RefreshMenuChecks();
        return;
    }

    switch(commandId)
    {
    case IdSelectSource: StartCapture(); break;
    case IdStopCapture: StopCapture(); break;
    case IdCaptureCursor:
        m_settings.captureCursor = !m_settings.captureCursor;
        RefreshMenuChecks();
        break;
    case IdVSync:
        m_settings.vsync = !m_settings.vsync;
        RefreshMenuChecks();
        break;
    case IdShowOutput: ShowOutput(true); break;
    case IdHideOutput:
        if(m_gameplayOutput)
            ExitGameplayOutput();
        else
            ShowOutput(false);
        break;
    case IdGameplayOutput:
        if(m_gameplayOutput)
            ExitGameplayOutput();
        else
            EnterGameplayOutput();
        break;
    case IdExitGameplay: ExitGameplayOutput(); break;
    case IdToggleInteraction: OnShortcut(PrismShortcut::ToggleMode); break;
    case IdConfigureShortcuts: m_hotkeys.Configure(); break;
    case IdShowPrism:
        ShowOutput(true);
        SetInteractionMode(InteractionMode::Configuration);
        break;
    case IdDiagnostics: ToggleDiagnostics(!m_settings.showDiagnostics); break;
    case IdSaveReport: SaveDiagnosticsReport(); break;
    case IdAbout:
        MessageBoxW(m_window,
                    L"Prism - a screen-capture host for Proton/Wine.\r\n\r\n"
                    L"Captures a Linux window, monitor or desktop through the XDG ScreenCast portal "
                    L"and PipeWire, presents it through Direct3D 11, and lets ReShade process the "
                    L"result inside Prism.exe.\r\n\r\n"
                    L"The captured application is never opened, hooked or modified.\r\n\r\n"
                    L"Gameplay Output puts Prism fullscreen over the game while input keeps going "
                    L"to the game itself. The global shortcuts switch between that and an "
                    L"interactive configuration mode.",
                    L"About Prism", MB_ICONINFORMATION | MB_OK);
        break;
    case IdExit: DestroyWindow(m_window); break;
    default: break;
    }
}

/* -------------------------------------------------------------- capture -- */

void App::StartCapture()
{
    if(!m_bridge.IsLoaded() && !m_bridge.Load())
    {
        MessageBoxW(m_window, m_bridge.LoadError().c_str(), L"Prism - Capture", MB_ICONWARNING | MB_OK);
        return;
    }
    if(!m_bridge.CaptureAvailable())
    {
        MessageBoxW(m_window, m_bridge.CaptureError().c_str(), L"Prism - Capture", MB_ICONWARNING | MB_OK);
        return;
    }
    if(m_bridge.IsActive())
        return;

    m_diagnostics.Reset();

    /* The desktop environment owns the picker. Prism never enumerates windows
     * itself and never touches the source application. */
    if(!m_bridge.Start(m_settings.sourceTypes, m_settings.captureCursor, m_settings.maxFps))
        MessageBoxW(m_window, L"Could not start the ScreenCast session.", L"Prism - Capture",
                    MB_ICONERROR | MB_OK);

    RefreshMenuChecks();
    UpdateTrayIcon();
}

void App::StopCapture()
{
    m_bridge.Stop();
    RefreshMenuChecks();
    UpdateTrayIcon();
}

/* --------------------------------------------------------- window modes -- */

void App::ApplyWindowMode(WindowMode mode)
{
    if(!m_window)
        return;

    const WindowMode previous = m_settings.windowMode;
    m_settings.windowMode     = mode;

    if(previous == WindowMode::Windowed && mode != WindowMode::Windowed)
    {
        m_savedPlacement.length = sizeof(m_savedPlacement);
        GetWindowPlacement(m_window, &m_savedPlacement);
        m_savedStyle = GetWindowLongPtrW(m_window, GWL_STYLE);
    }

    switch(mode)
    {
    case WindowMode::Windowed:
        SetMenu(m_window, m_menuBar);
        SetWindowLongPtrW(m_window, GWL_STYLE, m_savedStyle ? m_savedStyle : WS_OVERLAPPEDWINDOW);
        if(m_savedPlacement.length)
            SetWindowPlacement(m_window, &m_savedPlacement);
        SetWindowPos(m_window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        break;

    case WindowMode::Borderless:
        SetMenu(m_window, nullptr);
        SetWindowLongPtrW(m_window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(m_window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        break;

    case WindowMode::Fullscreen:
    {
        /* Borderless fullscreen rather than a DXGI mode switch: exclusive
         * fullscreen under Wine fights the compositor and complicates ReShade's
         * overlay for no latency win on a Wayland session. */
        HMONITOR    monitor = MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info {};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);

        SetMenu(m_window, nullptr);
        SetWindowLongPtrW(m_window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(m_window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                     SWP_FRAMECHANGED);
        break;
    }
    }

    RefreshMenuChecks();
    m_lastPresentUs = 0;
}

void App::ShowOutput(bool visible)
{
    m_outputVisible = visible;
    ShowWindow(m_window, visible ? SW_SHOW : SW_HIDE);
    if(visible)
    {
        SetForegroundWindow(m_window);
        m_lastPresentUs = 0;
    }
    UpdateTrayIcon();
}


/* --------------------------------------------------- gameplay output --- */

/* Each Wayland output KDE exposes shows up here as one Windows monitor, which
 * is how Prism can be told to go fullscreen on the NVIDIA-driven display while
 * the game stays on the AMD-driven one. Names come from the display device, so
 * they survive a re-plug better than an index does. */
BOOL CALLBACK App::MonitorEnumThunk(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM userData)
{
    App* self = reinterpret_cast<App*>(userData);

    (void)dc;
    (void)rect;

    MONITORINFOEXW info {};
    info.cbSize = sizeof(info);
    if(!GetMonitorInfoW(monitor, &info))
        return TRUE;

    MonitorEntry entry;
    entry.bounds  = info.rcMonitor;
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    wchar_t label[160];
    swprintf(label, 160, L"%ls  %ldx%ld at %ld,%ld%ls", info.szDevice, info.rcMonitor.right - info.rcMonitor.left,
             info.rcMonitor.bottom - info.rcMonitor.top, info.rcMonitor.left, info.rcMonitor.top,
             entry.primary ? L"  (primary)" : L"");
    entry.name = label;

    self->m_monitors.push_back(std::move(entry));
    return TRUE;
}

void App::RefreshMonitors()
{
    m_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, &App::MonitorEnumThunk, reinterpret_cast<LPARAM>(this));
    PrismLog("display: %zu monitor(s) visible to Wine", m_monitors.size());
}

RECT App::TargetMonitorRect() const
{
    if(m_settings.outputMonitorIndex >= 0 &&
       m_settings.outputMonitorIndex < static_cast<int>(m_monitors.size()))
        return m_monitors[static_cast<size_t>(m_settings.outputMonitorIndex)].bounds;

    HMONITOR    monitor = MonitorFromWindow(m_window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info {};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    return info.rcMonitor;
}

/*
 * Gameplay output is a borderless fullscreen window on the chosen monitor,
 * shown above the game and deliberately inert: WS_EX_NOACTIVATE keeps it out of
 * the focus chain and WS_EX_TRANSPARENT lets clicks fall through to whatever is
 * underneath, so the real keyboard and mouse keep driving the real game. Prism
 * forwards nothing and synthesises nothing.
 *
 * Wine maps both flags onto its X11/Wayland windows, but a compositor is free to
 * disagree about stacking and focus, so this is the one part of Prism that
 * genuinely needs checking on a real KDE session rather than reasoning about.
 */
void App::EnterGameplayOutput()
{
    if(!m_window)
        return;

    RefreshMonitors();
    m_previousForeground = GetForegroundWindow();
    m_gameplayOutput     = true;

    /* Remember where the ordinary window was, so leaving restores it. */
    if(m_settings.windowMode == WindowMode::Windowed)
    {
        m_savedPlacement.length = sizeof(m_savedPlacement);
        GetWindowPlacement(m_window, &m_savedPlacement);
        m_savedStyle = GetWindowLongPtrW(m_window, GWL_STYLE);
    }

    SetMenu(m_window, nullptr);
    SetWindowLongPtrW(m_window, GWL_STYLE, WS_POPUP | WS_VISIBLE);

    const RECT bounds = TargetMonitorRect();
    SetWindowPos(m_window, m_settings.gameplayTopmost ? HWND_TOPMOST : HWND_TOP, bounds.left, bounds.top,
                 bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);

    m_outputVisible = true;
    SetInteractionMode(InteractionMode::Gameplay);

    /* Nothing about entering the overlay should take the game's focus away. */
    if(m_previousForeground && m_previousForeground != m_window)
        SetForegroundWindow(m_previousForeground);

    m_lastPresentUs = 0;
    RefreshMenuChecks();
    UpdateTrayIcon();
    PrismLog("gameplay output on (%ldx%ld at %ld,%ld)", bounds.right - bounds.left, bounds.bottom - bounds.top,
             bounds.left, bounds.top);
}

void App::ExitGameplayOutput()
{
    if(!m_gameplayOutput)
        return;

    m_gameplayOutput = false;
    SetInteractionMode(InteractionMode::Configuration);

    /* Back to whatever ordinary window mode was configured. Capture keeps
     * running throughout: only presentation stops. */
    ShowWindow(m_window, SW_HIDE);
    m_outputVisible = false;

    SetWindowPos(m_window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetMenu(m_window, m_menuBar);
    SetWindowLongPtrW(m_window, GWL_STYLE, m_savedStyle ? m_savedStyle : WS_OVERLAPPEDWINDOW);
    if(m_savedPlacement.length)
        SetWindowPlacement(m_window, &m_savedPlacement);

    if(m_previousForeground && IsWindow(m_previousForeground))
        SetForegroundWindow(m_previousForeground);

    RefreshMenuChecks();
    UpdateTrayIcon();
    PrismLog("gameplay output off; capture continues");
}

void App::ApplyInteractionStyles()
{
    if(!m_window)
        return;

    LONG_PTR exStyle = GetWindowLongPtrW(m_window, GWL_EXSTYLE);

    if(m_interaction == InteractionMode::Gameplay)
    {
        /* NOACTIVATE: never becomes the foreground window on click or show.
         * TRANSPARENT: hit-testing falls through to the window underneath.
         * Together they are what "watch Prism, play the game" means. */
        exStyle |= (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT);
    }
    else
    {
        exStyle &= ~(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT);
    }

    SetWindowLongPtrW(m_window, GWL_EXSTYLE, exStyle);
    SetWindowPos(m_window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

void App::SetInteractionMode(InteractionMode mode)
{
    if(m_interaction == mode)
        return;

    m_interaction = mode;
    ApplyInteractionStyles();

    if(mode == InteractionMode::Configuration)
    {
        /* Hand focus to Prism so ReShade's overlay can be driven. Its own
         * hotkey (Home by default) opens it; Prism does not try to press that
         * for the user, because faking input into ReShade is exactly the kind
         * of fragility this mode exists to avoid. */
        if(m_gameplayOutput)
        {
            m_previousForeground = GetForegroundWindow();
            SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        SetForegroundWindow(m_window);
        SetFocus(m_window);
        PrismLog("configuration mode: Prism is interactive, open ReShade with its overlay key");
    }
    else
    {
        /* Give the game its input back. */
        if(m_previousForeground && IsWindow(m_previousForeground) && m_previousForeground != m_window)
            SetForegroundWindow(m_previousForeground);
        PrismLog("gameplay mode: input belongs to the captured application again");
    }

    RefreshMenuChecks();
    UpdateTrayIcon();
}

void App::OnShortcut(PrismShortcut shortcut)
{
    switch(shortcut)
    {
    case PrismShortcut::ToggleMode:
        /* Outside gameplay output the toggle just brings Prism forward, which
         * is the sensible reading of "make Prism interactive". */
        if(!m_gameplayOutput)
        {
            ShowOutput(true);
            SetInteractionMode(InteractionMode::Configuration);
            break;
        }
        SetInteractionMode(m_interaction == InteractionMode::Gameplay ? InteractionMode::Configuration
                                                                      : InteractionMode::Gameplay);
        break;

    case PrismShortcut::HideOutput:
        /* The escape hatch. Must work even if presentation has wedged, so it
         * only touches window state and never the renderer. */
        if(m_gameplayOutput)
            ExitGameplayOutput();
        else
            ShowOutput(false);
        break;

    default: break;
    }
}

/* ----------------------------------------------------------------- tray -- */

void App::CreateTrayIcon()
{
    NOTIFYICONDATAW data {};
    data.cbSize           = sizeof(data);
    data.hWnd             = m_window;
    data.uID              = 1;
    data.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_PRISM_TRAY;
    data.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy(data.szTip, L"Prism - Idle", ARRAYSIZE(data.szTip) - 1);

    m_trayIconAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if(!m_trayIconAdded)
        PrismLog("tray: Shell_NotifyIcon failed, running without a tray icon");
}

void App::UpdateTrayIcon()
{
    if(!m_trayIconAdded)
        return;

    NOTIFYICONDATAW data {};
    data.cbSize = sizeof(data);
    data.hWnd   = m_window;
    data.uID    = 1;
    data.uFlags = NIF_TIP;

    const std::wstring tip = m_diagnostics.BuildSummary(IsCapturing(), m_testPattern.IsRunning(), m_gameplayOutput);
    wcsncpy(data.szTip, tip.c_str(), ARRAYSIZE(data.szTip) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void App::DestroyTrayIcon()
{
    if(!m_trayIconAdded)
        return;

    NOTIFYICONDATAW data {};
    data.cbSize = sizeof(data);
    data.hWnd   = m_window;
    data.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    m_trayIconAdded = false;
}

void App::ShowTrayMenu()
{
    HMENU menu = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, m_diagnostics.BuildSummary(IsCapturing(), m_testPattern.IsRunning(), m_gameplayOutput).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    /* Both entries do the same thing on purpose: the portal grants a session
     * per selection, so starting capture always means picking a source. */
    AppendMenuW(menu, MF_STRING | (m_bridge.IsActive() ? MF_GRAYED : 0), IdSelectSource, L"Select Capture Source...");
    AppendMenuW(menu, MF_STRING | (m_bridge.IsActive() ? MF_GRAYED : 0), IdSelectSource, L"Start Capture");
    AppendMenuW(menu, MF_STRING | (m_bridge.IsActive() ? 0 : MF_GRAYED), IdStopCapture, L"Stop Capture");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (m_gameplayOutput ? MF_GRAYED : 0), IdGameplayOutput, L"Show Gameplay Output");
    AppendMenuW(menu, MF_STRING | (m_gameplayOutput ? 0 : MF_GRAYED), IdExitGameplay, L"Hide Gameplay Output");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, IdShowPrism, L"Show Prism");
    AppendMenuW(menu, MF_STRING, IdDiagnostics, L"Diagnostics...");
    AppendMenuW(menu, MF_STRING, IdConfigureShortcuts, L"Settings: Global Shortcuts...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IdExit, L"Quit");

    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(m_window);
    const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, m_window,
                                      nullptr);
    DestroyMenu(menu);

    if(chosen > 0)
        OnCommand(chosen);
}

/* ---------------------------------------------------------- diagnostics -- */

void App::ToggleDiagnostics(bool visible)
{
    m_settings.showDiagnostics = visible;

    if(visible && !m_diagnosticsWindow)
    {
        m_diagnosticsWindow = CreateWindowExW(WS_EX_TOOLWINDOW, kDiagnosticsClass, L"Prism - Diagnostics",
                                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX, CW_USEDEFAULT,
                                              CW_USEDEFAULT, 620, 640, m_window, nullptr, m_instance, nullptr);
        if(m_diagnosticsWindow)
        {
            m_diagnosticsEdit = CreateWindowExW(0, L"EDIT", L"",
                                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                                    ES_AUTOVSCROLL,
                                                0, 0, 620, 640, m_diagnosticsWindow, nullptr, m_instance, nullptr);
            HFONT font = static_cast<HFONT>(GetStockObject(ANSI_FIXED_FONT));
            if(m_diagnosticsEdit && font)
                SendMessageW(m_diagnosticsEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }

    if(m_diagnosticsWindow)
    {
        ShowWindow(m_diagnosticsWindow, visible ? SW_SHOW : SW_HIDE);
        if(visible)
            UpdateDiagnostics();
    }
}

Diagnostics::ReportContext App::BuildReportContext() const
{
    Diagnostics::ReportContext context;
    context.bridge         = &m_bridge;
    context.renderer       = &m_renderer;
    context.settings       = &m_settings;
    context.reshade        = &m_reshade;
    context.hotkeys        = &m_hotkeys;
    context.testPattern    = m_testPattern.IsRunning();
    context.gameplayOutput = m_gameplayOutput;
    context.interaction    = m_interaction;
    return context;
}

void App::UpdateDiagnostics()
{
    if(!m_diagnosticsEdit || !m_settings.showDiagnostics || !IsWindowVisible(m_diagnosticsWindow))
        return;
    SetWindowTextW(m_diagnosticsEdit, m_diagnostics.BuildReport(BuildReportContext()).c_str());
}

/* The diagnostics window cannot be scrolled through a screenshot and cannot be
 * pasted into a bug report, so the same text goes to a file on demand. This is
 * the artefact to attach when something misbehaves on a real desktop. */
void App::SaveDiagnosticsReport()
{
    const std::wstring path   = PrismModuleDirectory() + L"prism-diagnostics.txt";
    const std::wstring report = m_diagnostics.BuildReport(BuildReportContext());

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE)
    {
        PrismLog("diagnostics: could not write %ls (error %lu)", path.c_str(), GetLastError());
        return;
    }

    /* UTF-8 without a BOM: this ends up pasted into a terminal or an issue. */
    const int needed = WideCharToMultiByte(CP_UTF8, 0, report.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if(needed > 1)
    {
        std::string utf8(static_cast<size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, report.c_str(), -1, utf8.data(), needed, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);

    PrismLog("diagnostics: wrote %ls", path.c_str());
    fprintf(stderr, "----- Prism diagnostics -----\n");
    {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, report.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(bytes > 0 ? bytes - 1 : 0), '\0');
        if(bytes > 1)
            WideCharToMultiByte(CP_UTF8, 0, report.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
        fputs(utf8.c_str(), stderr);
    }
    fprintf(stderr, "----- end -----\n");
    fflush(stderr);
}

/* ------------------------------------------------------------- main loop -- */

int App::Run()
{
    while(m_running)
    {
        PumpMessages();
        if(!m_running)
            break;
        RenderOnce();
        if(!m_running)
            break;
        WaitForWork();
    }
    return 0;
}

void App::PumpMessages()
{
    MSG message;
    while(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if(message.message == WM_QUIT)
        {
            m_running = false;
            return;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void App::RenderOnce()
{
    const uint64_t now = PrismNowUs();

    /* The ceiling governs how fast Prism is willing to consume the source. It
     * never invents a frame to reach a target, and it never holds a newer frame
     * back to pace an older one. */
    const uint64_t minInterval = m_settings.maxFps ? (1000000ull / m_settings.maxFps) : 0ull;
    if(minInterval && m_lastPresentUs && now - m_lastPresentUs < minInterval)
        return;

    const CapturedFrame* frame = m_bridge.Mailbox().Acquire();

    if(frame)
    {
        m_diagnostics.NoteFrameReceived(now);
        if(frame->ptsNs != 0 && frame->bridgeRecvNs > frame->ptsNs)
            m_diagnostics.NoteCaptureLatency(static_cast<double>(frame->bridgeRecvNs - frame->ptsNs) / 1000000.0);

        if(!m_renderer.UploadFrame(*frame))
            return;
        m_diagnostics.NoteFrameAge(static_cast<double>(now - frame->receivedUs) / 1000.0);
    }
    else if(m_lastPresentUs && now - m_lastPresentUs < kIdleRepresentUs)
    {
        return;
    }

    /* A hidden output window still consumes frames - capture keeps running and
     * the counters keep moving - but there is nothing to present to. */
    if(!m_outputVisible || IsIconic(m_window))
    {
        m_lastPresentUs = now;
        return;
    }

    if(!m_renderer.Present(m_settings.displayMode, m_settings.vsync))
    {
        PrismLog("renderer: present failed, rebuilding the device");
        if(!m_renderer.Recreate(m_settings.adapterIndex))
        {
            MessageBoxW(m_window, m_renderer.LastError().c_str(), L"Prism - Direct3D", MB_ICONERROR | MB_OK);
            m_running = false;
            return;
        }
        m_reshade = DetectReShade();
    }

    m_lastPresentUs = PrismNowUs();
    m_diagnostics.NotePresented(m_lastPresentUs);
}

void App::WaitForWork()
{
    /* Block on the mailbox event and the message queue together, so a new frame
     * or a click both wake the loop immediately and an idle Prism costs nothing. */
    const uint64_t now         = PrismNowUs();
    const uint64_t minInterval = m_settings.maxFps ? (1000000ull / m_settings.maxFps) : 0ull;

    DWORD timeoutMs = static_cast<DWORD>(kIdleRepresentUs / 1000);
    if(minInterval && m_lastPresentUs)
    {
        const uint64_t elapsed = now - m_lastPresentUs;
        if(elapsed < minInterval)
            timeoutMs = std::min<DWORD>(timeoutMs, static_cast<DWORD>((minInterval - elapsed) / 1000) + 1);
    }

    HANDLE frameEvent = m_bridge.Mailbox().FrameEvent();
    if(!frameEvent)
    {
        Sleep(1);
        return;
    }

    MsgWaitForMultipleObjectsEx(1, &frameEvent, timeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}
