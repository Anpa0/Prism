# Global hotkeys

Prism has two global actions:

| Default | Action |
| --- | --- |
| `Ctrl+Shift+F11` | Switch between **gameplay** and **configuration** mode |
| `Ctrl+Shift+F12` | Hide the gameplay output immediately |

Both are configurable in `Prism.ini` (`HotkeyToggleMode`, `HotkeyHideOutput`),
and on KDE the compositor's own shortcut editor can rebind them — reachable from
**View → Configure Global Shortcuts…**.

## Why this is not `RegisterHotKey`

Under Wayland the compositor owns the keyboard. An application cannot grab a
key, and there is no X11 path worth designing around: `RegisterHotKey` inside
Wine only sees keys that already reached Prism's window, which is exactly the
case Prism needs to work in — the game has focus, not Prism.

The sanctioned mechanism is the **XDG GlobalShortcuts portal**. Prism's bridge
drives it natively:

```
Registry.Register("net.prism.Prism")          announce who we are
GlobalShortcuts.CreateSession                 open a session
GlobalShortcuts.BindShortcuts                 KWin shows its approval dialog
GlobalShortcuts.Activated  ---------------->  fires whatever has focus
```

`RegisterHotKey` remains as a fallback, armed only after the portal has
definitively failed, so a bound shortcut never fires twice. In a Wayland session
that fallback is focus-bound and therefore near-useless for gameplay mode; the
diagnostics window says so plainly when it is in use.

## The app-id requirement — read this if hotkeys do not bind

xdg-desktop-portal **1.20** added `org.freedesktop.host.portal.Registry`, and
**1.21** made `GlobalShortcuts.CreateSession` fail outright when the calling
connection has no application id:

```
org.freedesktop.portal.Error.NotAllowed: An app id is required
```

Prism registers as `net.prism.Prism` before any other portal call, which is why
`Registry.Register` happens the instant the bridge opens the session bus. The
portal additionally expects that id to match the basename of an installed
`.desktop` file, and GNOME's backend enforces it strictly. Install it once:

```sh
./scripts/install-desktop-file.sh
```

That writes `~/.local/share/applications/net.prism.Prism.desktop` and nothing
else; `--remove` undoes it.

## First run

KDE shows a one-time dialog listing Prism's two shortcuts and letting you accept
or change the keys. Accept it. **A denied decision is remembered**, and later
attempts then fail silently. To be asked again:

```sh
flatpak permission-reset net.prism.Prism
```

That permission store is shared with non-Flatpak applications, so this works
even though Prism is not a Flatpak.

## Checking what actually bound

**View → Diagnostics** reports the truth:

```
Global shortcuts
  Portal state:         bound
  Detail:               bound by the compositor
  Bound:                toggle-mode = Ctrl+Shift+F11, hide-output = Ctrl+Shift+F12
  Requested:            CTRL+SHIFT+F11 / CTRL+SHIFT+F12
  RegisterHotKey:       not needed
```

`Bound` is what the compositor decided, which may differ from what Prism asked
for — the user gets the final say in the bind dialog, and Prism displays the
result rather than assuming.

Other states and what they mean:

| Portal state | Meaning |
| --- | --- |
| `unavailable` | This session has no GlobalShortcuts backend. wlroots-based portals ship none; KDE and GNOME do. |
| `waiting for the compositor` | The bind dialog is open, or the request is in flight. |
| `denied by the user` | The dialog was dismissed. Reset the permission as above. |
| `error` | The message names the cause; an app-id error means the `.desktop` file is missing. |

## Trigger syntax

Triggers follow the XDG shortcuts specification: modifier names joined with `+`,
then a key.

```
CTRL+SHIFT+F11
ALT+F9
LOGO+SHIFT+P
```

Recognised modifiers are `CTRL`, `SHIFT`, `ALT` and `LOGO`. The `RegisterHotKey`
fallback parses the same strings, so one setting drives both paths.

## Testing without a compositor

The portal conversation is a D-Bus exchange, not a key event, so it can be
driven by a stand-in. `tests/mock_globalshortcuts_portal.py` implements enough
of the portal to run Prism through the whole handshake and then fire both
shortcuts:

```sh
dbus-run-session -- sh -c \
  'python3 tests/mock_globalshortcuts_portal.py & sleep 2; \
   cd build/dist && wine ./Prism.exe --test-pattern --gameplay'
```

Expect to see `Registry.Register`, `CreateSession`, `BindShortcuts`, then Prism
switching to configuration mode and dropping its gameplay output.
