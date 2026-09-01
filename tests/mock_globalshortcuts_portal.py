#!/usr/bin/env python3
"""
Prism - a stand-in for xdg-desktop-portal's GlobalShortcuts interface.
SPDX-License-Identifier: GPL-3.0-or-later

Prism's global hotkeys are the one subsystem that cannot be exercised without a
compositor: they arrive as a D-Bus signal from the portal, not as a key event.
This mock owns org.freedesktop.portal.Desktop on a private session bus and
implements just enough of the real thing to drive Prism through the whole
handshake, then fires the shortcuts on a timer:

    Registry.Register(app_id)
    GlobalShortcuts.CreateSession   -> Request::Response(session_handle)
    GlobalShortcuts.BindShortcuts   -> Request::Response(shortcuts)
    GlobalShortcuts.Activated       -> toggle-mode, then hide-output

It is a test double, not a portal: it approves everything and asks no one. Run
it inside `dbus-run-session` so it can never take over a real desktop's bus.

    dbus-run-session -- sh -c 'python3 tests/mock_globalshortcuts_portal.py & \
                              sleep 2; wine ./Prism.exe --test-pattern --gameplay'
"""

import sys

import dbus
import dbus.service
import dbus.lowlevel
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

BUS_NAME = "org.freedesktop.portal.Desktop"
OBJECT_PATH = "/org/freedesktop/portal/desktop"
SHORTCUTS_IFACE = "org.freedesktop.portal.GlobalShortcuts"
REGISTRY_IFACE = "org.freedesktop.host.portal.Registry"
REQUEST_IFACE = "org.freedesktop.portal.Request"

# How long after binding to start firing shortcuts, and the gap between them.
FIRST_ACTIVATION_MS = 6000
ACTIVATION_GAP_MS = 4000


def request_path(sender, token):
    """Rebuild the Request object path the client is listening on.

    The portal derives it from the caller's unique bus name with the leading
    ':' removed and dots replaced by underscores, plus the caller's own
    handle_token. Prism computes the same string in bridge/portal.c.
    """
    return "{}/request/{}/{}".format(OBJECT_PATH, sender[1:].replace(".", "_"), token)


