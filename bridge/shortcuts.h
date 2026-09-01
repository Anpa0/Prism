/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PRISM_SHORTCUTS_H
#define PRISM_SHORTCUTS_H

#include "PrismCapture.h"

/* Internal (cdecl) activation callback; the __stdcall one crosses into PE. */
typedef void (*prism_shortcut_activated_cb)(const char* shortcut_id);

/* All of these are called from the PE side and hop onto the GLib loop thread
 * themselves, so they are safe to call from Prism.exe's UI thread. */
void prism_shortcuts_start(const PrismShortcutSpec* shortcuts, unsigned int count,
                           prism_shortcut_activated_cb callback);
void prism_shortcuts_stop(void);
void prism_shortcuts_configure(void);

unsigned int prism_shortcuts_status(char* message, unsigned int message_bytes);
unsigned int prism_shortcuts_bindings(PrismShortcutBinding* out, unsigned int max);

#endif /* PRISM_SHORTCUTS_H */
