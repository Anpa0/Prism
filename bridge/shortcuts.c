/*
 * Prism - Proton/Wine screen-capture host with ReShade support
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XDG GlobalShortcuts portal client.
 *
 * This is the Wayland-correct way to get a hotkey that fires while another
 * window has focus. There is no X11 key grab anywhere in Prism: under Wayland
 * the compositor owns the keyboard, and KWin only delivers shortcuts an
 * application registered through this portal and the user approved.
 *
 * The handshake is:
 *
 *   Registry.Register(app_id)      (portal.c, once per connection, first)
 *   CreateSession   -> Response(session_handle)
 *   BindShortcuts   -> Response(shortcuts)     <- KDE shows its bind dialog
 *   Activated signal -> Prism.exe
 *
 * Everything here runs on the bridge's capture thread alongside the ScreenCast
 * conversation, so the two never race.
 */

#include "prism_log.h"
#include "shortcuts.h"
#include "portal.h"

#define PRISM_SHORTCUTS_IFACE "org.freedesktop.portal.GlobalShortcuts"

struct shortcut_copy
{
    char* id;
    char* description;
    char* trigger;
};

static struct
{
    GMutex mutex; /* guards state, message and bindings */

    unsigned int state;
    char         message[512];

    PrismShortcutBinding bindings[PRISM_SHORTCUT_MAX];
    unsigned int         binding_count;

    /* Loop-thread only below this line. */
    struct shortcut_copy requested[PRISM_SHORTCUT_MAX];
    unsigned int         requested_count;
    char*                session_handle;
    guint                activated_id;
    guint                changed_id;
    GCancellable*        cancellable;

    prism_shortcut_activated_cb callback;
    int                         initialised;
} sh;

/* ---------------------------------------------------------------- state -- */

static void set_state(unsigned int state, const char* message)
{
    g_mutex_lock(&sh.mutex);
    sh.state = state;
    g_strlcpy(sh.message, message ? message : "", sizeof(sh.message));
    g_mutex_unlock(&sh.mutex);

    if(state == PRISM_SHORTCUTS_ERROR || state == PRISM_SHORTCUTS_DENIED)
        prism_warn("global shortcuts: %s", message ? message : "unavailable");
    else
        prism_info("global shortcuts: %s", message ? message : "");
}

unsigned int prism_shortcuts_status(char* message, unsigned int message_bytes)
{
    unsigned int state;

    g_mutex_lock(&sh.mutex);
    state = sh.state;
    if(message && message_bytes)
        g_strlcpy(message, sh.message, message_bytes);
    g_mutex_unlock(&sh.mutex);
    return state;
}

unsigned int prism_shortcuts_bindings(PrismShortcutBinding* out, unsigned int max)
{
    unsigned int count;

    if(!out || max == 0)
        return 0;

    g_mutex_lock(&sh.mutex);
    count = sh.binding_count < max ? sh.binding_count : max;
    memcpy(out, sh.bindings, count * sizeof(PrismShortcutBinding));
    g_mutex_unlock(&sh.mutex);
    return count;
}

/* Turns the a(sa{sv}) the portal returns into the flat list the UI shows. */
static void store_bindings(GVariant* shortcuts)
{
    GVariantIter iter;
    const char*  id;
    GVariant*    properties;
    unsigned int index = 0;

    if(!shortcuts)
        return;

    g_mutex_lock(&sh.mutex);
    sh.binding_count = 0;
    g_variant_iter_init(&iter, shortcuts);
    while(index < PRISM_SHORTCUT_MAX && g_variant_iter_loop(&iter, "(&s@a{sv})", &id, &properties))
    {
        const char* trigger = NULL;

        /* trigger_description is what the compositor actually bound, which may
         * differ from what we asked for - the user can change it in the dialog. */
        if(!g_variant_lookup(properties, "trigger_description", "&s", &trigger))
            trigger = "(unreported)";

        g_strlcpy(sh.bindings[index].id, id, PRISM_SHORTCUT_ID_MAX);
        g_strlcpy(sh.bindings[index].trigger, trigger, PRISM_SHORTCUT_TRIGGER_MAX);
        index++;
    }
    sh.binding_count = index;
    g_mutex_unlock(&sh.mutex);

    for(unsigned int i = 0; i < index; i++)
        prism_info("  bound %s -> %s", sh.bindings[i].id, sh.bindings[i].trigger);
}

