/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Derived from the portal helper in OBS Studio / ShaderGlass WineCap:
 *   Copyright 2021 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *   SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "prism_log.h"
#include "portal.h"

#include "PrismCapture.h"

#define PRISM_REQUEST_PATH "/org/freedesktop/portal/desktop/request/%s/prism%u"
#define PRISM_TOKEN_MAX    512

struct prism_portal_call
{
    GCancellable*          cancellable;
    prism_portal_signal_cb callback;
    void*                  user_data;
    char*                  request_path;
    guint                  signal_id;
    gulong                 cancelled_id;
};

static GDBusConnection* g_connection    = NULL;
static int              g_app_registered = 0;

/* Must run before any other portal method on this connection. */
static void prism_portal_register_app_id(void)
{
    GVariantBuilder builder;
    GVariant*       result;
    GError*         error = NULL;

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    result = g_dbus_connection_call_sync(
        g_connection, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.host.portal.Registry", "Register",
        g_variant_new("(sa{sv})", PRISM_APP_ID, &builder), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

    if(error)
    {
        /* UnknownMethod/UnknownInterface means the portal predates 1.20, which
         * is fine - those versions derive the app id themselves. Anything else
         * is worth surfacing, because GlobalShortcuts will refuse us later. */
        if(g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) ||
           g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE) ||
           g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT))
            prism_info("host portal Registry not present; assuming a pre-1.20 xdg-desktop-portal");
        else
            prism_warn("could not register the app id '%s': %s", PRISM_APP_ID, error->message);
        g_error_free(error);
        return;
    }

    g_clear_pointer(&result, g_variant_unref);
    g_app_registered = 1;
    prism_info("registered with the host portal as '%s'", PRISM_APP_ID);
}

GDBusConnection* prism_portal_connection(void)
{
    if(!g_connection)
    {
        GError* error = NULL;
        g_connection   = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
        if(error)
        {
            prism_warn("cannot reach the session bus: %s", error->message);
            g_error_free(error);
            return NULL;
        }
        prism_portal_register_app_id();
    }
    return g_connection;
}

int prism_portal_app_id_registered(void)
{
    return g_app_registered;
}

/* The portal derives the Request object path from our unique bus name with the
 * leading ':' stripped and dots replaced by underscores. */
static char* prism_sender_name(void)
{
    GDBusConnection* connection = prism_portal_connection();
    char*            sender;
    char*            cursor;

    if(!connection)
        return g_strdup("unknown");

    sender = g_strdup(g_dbus_connection_get_unique_name(connection) + 1);
    while((cursor = strstr(sender, ".")) != NULL)
        *cursor = '_';
    return sender;
}

void prism_portal_request_path(char** out_path, char** out_token)
{
    static unsigned int counter = 0;
    counter++;

    if(out_token)
        *out_token = g_strdup_printf("prism%u", counter);

    if(out_path)
    {
        char* sender = prism_sender_name();
        *out_path    = g_strdup_printf(PRISM_REQUEST_PATH, sender, counter);
        g_free(sender);
    }
}

void prism_portal_session_token(char** out_token)
{
    static unsigned int counter = 0;
    counter++;
    if(out_token)
        *out_token = g_strdup_printf("prismsession%u", counter);
}

static void prism_portal_call_free(struct prism_portal_call* call)
{
    GDBusConnection* connection = prism_portal_connection();

    if(call->signal_id && connection)
        g_dbus_connection_signal_unsubscribe(connection, call->signal_id);
    if(call->cancelled_id > 0 && call->cancellable)
        g_signal_handler_disconnect(call->cancellable, call->cancelled_id);
    g_clear_object(&call->cancellable);
    g_clear_pointer(&call->request_path, g_free);
    g_free(call);
}

static void on_cancelled(GCancellable* cancellable, void* user_data)
{
    struct prism_portal_call* call       = user_data;
    GDBusConnection*          connection = prism_portal_connection();

    (void)cancellable;
    prism_debug("portal request cancelled: %s", call->request_path);

    if(connection)
        g_dbus_connection_call(connection, "org.freedesktop.portal.Desktop", call->request_path,
                               "org.freedesktop.portal.Request", "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                               NULL, NULL);
    prism_portal_call_free(call);
}

static void on_response(GDBusConnection* connection, const char* sender_name, const char* object_path,
                        const char* interface_name, const char* signal_name, GVariant* parameters, void* user_data)
{
    struct prism_portal_call* call = user_data;

    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;

    if(call->callback)
        call->callback(parameters, call->user_data);
    prism_portal_call_free(call);
}

void prism_portal_subscribe(const char* path, GCancellable* cancellable, prism_portal_signal_cb callback,
                            void* user_data)
{
    GDBusConnection*          connection = prism_portal_connection();
    struct prism_portal_call* call;

    if(!connection)
        return;

    call               = g_new0(struct prism_portal_call, 1);
    call->request_path = g_strdup(path);
    call->callback     = callback;
    call->user_data    = user_data;
    call->cancellable  = cancellable ? g_object_ref(cancellable) : NULL;
    call->cancelled_id =
        cancellable ? g_signal_connect(cancellable, "cancelled", G_CALLBACK(on_cancelled), call) : 0;
    /* G_DBUS_SIGNAL_FLAGS_NONE, deliberately, and not NO_MATCH_RULE.
     *
     * xdg-desktop-portal exports each Request as an ordinary skeleton and emits
     * Response as a broadcast signal with no destination, so the bus only
     * routes it to us if we have installed a match rule for it. Suppressing the
     * AddMatch saves one round trip per request and loses every reply. */
    call->signal_id = g_dbus_connection_signal_subscribe(
        connection, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request", "Response",
        call->request_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_response, call, NULL);
}
