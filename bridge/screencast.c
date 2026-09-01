/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XDG ScreenCast portal driver. Derived from the screencast portal code in
 * OBS Studio / ShaderGlass WineCap:
 *   Copyright 2022 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *   SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Everything here runs on one thread: the capture thread created by
 * prism_bridge.c with CreateThread(). GLib drives the D-Bus conversation and
 * pumps the PipeWire loop through an attached GSource, so no extra thread and
 * no cross-thread locking is needed.
 */

#include "prism_log.h"
#include "screencast.h"
#include "portal.h"

#include <gio/gunixfdlist.h>
#include <spa/utils/defs.h>

/* org.freedesktop.portal.ScreenCast source-type and cursor-mode bits. They
 * happen to line up with the values Prism exposes, but the translation is kept
 * explicit rather than relying on that. */
#define PORTAL_SOURCE_MONITOR 1u
#define PORTAL_SOURCE_WINDOW  2u
#define PORTAL_SOURCE_VIRTUAL 4u

#define PORTAL_CURSOR_HIDDEN   1u
#define PORTAL_CURSOR_EMBEDDED 2u
#define PORTAL_CURSOR_METADATA 4u

static struct
{
    GDBusProxy*   proxy;
    GMainLoop*    main_loop;
    GCancellable* cancellable;
    char*         session_handle;

    struct pw_loop*    pw_loop;
    struct pw_context* pw_ctx;

    uint32_t                  pipewire_node;
    uint32_t                  available_sources;
    unsigned int              requested_sources;
    unsigned int              requested_cursor;
    struct prism_pw_callbacks callbacks;
} sc;

static void report_state(unsigned int state, const char* message)
{
    if(sc.callbacks.on_state)
        sc.callbacks.on_state(state, message);
}

static uint32_t portal_cached_uint(const char* property)
{
    GVariant* value;
    uint32_t  result;

    if(!sc.proxy)
        return 0;
    value  = g_dbus_proxy_get_cached_property(sc.proxy, property);
    result = value ? g_variant_get_uint32(value) : 0;
    if(value)
        g_variant_unref(value);
    return result;
}

/* --------------------------------------------------- OpenPipeWireRemote -- */

static void on_pipewire_remote_opened(GObject* source, GAsyncResult* res, void* user_data)
{
    GUnixFDList* fd_list = NULL;
    GVariant*    result  = NULL;
    GError*      error   = NULL;
    int          fd_index;
    int          pipewire_fd;

    (void)user_data;

    result = g_dbus_proxy_call_with_unix_fd_list_finish(G_DBUS_PROXY(source), &fd_list, res, &error);
    if(error)
    {
        if(!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            prism_warn("OpenPipeWireRemote failed: %s", error->message);
            report_state(PRISM_STATE_ERROR, error->message);
        }
        g_error_free(error);
        return;
    }

    g_variant_get(result, "(h)", &fd_index);
    pipewire_fd = g_unix_fd_list_get(fd_list, fd_index, &error);
    if(error)
    {
        prism_warn("could not take the PipeWire fd: %s", error->message);
        report_state(PRISM_STATE_ERROR, error->message);
        g_error_free(error);
        goto out;
    }

    if(prism_pipewire_start(sc.pw_loop, sc.pw_ctx, pipewire_fd, sc.pipewire_node, &sc.callbacks) < 0)
        report_state(PRISM_STATE_ERROR, "could not start the PipeWire stream");

out:
    g_clear_object(&fd_list);
    g_clear_pointer(&result, g_variant_unref);
}

static void open_pipewire_remote(void)
{
    GVariantBuilder builder;

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_dbus_proxy_call_with_unix_fd_list(sc.proxy, "OpenPipeWireRemote",
                                        g_variant_new("(oa{sv})", sc.session_handle, &builder),
                                        G_DBUS_CALL_FLAGS_NONE, -1, NULL, sc.cancellable,
                                        on_pipewire_remote_opened, NULL);
}

/* ------------------------------------------------------------- Start() --- */

static void on_start_response(GVariant* parameters, void* user_data)
{
    GVariant*     result  = NULL;
    GVariant*     streams = NULL;
    GVariant*     stream_properties;
    GVariantIter  iter;
    uint32_t      response;
    unsigned      n_streams;
    unsigned      i;

    (void)user_data;
    g_variant_get(parameters, "(u@a{sv})", &response, &result);

    if(response != 0)
    {
        prism_info("source selection cancelled or denied by the user");
        report_state(PRISM_STATE_IDLE, "capture cancelled");
        g_clear_pointer(&result, g_variant_unref);
        return;
    }

    streams = g_variant_lookup_value(result, "streams", G_VARIANT_TYPE_ARRAY);
    if(!streams)
    {
        report_state(PRISM_STATE_ERROR, "portal returned no stream");
        g_clear_pointer(&result, g_variant_unref);
        return;
    }

    g_variant_iter_init(&iter, streams);
    n_streams = g_variant_iter_n_children(&iter);

    /* Prism asks for a single stream. KDE's portal has historically answered
     * with several, where only the last one is the selected source, so skip
     * ahead rather than failing. */
    if(n_streams != 1)
        prism_warn("portal returned %u streams for a single-source request, using the last one", n_streams);
    for(i = 0; i + 1 < n_streams; i++)
    {
        uint32_t  ignored_node;
        GVariant* ignored_props = NULL;
        g_variant_iter_loop(&iter, "(u@a{sv})", &ignored_node, &ignored_props);
    }

    stream_properties = NULL;
    if(g_variant_iter_loop(&iter, "(u@a{sv})", &sc.pipewire_node, &stream_properties))
    {
        prism_info("source selected, PipeWire node %u", sc.pipewire_node);
        open_pipewire_remote();
    }
    else
    {
        report_state(PRISM_STATE_ERROR, "portal returned an empty stream list");
    }

    g_clear_pointer(&streams, g_variant_unref);
    g_clear_pointer(&result, g_variant_unref);
}

