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

/* Session bus connection shared by the whole bridge. NULL if unavailable.
 * The first call also performs the host app-id registration below, because the
 * portal refuses a late Register(). */
GDBusConnection* prism_portal_connection(void);

/* Announces PRISM_APP_ID to org.freedesktop.host.portal.Registry.
 *
 * xdg-desktop-portal 1.20 introduced this handshake for unsandboxed apps, and
 * 1.21 made GlobalShortcuts.CreateSession fail outright when the connection has
 * no app id ("An app id is required"). Registration is one-shot per connection
 * and must happen before any other portal call, so this runs the moment the bus
 * connection is created. Older portals simply have no such interface, which is
 * not an error. Returns 1 when the connection carries our app id. */
int prism_portal_app_id_registered(void);

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