class MockPortal(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, OBJECT_PATH)
        self.bus = bus
        self.app_id = None
        self.sessions = {}
        self.session_counter = 0

    # ---------------------------------------------------------- Registry --

    @dbus.service.method(REGISTRY_IFACE, in_signature="sa{sv}", out_signature="",
                         sender_keyword="sender")
    def Register(self, app_id, options, sender=None):
        if self.app_id is not None:
            raise dbus.DBusException("Connection already associated with an application ID")
        self.app_id = str(app_id)
        print("Registry.Register({!r}) from {}".format(self.app_id, sender), flush=True)

    # --------------------------------------------------- GlobalShortcuts --

    @dbus.service.method(SHORTCUTS_IFACE, in_signature="a{sv}", out_signature="o",
                         sender_keyword="sender")
    def CreateSession(self, options, sender=None):
        # The real portal refuses a session whose connection has no app id;
        # mirroring that is the whole point of testing against this mock.
        if not self.app_id:
            raise dbus.DBusException("An app id is required")

        token = str(options.get("handle_token", "t"))
        session_token = str(options.get("session_handle_token", "s"))
        handle = request_path(sender, token)

        self.session_counter += 1
        session_handle = "{}/session/{}/{}".format(
            OBJECT_PATH, sender[1:].replace(".", "_"), session_token)
        self.sessions[session_handle] = sender
        print("CreateSession -> {}".format(session_handle), flush=True)

        GLib.timeout_add(50, self._respond, handle,
                         {"session_handle": dbus.String(session_handle)})
        return handle

    @dbus.service.method(SHORTCUTS_IFACE, in_signature="oa(sa{sv})sa{sv}", out_signature="o",
                         sender_keyword="sender")
    def BindShortcuts(self, session_handle, shortcuts, parent_window, options, sender=None):
        token = str(options.get("handle_token", "t"))
        handle = request_path(sender, token)

        bound = dbus.Array(signature="(sa{sv})")
        ids = []
        for shortcut_id, properties in shortcuts:
            shortcut_id = str(shortcut_id)
            ids.append(shortcut_id)
            trigger = str(properties.get("preferred_trigger", "CTRL+SHIFT+F1"))
            print("  bind {!r} desc={!r} trigger={!r}".format(
                shortcut_id, str(properties.get("description", "")), trigger), flush=True)
            bound.append(dbus.Struct(
                (dbus.String(shortcut_id),
                 dbus.Dictionary({
                     "description": dbus.String(str(properties.get("description", ""))),
                     "trigger_description": dbus.String(trigger),
                 }, signature="sv")),
                signature="sa{sv}"))

        GLib.timeout_add(50, self._respond, handle, {"shortcuts": bound})

        # Fire each bound shortcut once so Prism's whole path runs: portal
        # signal -> bridge callback -> PostMessage -> mode switch.
        delay = FIRST_ACTIVATION_MS
        for shortcut_id in ids:
            GLib.timeout_add(delay, self._activate, str(session_handle), shortcut_id)
            delay += ACTIVATION_GAP_MS
        return handle

    @dbus.service.method(SHORTCUTS_IFACE, in_signature="oa{sv}", out_signature="o",
                         sender_keyword="sender")
    def ListShortcuts(self, session_handle, options, sender=None):
        handle = request_path(sender, str(options.get("handle_token", "t")))
        GLib.timeout_add(50, self._respond, handle,
                         {"shortcuts": dbus.Array(signature="(sa{sv})")})
        return handle

    @dbus.service.method(SHORTCUTS_IFACE, in_signature="osa{sv}", out_signature="")
    def ConfigureShortcuts(self, session_handle, parent_window, options):
        print("ConfigureShortcuts (a real portal would open its editor)", flush=True)

    @dbus.service.signal(SHORTCUTS_IFACE, signature="osta{sv}")
    def Activated(self, session_handle, shortcut_id, timestamp, options):
        pass

    # ------------------------------------------------------------ Session --

    @dbus.service.method("org.freedesktop.portal.Session", in_signature="", out_signature="")
    def Close(self):
        print("Session.Close", flush=True)

    # --------------------------------------------------------- Properties --

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface, prop):
        if prop != "version":
            raise dbus.DBusException("no such property: " + str(prop))
        if interface == SHORTCUTS_IFACE:
            return dbus.UInt32(2)
        if interface == REGISTRY_IFACE:
            return dbus.UInt32(1)
        raise dbus.DBusException("no such interface: " + str(interface))

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface in (SHORTCUTS_IFACE, REGISTRY_IFACE):
            return {"version": dbus.UInt32(2 if interface == SHORTCUTS_IFACE else 1)}
        return {}

    # ----------------------------------------------------------- internals --

    def _respond(self, handle, results):
        """Emit Request::Response from `handle`, which we do not own as an object."""
        message = dbus.lowlevel.SignalMessage(handle, REQUEST_IFACE, "Response")
        message.append(dbus.UInt32(0), dbus.Dictionary(results, signature="sv"),
                       signature="ua{sv}")
        self.bus.send_message(message)
        print("Response(0) -> {}".format(handle), flush=True)
        return GLib.SOURCE_REMOVE

    def _activate(self, session_handle, shortcut_id):
        print("*** Activated {} ***".format(shortcut_id), flush=True)
        self.Activated(dbus.ObjectPath(session_handle), shortcut_id,
                       dbus.UInt64(0), dbus.Dictionary({}, signature="sv"))
        return GLib.SOURCE_REMOVE


def main():
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    if bus.request_name(BUS_NAME) not in (
            dbus.bus.REQUEST_NAME_REPLY_PRIMARY_OWNER,
            dbus.bus.REQUEST_NAME_REPLY_ALREADY_OWNER):
        sys.exit("could not take {} - run this inside dbus-run-session".format(BUS_NAME))

    MockPortal(bus)
    print("mock GlobalShortcuts portal ready on {}".format(BUS_NAME), flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
