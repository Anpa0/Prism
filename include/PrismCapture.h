/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * Copyright (C) 2026 Prism contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PrismCapture ABI
 * ----------------
 * Shared contract between Prism.exe (a PE binary built with MinGW/MSVC) and
 * PrismCapture.dll (a Winelib ELF shared object built with winegcc that links
 * against native libpipewire and GIO).
 *
 * This header is compiled by BOTH sides, so it must stay free of anything that
 * is not available to a plain C89-ish compiler on either toolchain.
 */

#ifndef PRISM_CAPTURE_H
#define PRISM_CAPTURE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bump whenever the ABI below changes in an incompatible way. Prism.exe
 * refuses to talk to a bridge that reports a different value.
 *
 *   1 - capture only
 *   2 - adds the GlobalShortcuts portal, host app-id registration, Linux
 *       system/GPU introspection, and frame buffer-size validation
 */
#define PRISM_CAPTURE_ABI_VERSION 2u

/* Reverse-DNS application id. xdg-desktop-portal 1.20 added
 * org.freedesktop.host.portal.Registry, and 1.21 made GlobalShortcuts refuse a
 * session whose connection has an empty app id, so an unsandboxed process like
 * Prism has to announce itself before it calls any other portal method. The id
 * must match the basename of an installed .desktop file; ship the one in
 * packaging/ to ~/.local/share/applications/. */
#define PRISM_APP_ID "net.prism.Prism"

/* The file Prism.exe passes to LoadLibraryW(). It is a Winelib module, i.e. an
 * ELF shared object with a PE-style export table, so it only resolves when
 * running under Wine/Proton. On real Windows the load fails and Prism reports
 * "no capture bridge" instead of crashing. */
#define PRISM_CAPTURE_MODULE L"PrismCapture.dll"

/* ---------------------------------------------------------------- types -- */

/* Bit mask of source kinds offered to the XDG ScreenCast portal picker.
 * Mirrors org.freedesktop.portal.ScreenCast "AvailableSourceTypes". */
#define PRISM_SOURCE_MONITOR 0x1u
#define PRISM_SOURCE_WINDOW  0x2u
#define PRISM_SOURCE_VIRTUAL 0x4u
#define PRISM_SOURCE_ANY     0x7u

/* Cursor handling requested from the portal. */
#define PRISM_CURSOR_HIDDEN   0u
#define PRISM_CURSOR_EMBEDDED 1u

/* Pixel layouts the bridge is allowed to hand over. Both are 32bpp little
 * endian BGRA byte order, which maps 1:1 onto DXGI_FORMAT_B8G8R8A8_UNORM, so
 * no colour conversion ever happens on the way to Direct3D. */
#define PRISM_FORMAT_BGRX 1u /* alpha byte is padding, treat as opaque */
#define PRISM_FORMAT_BGRA 2u /* alpha byte is meaningful (ignored by Prism)  */
/* Byte-swapped layouts. Some portal backends and virtual sources hand these
 * over instead. They need an explicit channel swap, which Prism performs once,
 * measures, and reports in diagnostics rather than silently reinterpreting. */
#define PRISM_FORMAT_RGBX 3u
#define PRISM_FORMAT_RGBA 4u

/* Session state pushed to Prism.exe through the status callback. */
#define PRISM_STATE_IDLE        0u /* stopped, nothing in flight              */
#define PRISM_STATE_NEGOTIATING 1u /* portal dialog is up / stream connecting */
#define PRISM_STATE_ACTIVE      2u /* frames are flowing                      */
#define PRISM_STATE_ERROR       3u /* failed or denied, see message           */