static void on_call_finished(GObject* source, GAsyncResult* res, void* user_data)
{
    GVariant* result = NULL;
    GError*   error  = NULL;

    result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
    if(error)
    {
        if(!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            prism_warn("%s failed: %s", (const char*)user_data, error->message);
            report_state(PRISM_STATE_ERROR, error->message);
        }
        g_error_free(error);
    }
    g_clear_pointer(&result, g_variant_unref);
}

static void start_session(void)
{
    GVariantBuilder builder;
    char*           request_token = NULL;
    char*           request_path  = NULL;

    prism_portal_request_path(&request_path, &request_token);
    prism_portal_subscribe(request_path, sc.cancellable, on_start_response, NULL);

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));

    /* An empty parent window handle is correct here: Prism.exe is an X11/Wayland
     * client of Wine, and the portal falls back to a standalone dialog. */
    g_dbus_proxy_call(sc.proxy, "Start", g_variant_new("(osa{sv})", sc.session_handle, "", &builder),
                      G_DBUS_CALL_FLAGS_NONE, -1, sc.cancellable, on_call_finished, (void*)"Start");

    g_free(request_token);
    g_free(request_path);
}

/* ------------------------------------------------------ SelectSources() --- */

static void on_select_sources_response(GVariant* parameters, void* user_data)
{
    GVariant* result = NULL;
    uint32_t  response;

    (void)user_data;
    g_variant_get(parameters, "(u@a{sv})", &response, &result);
    g_clear_pointer(&result, g_variant_unref);

    if(response != 0)
    {
        prism_info("SelectSources denied or cancelled");
        report_state(PRISM_STATE_IDLE, "capture cancelled");
        return;
    }
    start_session();
}

static void select_sources(void)
{
    GVariantBuilder builder;
    uint32_t        cursor_modes;
    uint32_t        types;
    char*           request_token = NULL;
    char*           request_path  = NULL;

    prism_portal_request_path(&request_path, &request_token);
    prism_portal_subscribe(request_path, sc.cancellable, on_select_sources_response, NULL);

    /* Only offer what both Prism and the portal support, otherwise KDE rejects
     * the whole call. */
    types = sc.requested_sources & sc.available_sources;
    if(types == 0)
        types = sc.available_sources;

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(types));
    g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));

    cursor_modes = portal_cached_uint("AvailableCursorModes");
    if(sc.requested_cursor == PRISM_CURSOR_EMBEDDED && (cursor_modes & PORTAL_CURSOR_EMBEDDED))
        g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(PORTAL_CURSOR_EMBEDDED));
    else if(cursor_modes & PORTAL_CURSOR_HIDDEN)
        g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(PORTAL_CURSOR_HIDDEN));

    g_dbus_proxy_call(sc.proxy, "SelectSources", g_variant_new("(oa{sv})", sc.session_handle, &builder),
                      G_DBUS_CALL_FLAGS_NONE, -1, sc.cancellable, on_call_finished, (void*)"SelectSources");

    g_free(request_token);
    g_free(request_path);
}

/* ------------------------------------------------------ CreateSession() --- */

static void on_create_session_response(GVariant* parameters, void* user_data)
{
    GVariant* result = NULL;
    GVariant* handle = NULL;
    uint32_t  response;

    (void)user_data;
    g_variant_get(parameters, "(u@a{sv})", &response, &result);
    if(response != 0)
    {
        prism_info("CreateSession denied or cancelled");
        report_state(PRISM_STATE_IDLE, "capture cancelled");
        g_clear_pointer(&result, g_variant_unref);
        return;
    }

    handle = g_variant_lookup_value(result, "session_handle", NULL);
    if(!handle)
    {
        report_state(PRISM_STATE_ERROR, "portal returned no session handle");
        g_clear_pointer(&result, g_variant_unref);
        return;
    }

    g_clear_pointer(&sc.session_handle, g_free);
    sc.session_handle = g_variant_dup_string(handle, NULL);
    prism_info("ScreenCast session created");

    g_variant_unref(handle);
    g_clear_pointer(&result, g_variant_unref);

    select_sources();
}

