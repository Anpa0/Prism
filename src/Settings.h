/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "Common.h"

enum class DisplayMode
{
    OriginalSize = 0, /* 1:1 pixels, centred, no filtering            */
    Fit          = 1, /* scale to fit, aspect preserved, letterboxed  */
    Stretch      = 2, /* fill the window, aspect ignored              */
    IntegerScale = 3, /* largest whole-number multiple that fits      */
};

enum class WindowMode
{
    Windowed   = 0,
    Borderless = 1,
    Fullscreen = 2,
};

/* Frame-rate ceilings offered in the menu. 0 means "match source": Prism
 * consumes every frame PipeWire produces and never fabricates one. */
inline constexpr unsigned kFpsCeilings[] = {0, 30, 60, 90, 120, 144, 165, 240};

/* Prism.ini, next to Prism.exe. Kept separate from ReShade.ini on purpose. */
struct Settings
{
    DisplayMode displayMode  = DisplayMode::Fit;
    WindowMode  windowMode   = WindowMode::Windowed;
    unsigned    maxFps       = 0;
    bool        vsync        = false;
    bool        captureCursor = false;
    unsigned    sourceTypes  = 0x7u; /* PRISM_SOURCE_ANY */
    int         adapterIndex = -1;   /* -1 = let DXGI pick the default   */
    int         windowWidth  = 1280;
    int         windowHeight = 720;
    bool        showDiagnostics = false;

    void Load();
    void Save() const;
};