typedef struct PrismFrame
{
    const void*        data;     /* first pixel, valid ONLY inside callback  */
    unsigned int       width;    /* visible width in pixels                  */
    unsigned int       height;   /* visible height in pixels                 */
    unsigned int       pitch;    /* bytes per row of `data`                  */
    unsigned int       format;   /* PRISM_FORMAT_*                           */
    unsigned long long pts_ns;   /* PipeWire presentation stamp, 0 = unknown */
    unsigned long long recv_ns;  /* CLOCK_MONOTONIC when the bridge got it   */
    unsigned long long sequence; /* monotonic counter of delivered frames    */
    unsigned long long data_size; /* readable bytes at `data`, for bounds
                                   * checking. Never assume height * pitch:
                                   * a cropped frame starts partway in.      */
} PrismFrame;

typedef struct PrismBridgeStats
{
    unsigned long long frames_received;   /* buffers dequeued from PipeWire   */
    unsigned long long frames_delivered;  /* buffers handed to Prism.exe      */
    unsigned long long frames_throttled;  /* skipped by the FPS ceiling       */
    unsigned long long frames_corrupt;    /* dropped: no data / bad geometry  */
    unsigned int       source_width;      /* negotiated stream width          */
    unsigned int       source_height;     /* negotiated stream height         */
    unsigned int       source_format;     /* PRISM_FORMAT_*                   */
    unsigned int       source_fps_num;    /* negotiated framerate numerator   */
    unsigned int       source_fps_den;    /* negotiated framerate denominator */
    unsigned int       state;             /* PRISM_STATE_*                    */
    unsigned int       source_stride;     /* negotiated bytes per row         */
    unsigned int       source_max_size;   /* PipeWire buffer capacity         */
    unsigned int       needs_swizzle;     /* 1 when the layout is R/B swapped */
    double             callback_ms;       /* time spent in the last callback  */
} PrismBridgeStats;

/* The frame callback runs on the bridge's capture thread (a real Win32 thread
 * created with CreateThread, so it is registered with Wine and may re-enter PE
 * code). `frame->data` points into PipeWire-mapped memory that is recycled the
 * moment the callback returns: copy it, do not retain it. */
typedef void(__stdcall* PrismFrameCallback)(const PrismFrame* frame, void* context);

/* Status callback, same thread rules. `message` is UTF-8 and only valid for
 * the duration of the call. */
typedef void(__stdcall* PrismStatusCallback)(unsigned int state, const char* message, void* context);

/* ------------------------------------------------------------- exports -- */

unsigned int __stdcall PrismCaptureVersion(void);

/* Connects to the session bus and the ScreenCast portal. Returns S_OK (0) on
 * success. Safe to call more than once. */
long __stdcall PrismCaptureInit(void);

/* Opens the desktop environment's own source picker and, once the user has
 * chosen, starts streaming frames to `on_frame`.
 *   source_types - PRISM_SOURCE_* bit mask offered to the picker
 *   cursor_mode  - PRISM_CURSOR_*
 *   max_fps      - ceiling on delivered frames per second, 0 = match source
 * Returns S_OK once the request has been dispatched; the actual outcome
 * arrives asynchronously through `on_status`. */
long __stdcall PrismCaptureStart(unsigned int        source_types,
                                 unsigned int        cursor_mode,
                                 unsigned int        max_fps,
                                 PrismFrameCallback  on_frame,
                                 PrismStatusCallback on_status,
                                 void*               context);

/* Tears the PipeWire stream and portal session down. After this returns no
 * further frame callbacks will be issued. */
long __stdcall PrismCaptureStop(void);

/* Adjusts the delivery ceiling of a running session. 0 = match source. */
void __stdcall PrismCaptureSetMaxFps(unsigned int max_fps);

/* Copies the current counters into `out`. Cheap, lock-free, safe any time. */
void __stdcall PrismCaptureGetStats(PrismBridgeStats* out);

/* Stops the capture thread. Called before unloading the module. */
void __stdcall PrismCaptureShutdown(void);

/* ---------------------------------------------------- global shortcuts -- */

/* Shortcut state, reported through PrismShortcutsGetStatus(). */
#define PRISM_SHORTCUTS_UNAVAILABLE 0u /* no GlobalShortcuts portal here     */
#define PRISM_SHORTCUTS_PENDING     1u /* session/bind request in flight     */
#define PRISM_SHORTCUTS_BOUND       2u /* the compositor is delivering them  */
#define PRISM_SHORTCUTS_DENIED      3u /* the user declined the bind dialog  */
#define PRISM_SHORTCUTS_ERROR       4u /* see the message                    */