static void create_session(void)
{
    GVariantBuilder builder;
    char*           request_token = NULL;
    char*           request_path  = NULL;
    char*           session_token = NULL;

    prism_portal_request_path(&request_path, &request_token);
    prism_portal_session_token(&session_token);
    prism_portal_subscribe(request_path, sc.cancellable, on_create_session_response, NULL);

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
    g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(session_token));

    g_dbus_proxy_call(sc.proxy, "CreateSession", g_variant_new("(a{sv})", &builder), G_DBUS_CALL_FLAGS_NONE, -1,
                      sc.cancellable, on_call_finished, (void*)"CreateSession");

    g_free(session_token);
    g_free(request_token);
    g_free(request_path);
}

/* --------------------------------------------------------------- public -- */

int prism_screencast_init(void)
{
    GError* error = NULL;

    if(sc.proxy)
        return 0;

    if(!prism_portal_connection())
        return -1;

    sc.proxy = g_dbus_proxy_new_sync(prism_portal_connection(), G_DBUS_PROXY_FLAGS_NONE, NULL,
                                     "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                     "org.freedesktop.portal.ScreenCast", NULL, &error);
    if(error)
    {
        prism_warn("no ScreenCast portal on this session bus: %s", error->message);
        g_error_free(error);
        return -1;
    }

    sc.available_sources = portal_cached_uint("AvailableSourceTypes");
    if(sc.available_sources == 0)
    {
        prism_warn("the ScreenCast portal reports no available source types");
        return -1;
    }

    prism_info("ScreenCast portal ready (sources mask 0x%x)", sc.available_sources);
    return 0;
}

unsigned int prism_screencast_available_sources(void)
{
    return sc.available_sources;
}

/* The PipeWire loop is pumped from the GLib loop rather than from a thread of
 * its own, so D-Bus replies and frame callbacks are naturally serialised. */
typedef struct
{
    GSource         base;
    struct pw_loop* loop;
} PrismPwSource;

static gboolean pw_source_dispatch(GSource* source, GSourceFunc callback, gpointer user_data)
{
    PrismPwSource* self = (PrismPwSource*)source;
    int            result;

    (void)callback;
    (void)user_data;

    result = pw_loop_iterate(self->loop, 0);
    if(result < 0)
        prism_warn("pw_loop_iterate failed: %d", result);
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs pw_source_funcs = {
    NULL, NULL, pw_source_dispatch, NULL, NULL, NULL,
};

void prism_screencast_run(void)
{
    PrismPwSource* source;

    sc.main_loop = g_main_loop_new(NULL, FALSE);

    pw_init(NULL, NULL);
    sc.pw_loop = pw_loop_new(NULL);
    sc.pw_ctx  = pw_context_new(sc.pw_loop, NULL, 0);

    source       = (PrismPwSource*)g_source_new(&pw_source_funcs, sizeof(PrismPwSource));
    source->loop = sc.pw_loop;
    g_source_add_unix_fd(&source->base, pw_loop_get_fd(sc.pw_loop), G_IO_IN | G_IO_ERR);
    g_source_attach(&source->base, NULL);
    g_source_unref(&source->base);

    pw_loop_enter(sc.pw_loop);
    g_main_loop_run(sc.main_loop);
    pw_loop_leave(sc.pw_loop);

    prism_pipewire_stop();
    pw_context_destroy(sc.pw_ctx);
    pw_loop_destroy(sc.pw_loop);
    g_main_loop_unref(sc.main_loop);
    sc.pw_ctx    = NULL;
    sc.pw_loop   = NULL;
    sc.main_loop = NULL;
}

static gboolean idle_create_session(gpointer data)
{
    (void)data;
    create_session();
    return G_SOURCE_REMOVE;
}

void prism_screencast_start(unsigned int source_types, unsigned int cursor_mode,
                            const struct prism_pw_callbacks* callbacks)
{
    sc.requested_sources = source_types;
    sc.requested_cursor  = cursor_mode;
    sc.callbacks         = *callbacks;
    sc.cancellable       = g_cancellable_new();

    report_state(PRISM_STATE_NEGOTIATING, "waiting for source selection");

    /* Hop onto the loop thread: everything D-Bus touches must stay there. */
    g_idle_add(idle_create_session, NULL);
}

static gboolean idle_stop(gpointer data)
{
    (void)data;
    prism_pipewire_stop();
    return G_SOURCE_REMOVE;
}

void prism_screencast_stop(void)
{
    if(sc.cancellable)
        g_cancellable_cancel(sc.cancellable);

    if(sc.session_handle)
    {
        GDBusConnection* connection = prism_portal_connection();
        if(connection)
            g_dbus_connection_call(connection, "org.freedesktop.portal.Desktop", sc.session_handle,
                                   "org.freedesktop.portal.Session", "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                                   -1, NULL, NULL, NULL);
        g_clear_pointer(&sc.session_handle, g_free);
    }

    if(sc.main_loop)
        g_idle_add(idle_stop, NULL);
    else
        prism_pipewire_stop();

    g_clear_object(&sc.cancellable);
    sc.callbacks.on_frame  = NULL;
    sc.callbacks.on_format = NULL;
    sc.callbacks.on_state  = NULL;
}

void prism_screencast_quit(void)
{
    if(sc.main_loop)
        g_main_loop_quit(sc.main_loop);
}
