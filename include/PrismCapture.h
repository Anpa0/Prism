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
 * refuses to talk to a bridge that reports a different value. */
#define PRISM_CAPTURE_ABI_VERSION 1u

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
    unsigned int       reserved;
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRISM_CAPTURE_H */