/* ------------------------------------------------------------- signals -- */

static void on_activated(GDBusConnection* connection, const char* sender, const char* path, const char* interface,
                         const char* signal, GVariant* parameters, void* user_data)
{
    const char*        session_handle = NULL;
    const char*        shortcut_id    = NULL;
    unsigned long long timestamp      = 0;
    GVariant*          options        = NULL;

    (void)connection;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    (void)user_data;

    g_variant_get(parameters, "(&o&st@a{sv})", &session_handle, &shortcut_id, &timestamp, &options);
    g_clear_pointer(&options, g_variant_unref);

    /* One process can hold several portal sessions; only ours is interesting. */
    if(!sh.session_handle || g_strcmp0(session_handle, sh.session_handle) != 0)
        return;

    prism_debug("shortcut activated: %s", shortcut_id);
    if(sh.callback)
        sh.callback(shortcut_id);
}

static void on_shortcuts_changed(GDBusConnection* connection, const char* sender, const char* path,
                                 const char* interface, const char* signal, GVariant* parameters, void* user_data)
{
    const char* session_handle = NULL;
    GVariant*   shortcuts      = NULL;

    (void)connection;
    (void)sender;
    (void)path;
    (void)interface;
    (void)signal;
    (void)user_data;

    g_variant_get(parameters, "(&o@a(sa{sv}))", &session_handle, &shortcuts);
    if(sh.session_handle && g_strcmp0(session_handle, sh.session_handle) == 0)
    {
        prism_info("the compositor changed our shortcut bindings");
        store_bindings(shortcuts);
    }
    g_clear_pointer(&shortcuts, g_variant_unref);
}

static void subscribe_signals(void)
{
    GDBusConnection* connection = prism_portal_connection();

    if(!connection || sh.activated_id)
        return;

    sh.activated_id = g_dbus_connection_signal_subscribe(
        connection, "org.freedesktop.portal.Desktop", PRISM_SHORTCUTS_IFACE, "Activated",
        "/org/freedesktop/portal/desktop", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_activated, NULL, NULL);
    sh.changed_id = g_dbus_connection_signal_subscribe(
        connection, "org.freedesktop.portal.Desktop", PRISM_SHORTCUTS_IFACE, "ShortcutsChanged",
        "/org/freedesktop/portal/desktop", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_shortcuts_changed, NULL, NULL);
}

/* --------------------------------------------------------- BindShortcuts -- */

static void on_bind_response(GVariant* parameters, void* user_data)
{
    GVariant*    result = NULL;
    GVariant*    shortcuts;
    unsigned int response;

    (void)user_data;
    g_variant_get(parameters, "(u@a{sv})", &response, &result);

    if(response != 0)
    {
        set_state(PRISM_SHORTCUTS_DENIED,
                  "The shortcut request was declined. Re-run Prism after clearing the stored decision "
                  "with: flatpak permission-reset " PRISM_APP_ID);
        g_clear_pointer(&result, g_variant_unref);
        return;
    }

    shortcuts = g_variant_lookup_value(result, "shortcuts", G_VARIANT_TYPE("a(sa{sv})"));
    store_bindings(shortcuts);
    g_clear_pointer(&shortcuts, g_variant_unref);
    g_clear_pointer(&result, g_variant_unref);

    set_state(PRISM_SHORTCUTS_BOUND, "bound by the compositor");
}

static void on_call_finished(GObject* source, GAsyncResult* res, void* user_data)
{
    GVariant* result = NULL;
    GError*   error  = NULL;

    result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if(error)
    {
        if(!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            char detail[512];

            /* The one failure worth explaining precisely: xdg-desktop-portal
             * 1.21+ refuses a shortcuts session whose connection has no app id,
             * which happens when Registry.Register did not take. */
            if(strstr(error->message, "app id is required") != NULL)
                g_snprintf(detail, sizeof(detail),
                           "%s failed: the portal wants an app id. Install packaging/" PRISM_APP_ID
                           ".desktop into ~/.local/share/applications and restart Prism.",
                           (const char*)user_data);
            else
                g_snprintf(detail, sizeof(detail), "%s failed: %s", (const char*)user_data, error->message);

            set_state(PRISM_SHORTCUTS_ERROR, detail);
        }
        g_error_free(error);
    }
    g_clear_pointer(&result, g_variant_unref);
}

