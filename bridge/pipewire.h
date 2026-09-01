/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PRISM_PIPEWIRE_H
#define PRISM_PIPEWIRE_H

#include <pipewire/pipewire.h>

#include "PrismCapture.h"

/* Internal (cdecl) callbacks used between the bridge's own translation units.
 * The __stdcall callbacks in PrismCapture.h are the ones crossing into PE code. */
typedef void (*prism_pw_frame_cb)(const PrismFrame* frame);
typedef void (*prism_pw_format_cb)(unsigned int width, unsigned int height, unsigned int format,
                                   unsigned int fps_num, unsigned int fps_den);
typedef void (*prism_pw_state_cb)(unsigned int state, const char* message);

struct prism_pw_callbacks
{
    prism_pw_frame_cb  on_frame;
    prism_pw_format_cb on_format;
    prism_pw_state_cb  on_state;
};

/* Connects `pipewire_fd` (received from OpenPipeWireRemote) and subscribes to
 * `pipewire_node`. Runs entirely on the loop passed in. */
int  prism_pipewire_start(struct pw_loop* loop, struct pw_context* ctx, int pipewire_fd, uint32_t pipewire_node,
                          const struct prism_pw_callbacks* callbacks);
void prism_pipewire_stop(void);

/* Delivery ceiling in frames per second, 0 disables throttling. Frames dropped
 * here never cost a copy. */
void prism_pipewire_set_max_fps(unsigned int max_fps);

/* Counters, read by PrismCaptureGetStats(). */
void prism_pipewire_get_counters(unsigned long long* received, unsigned long long* delivered,
                                 unsigned long long* throttled, unsigned long long* corrupt);

/* Layout of the most recent buffer, for the diagnostics panel: negotiated
 * stride, the PipeWire mapping size, and how long the last callback took. */
void prism_pipewire_get_layout(unsigned int* stride, unsigned int* maxsize, double* callback_ms);

#endif /* PRISM_PIPEWIRE_H */
