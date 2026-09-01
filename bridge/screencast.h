/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PRISM_SCREENCAST_H
#define PRISM_SCREENCAST_H

#include "pipewire.h"

/* Opens the session bus and the ScreenCast portal proxy. */
int prism_screencast_init(void);

/* Runs the GLib main loop with the PipeWire loop attached as a GSource.
 * Blocks until prism_screencast_quit(). Must be called on a Win32 thread. */
void prism_screencast_run(void);

/* Kicks off CreateSession -> SelectSources -> Start on the loop thread. The
 * desktop environment shows its own picker; the outcome arrives through the
 * callbacks. */
void prism_screencast_start(unsigned int source_types, unsigned int cursor_mode,
                            const struct prism_pw_callbacks* callbacks);
void prism_screencast_stop(void);
void prism_screencast_quit(void);

/* Source kinds the portal says it can offer, as a PRISM_SOURCE_* mask. */
unsigned int prism_screencast_available_sources(void);

#endif /* PRISM_SCREENCAST_H */