static void bind_shortcuts(void)
{
    GVariantBuilder shortcut_builder;
    GVariantBuilder options_builder;
    char*           request_token = NULL;
    char*           request_path  = NULL;

    prism_portal_request_path(&request_path, &request_token);
    prism_portal_subscribe(request_path, sh.cancellable, on_bind_response, NULL);

    g_variant_builder_init(&shortcut_builder, G_VARIANT_TYPE("a(sa{sv})"));
    for(unsigned int i = 0; i < sh.requested_count; i++)
    {
        GVariantBuilder properties;

        g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&properties, "{sv}", "description",
                              g_variant_new_string(sh.requested[i].description));
        if(sh.requested[i].trigger && *sh.requested[i].trigger)
            g_variant_builder_add(&properties, "{sv}", "preferred_trigger",
                                  g_variant_new_string(sh.requested[i].trigger));
        g_variant_builder_add(&shortcut_builder, "(sa{sv})", sh.requested[i].id, &properties);
    }

    g_variant_builder_init(&options_builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options_builder, "{sv}", "handle_token", g_variant_new_string(request_token));

    /* An empty parent window handle: Prism's window belongs to Wine, so there
     * is no Wayland handle to export, and the portal falls back to a dialog. */
    g_dbus_connection_call(prism_portal_connection(), "org.freedesktop.portal.Desktop",
                           "/org/freedesktop/portal/desktop", PRISM_SHORTCUTS_IFACE, "BindShortcuts",
                           g_variant_new("(oa(sa{sv})sa{sv})", sh.session_handle, &shortcut_builder, "",
                                         &options_builder),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, sh.cancellable, on_call_finished,
                           (void*)"BindShortcuts");

    g_free(request_token);
    g_free(request_path);
}

/* --------------------------------------------------------- CreateSession -- */

static void on_create_session_response(GVariant* parameters, void* user_data)
{
    GVariant*    result = NULL;
    const char*  handle = NULL;
    unsigned int response;

    (void)user_data;
    g_variant_get(parameters, "(u@a{sv})", &response, &result);

    if(response != 0)
    {
        set_state(PRISM_SHORTCUTS_DENIED, "the shortcuts session was refused");
        g_clear_pointer(&result, g_variant_unref);
        return;
    }

    /* Declared as 's' rather than 'o' in the portal interface, which upstream
     * documents as a mistake kept for compatibility. */
    if(!g_variant_lookup(result, "session_handle", "&s", &handle))
    {
        set_state(PRISM_SHORTCUTS_ERROR, "the portal returned no session handle");
        g_clear_pointer(&result, g_variant_unref);
        return;
    }

    g_clear_pointer(&sh.session_handle, g_free);
    sh.session_handle = g_strdup(handle);
    g_clear_pointer(&result, g_variant_unref);

    subscribe_signals();
    bind_shortcuts();
}

static void create_session(void)
{
    GVariantBuilder builder;
    char*           request_token = NULL;
    char*           request_path  = NULL;
    char*           session_token = NULL;

    prism_portal_request_path(&request_path, &request_token);
    prism_portal_session_token(&session_token);
    prism_portal_subscribe(request_path, sh.cancellable, on_create_session_response, NULL);

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
    g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(session_token));

    g_dbus_connection_call(prism_portal_connection(), "org.freedesktop.portal.Desktop",
                           "/org/freedesktop/portal/desktop", PRISM_SHORTCUTS_IFACE, "CreateSession",
                           g_variant_new("(a{sv})", &builder), NULL, G_DBUS_CALL_FLAGS_NONE, -1, sh.cancellable,
                           on_call_finished, (void*)"CreateSession");

    g_free(session_token);
    g_free(request_token);
    g_free(request_path);
}

/* Does this session have a GlobalShortcuts portal at all? wlroots-based
 * portals ship no implementation, and asking is cheaper than failing. */
