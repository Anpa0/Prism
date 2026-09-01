/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Thin helper around the org.freedesktop.portal.Request handshake. Derived from
 * the portal helper in OBS Studio / ShaderGlass WineCap:
 *   Copyright 2021 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *   SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PRISM_PORTAL_H
#define PRISM_PORTAL_H

#include <gio/gio.h>

typedef void (*prism_portal_signal_cb)(GVariant* parameters, void* user_data);

/* Session bus connection shared by the whole bridge. NULL if unavailable. */
GDBusConnection* prism_portal_connection(void);

/* Builds the /org/freedesktop/portal/desktop/request/<sender>/<token> path the
 * portal will emit its Response signal on, together with the matching token.
 * Both outputs are malloc()ed; either pointer may be NULL. */
void prism_portal_request_path(char** out_path, char** out_token);
void prism_portal_session_token(char** out_token);

/* Subscribes to a single Response signal on `path`. The subscription and the
 * bookkeeping are released once the signal fires or `cancellable` trips. */
void prism_portal_subscribe(const char* path, GCancellable* cancellable, prism_portal_signal_cb callback,
                            void* user_data);

#endif /* PRISM_PORTAL_H */