#define PRISM_SHORTCUT_ID_MAX      64
#define PRISM_SHORTCUT_TRIGGER_MAX 128
#define PRISM_SHORTCUT_MAX          8

typedef struct PrismShortcutSpec
{
    const char* id;                /* stable, e.g. "toggle-mode"            */
    const char* description;       /* shown in the compositor's dialog      */
    const char* preferred_trigger; /* XDG shortcuts syntax, e.g.
                                    * "CTRL+SHIFT+F11". The user may change
                                    * it; the compositor decides.           */
} PrismShortcutSpec;

typedef struct PrismShortcutBinding
{
    char id[PRISM_SHORTCUT_ID_MAX];
    char trigger[PRISM_SHORTCUT_TRIGGER_MAX]; /* as the compositor describes it */
} PrismShortcutBinding;

/* Fires on the bridge's capture thread when the compositor reports the
 * shortcut, whatever has focus. Post it to the UI thread; do not block. */
typedef void(__stdcall* PrismShortcutCallback)(const char* shortcut_id, void* context);

/* Creates a GlobalShortcuts session and binds `count` shortcuts in one go -
 * the portal only allows one bind attempt per session. Returns S_OK once the
 * request is dispatched; the outcome shows up in PrismShortcutsGetStatus().
 * Safe to call when no portal exists: it reports UNAVAILABLE. */
long __stdcall PrismShortcutsStart(const PrismShortcutSpec* shortcuts, unsigned int count,
                                   PrismShortcutCallback on_activated, void* context);

/* `message` receives a UTF-8 explanation, truncated to `message_bytes`. */
unsigned int __stdcall PrismShortcutsGetStatus(char* message, unsigned int message_bytes);

/* Copies up to `max` actual bindings; returns how many were written. */
unsigned int __stdcall PrismShortcutsGetBindings(PrismShortcutBinding* out, unsigned int max);

/* Asks the compositor to show its shortcut configuration UI for our session.
 * Portal interface version 2 and later; a no-op elsewhere. */
void __stdcall PrismShortcutsConfigure(void);

void __stdcall PrismShortcutsStop(void);

/* ------------------------------------------------- Linux system probing -- */

#define PRISM_GPU_MAX 8

typedef struct PrismGpuInfo
{
    char         pci_address[16]; /* 0000:03:00.0                            */
    char         driver[32];      /* amdgpu, nvidia, i915, ...               */
    char         name[160];       /* resolved from pci.ids when available    */
    char         drm_card[32];    /* card1                                   */
    char         drm_render[32];  /* renderD128                              */
    char         by_path[160];    /* /dev/dri/by-path/pci-0000:03:00.0-card  */
    char         link_speed_cur[24]; /* "16.0 GT/s PCIe"                     */
    char         link_speed_max[24];
    unsigned int vendor_id;
    unsigned int device_id;
    unsigned int link_width_cur;
    unsigned int link_width_max;
    unsigned int boot_vga;        /* 1 when the firmware booted on this one  */
    unsigned int reserved;
} PrismGpuInfo;

typedef struct PrismSystemInfo
{
    unsigned int gpu_count;
    PrismGpuInfo gpus[PRISM_GPU_MAX];
    char         session_type[24];  /* wayland / x11                         */
    char         desktop[48];       /* XDG_CURRENT_DESKTOP                   */
    char         app_id[64];        /* what we registered with the portal    */
    char         gpu_env[512];      /* the GPU-selection variables in effect  */
} PrismSystemInfo;

/* Fills `out` from sysfs and the environment. Cheap enough to call on a timer,
 * but Prism only refreshes it on demand. */
void __stdcall PrismSystemInfoQuery(PrismSystemInfo* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRISM_CAPTURE_H */