static int portal_supports_shortcuts(void)
{
    GDBusConnection* connection = prism_portal_connection();
    GVariant*        reply;
    GError*          error = NULL;

    if(!connection)
        return 0;

    reply = g_dbus_connection_call_sync(
        connection, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", PRISM_SHORTCUTS_IFACE, "version"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE,
        3000, NULL, &error);

    if(error)
    {
        prism_info("no GlobalShortcuts portal in this session: %s", error->message);
        g_error_free(error);
        return 0;
    }

    {
        GVariant* boxed = NULL;
        g_variant_get(reply, "(v)", &boxed);
        prism_info("GlobalShortcuts portal version %u", boxed ? g_variant_get_uint32(boxed) : 0);
        g_clear_pointer(&boxed, g_variant_unref);
    }
    g_variant_unref(reply);
    return 1;
}

/* ---------------------------------------------------------------- entry -- */

static gboolean idle_start(gpointer data)
{
    (void)data;

    if(!portal_supports_shortcuts())
    {
        set_state(PRISM_SHORTCUTS_UNAVAILABLE,
                  "This session has no GlobalShortcuts portal. Prism's in-window hotkeys still work.");
        return G_SOURCE_REMOVE;
    }

    if(!prism_portal_app_id_registered())
        prism_warn("the host portal did not accept our app id; the shortcuts session may be refused");

    set_state(PRISM_SHORTCUTS_PENDING, "waiting for the compositor's shortcut dialog");
    create_session();
    return G_SOURCE_REMOVE;
}

void prism_shortcuts_start(const PrismShortcutSpec* shortcuts, unsigned int count,
                           prism_shortcut_activated_cb callback)
{
    if(!sh.initialised)
    {
        g_mutex_init(&sh.mutex);
        sh.initialised = 1;
    }

    if(count > PRISM_SHORTCUT_MAX)
        count = PRISM_SHORTCUT_MAX;

    /* Copy the specs: the caller's strings live in Prism.exe and the loop
     * thread reads them later. */
    for(unsigned int i = 0; i < sh.requested_count; i++)
    {
        g_clear_pointer(&sh.requested[i].id, g_free);
        g_clear_pointer(&sh.requested[i].description, g_free);
        g_clear_pointer(&sh.requested[i].trigger, g_free);
    }
    for(unsigned int i = 0; i < count; i++)
    {
        sh.requested[i].id          = g_strdup(shortcuts[i].id);
        sh.requested[i].description = g_strdup(shortcuts[i].description);
        sh.requested[i].trigger     = g_strdup(shortcuts[i].preferred_trigger);
    }
    sh.requested_count = count;
    sh.callback        = callback;

    if(!sh.cancellable)
        sh.cancellable = g_cancellable_new();

    g_idle_add(idle_start, NULL);
}

static gboolean idle_configure(gpointer data)
{
    GVariantBuilder builder;

    (void)data;
    if(!sh.session_handle)
        return G_SOURCE_REMOVE;

    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_dbus_connection_call(prism_portal_connection(), "org.freedesktop.portal.Desktop",
                           "/org/freedesktop/portal/desktop", PRISM_SHORTCUTS_IFACE, "ConfigureShortcuts",
                           g_variant_new("(osa{sv})", sh.session_handle, "", &builder), NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, sh.cancellable, on_call_finished,
                           (void*)"ConfigureShortcuts");
    return G_SOURCE_REMOVE;
}

void prism_shortcuts_configure(void)
{
    if(sh.initialised)
        g_idle_add(idle_configure, NULL);
}

static gboolean idle_stop(gpointer data)
{
    GDBusConnection* connection = prism_portal_connection();

    (void)data;

    if(connection && sh.activated_id)
    {
        g_dbus_connection_signal_unsubscribe(connection, sh.activated_id);
        sh.activated_id = 0;
    }
    if(connection && sh.changed_id)
    {
        g_dbus_connection_signal_unsubscribe(connection, sh.changed_id);
        sh.changed_id = 0;
    }
    if(connection && sh.session_handle)
    {
        g_dbus_connection_call(connection, "org.freedesktop.portal.Desktop", sh.session_handle,
                               "org.freedesktop.portal.Session", "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                               NULL, NULL, NULL);
        g_clear_pointer(&sh.session_handle, g_free);
    }
    return G_SOURCE_REMOVE;
}

void prism_shortcuts_stop(void)
{
    if(!sh.initialised)
        return;

    sh.callback = NULL;
    if(sh.cancellable)
        g_cancellable_cancel(sh.cancellable);
    g_clear_object(&sh.cancellable);
    g_idle_add(idle_stop, NULL);
    set_state(PRISM_SHORTCUTS_UNAVAILABLE, "stopped");
}
