import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import qs.Ui
import qs.Commons

// OmaQuake bar widget.
//
// The popout is NOT a QML reimplementation of a terminal. It is a real
// terminal emulator window (foot) running the real `omaquake` binary,
// positioned by Hyprland to sit exactly behind a transparent hole in this
// layer-shell surface. The chrome around the hole is drawn here; the picture
// inside it is a tty.
//
// Two things make that work, both verified on Hyprland 0.56.2:
//
//   1. A layer-shell surface on the OVERLAY layer composites over toplevels,
//      so a region this surface never paints shows the window underneath.
//      The ring below is drawn as four bars for exactly this reason -- a
//      child Rectangle with color:"transparent" does NOT punch a hole, it
//      just declines to paint over its parent's fill.
//
//   2. `mask` subtracts the hole from the input region so clicks and keys
//      reach the terminal instead of being swallowed here.
Panel {
    id: root

    moduleName: "rsd.omaquake"
    ipcTarget: "rsd.omaquake"

    // --- tunables ------------------------------------------------------
    // Sized in terminal cells; the pixel geometry is whatever foot decides
    // those cells cost on this output. Fixing cells rather than pixels keeps
    // Quake framed identically across monitors of different scale.
    readonly property int cols: setting("cols", 100)
    readonly property int rows: setting("rows", 30)
    readonly property int padding: setting("padding", 10)
    readonly property int gap: setting("gap", 6)

    readonly property string binary: setting("binary", "omaquake")
    readonly property string gameDir: setting("gameDir", "")
    readonly property string pak: setting("pak", "")

    readonly property string appId: "omaquake-popout"

    // Cell size in logical px. foot's default font lands near this; the
    // window is sized from it and then corrected once Hyprland reports the
    // real geometry back.
    readonly property int cellW: setting("cellW", 8)
    readonly property int cellH: setting("cellH", 17)

    readonly property int termW: cols * cellW
    readonly property int termH: rows * cellH

    // --- bar button ----------------------------------------------------
    // Ui/Panel.qml carries no geometry of its own, so the widget is invisible
    // in the bar unless the root forwards the button's implicit size.
    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight

    BarIconButton {
        id: button
        anchors.fill: parent
        bar: root.bar
        iconComponent: Component {
            QuakeIcon {
                iconSize: Style.bar.iconCanvas * 1.15
                color: button.foreground
            }
        }
        onPressed: function (b) { root.activate() }
    }

    // --- the surface the hole lives in ---------------------------------
    PanelWindow {
        id: surface

        // Follow the bar this widget instance belongs to, so the popout lands
        // on the monitor the icon was clicked on rather than a fixed output.
        screen: {
            var w = button.QsWindow ? button.QsWindow.window : null
            return w ? w.screen : null
        }

        visible: root.opened
        color: "transparent"
        exclusionMode: ExclusionMode.Ignore
        anchors { top: true; left: true; right: true; bottom: true }

        WlrLayershell.layer: WlrLayer.Overlay
        // The terminal below owns the keyboard; this surface must never take
        // it, or keystrokes stop reaching the game.
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        WlrLayershell.namespace: "omaquake-popout-chrome"

        readonly property int barH: {
            var w = button.QsWindow ? button.QsWindow.window : null
            return w ? w.height : 26
        }

        // Horizontally centred on the bar icon, clamped to the screen.
        readonly property int holeX: {
            var w = button.QsWindow ? button.QsWindow.window : null
            if (!w || !surface.screen) return 0
            var p = button.mapToItem(w.contentItem, 0, 0)
            var want = p.x + button.width / 2 - root.termW / 2
            var max = surface.screen.width - root.termW - root.padding
            return Math.round(Math.max(root.padding, Math.min(want, max)))
        }
        readonly property int holeY: Math.round(barH + root.gap)

        // Global compositor coordinates for Hyprland. Verified 1:1 with
        // logical coords on a scale-2 output.
        readonly property int globalX: (surface.screen ? surface.screen.x : 0) + holeX
        readonly property int globalY: (surface.screen ? surface.screen.y : 0) + holeY

        mask: Region {
            x: surface.holeX - root.padding
            y: surface.holeY - root.padding
            width: root.termW + root.padding * 2
            height: root.termH + root.padding * 2
            intersection: Intersection.Combine
            regions: [
                Region {
                    x: surface.holeX
                    y: surface.holeY
                    width: root.termW
                    height: root.termH
                    intersection: Intersection.Subtract
                }
            ]
        }

        // Chrome: four bars around a centre that is never painted.
        Item {
            id: frame

            x: surface.holeX - root.padding
            y: surface.holeY - root.padding
            width: root.termW + root.padding * 2
            height: root.termH + root.padding * 2

            readonly property color chrome: Color ? Color.popups.background : "#1a1b26"
            readonly property color edge: Color ? Color.popups.border : "#7aa2f7"

            Rectangle { x: 0; y: 0; width: parent.width; height: root.padding; color: frame.chrome }
            Rectangle { x: 0; y: parent.height - root.padding; width: parent.width; height: root.padding; color: frame.chrome }
            Rectangle { x: 0; y: root.padding; width: root.padding; height: parent.height - root.padding * 2; color: frame.chrome }
            Rectangle { x: parent.width - root.padding; y: root.padding; width: root.padding; height: parent.height - root.padding * 2; color: frame.chrome }

            // Transparent fill + border paints the outline only.
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                radius: 10
                border.color: frame.edge
                border.width: 1
            }
        }
    }

    // --- the engine binary ----------------------------------------------
    // A plugin cannot declare a dependency on it: the manifest schema has no
    // field for one, and `omarchy plugin add` only clones and validates -- it
    // never builds anything, runs a hook, or calls a package manager. So a
    // missing engine has to be diagnosed here, where there is still someone to
    // tell, rather than showing up as a chrome ring over an empty hole that
    // the watchdog clears five seconds later with no explanation.
    property bool binaryMissing: false

    Process {
        id: binaryProbe
        onExited: function (code) {
            root.binaryMissing = code !== 0
            // Chain the data probe off a binary that was just confirmed to
            // exist. Quickshell answers a Process whose command cannot be
            // found with a log line and NO exited signal, so probing through a
            // missing engine would leave pakFound stuck at its previous value
            // rather than being corrected. This also covers "the engine got
            // installed since the last click".
            if (code === 0) root.probePak()
        }
    }

    // The command is assigned here rather than bound to root.binary: a binding
    // and the onBinaryChanged below both react to the same change, in an order
    // QML does not promise, so the probe could start against the PREVIOUS
    // binary and write a verdict about the wrong path. Setting both in one
    // place makes the order explicit.
    //
    // `command -v` resolves a bare name on PATH and also accepts an absolute
    // path, so one probe covers both forms the setting can take.
    function probeBinary() {
        binaryProbe.command = ["sh", "-c", "command -v -- \"$1\" >/dev/null 2>&1",
                               "sh", root.binary]
        binaryProbe.running = true
    }

    Process {
        id: installHint
        command: ["omarchy-notification-send", "-u", "normal", "-g", "\uf11b",
                  "OmaQuake engine not installed",
                  "No '" + root.binary + "' found. Build it with `makepkg -si` in "
                  + "the repo's packaging/ directory, or point this widget's "
                  + "\"binary\" setting at your own build: "
                  + "https://github.com/rsd/omaquake"]
    }

    // The setting is user-editable at runtime, and a binary that was missing
    // under the old value tells you nothing about the new one.
    onBinaryChanged: probeBinary()

    // Re-probe on every rejected click: the engine may have been installed
    // since the last one, and the alternative is telling someone to install
    // software they just installed.
    function activate() {
        if (root.binaryMissing) {
            installHint.running = true
            probeBinary()
            return
        }
        root.toggle()
    }

    // --- the game data --------------------------------------------------
    // An empty `pak` setting does not mean there is nothing to play. omaquake
    // searches ./id1, ~/.local/share/omaquake and /usr/share/omaquake (where
    // the omaquake-shareware-data package drops pak0.pak) among others, so
    // only the binary knows whether a game exists -- `--find-pak` asks it, and
    // answers with exit 0 when it found data.
    property bool pakFound: false

    Process {
        id: pakProbe
        // Any non-zero status means "no data I can prove is there", which
        // deliberately also covers an engine older than --find-pak: it answers
        // an unknown flag with usage and exit 2, and the widget then falls back
        // to --demo exactly as it always did.
        onExited: function (code) { root.pakFound = code === 0 }
    }

    // Assigned in a function rather than bound, for the same reason as
    // probeBinary(): a binding and the change handlers below would both react
    // to one change, in an order QML does not promise, so the probe could run
    // against the PREVIOUS binary or directory and file a verdict about it.
    //
    // The search starts at the process's working directory -- ./id1 is the
    // first place omaquake looks -- so the probe must run from the directory
    // the terminal will be started in (foot gets `-D gameDir`), or it would
    // answer for a directory the game never sees. Empty means "inherit
    // quickshell's cwd", which is what foot does without -D.
    function probePak() {
        // With `pak` set there is nothing to discover: gameArgs passes the
        // configured path straight through, and nothing reads pakFound.
        if (root.pak !== "" || root.binaryMissing) return
        pakProbe.workingDirectory = root.gameDir
        pakProbe.command = [root.binary, "--find-pak", "--no-sound"]
        pakProbe.running = true
    }

    // Both settings are user-editable at runtime and either can turn "no data"
    // into "data" without the binary itself changing. A change to `binary`
    // re-probes through binaryProbe.onExited instead, which is the only moment
    // the new path is known to be runnable.
    onPakChanged: probePak()
    onGameDirChanged: probePak()

    // --- terminal lifecycle --------------------------------------------
    // Spawned through Hyprland's exec-with-rules: 0.56 gained a Lua dispatcher
    // API, and classic per-window dispatchers (`dispatch movewindowpixel exact
    // X Y,address:0x...` and friends) no longer parse under it, so the geometry
    // has to travel with the spawn as window rules. See launchTerminal() for
    // how the request itself is spelled. `move` in a window rule is
    // MONITOR-RELATIVE, so the rule pins the monitor by name and passes
    // monitor-local coordinates.
    readonly property string spawnRule: {
        var mon = surface.screen ? surface.screen.name : ""
        var lx = surface.holeX
        var ly = surface.holeY
        return "[float; pin; noanim; noborder; rounding 0; "
             + "opacity 1.0 override 1.0 override; "
             + "monitor " + mon + "; "
             + "size " + root.termW + " " + root.termH + "; "
             + "move " + lx + " " + ly + "]"
    }

    // --no-mouse matters: omaquake's evdev pointer path takes EVIOCGRAB, which
    // would steal the pointer from the whole desktop while the popout is open.
    //
    // A configured `pak` wins. Otherwise the binary is left to find the data on
    // its own (see probePak), and --demo is the last resort for the one case
    // that is genuinely unplayable: engine present, data nowhere. So a fresh
    // install of omaquake plus omaquake-shareware-data shows Quake with zero
    // configuration, and only a machine without a pak gets the test pattern --
    // which is still needed, because omaquake exits on usage if handed neither
    // game data nor --demo.
    readonly property var gameArgs: {
        var a = ["--no-sound", "--no-mouse"]
        if (root.pak !== "") a.push(root.pak)
        else if (!root.pakFound) a.push("--demo")
        return a
    }

    // Fire-and-forget rather than a Process with a `running` flag. Driving a
    // reused Process by assigning running=false and then true in the same
    // event-loop turn can coalesce into no property change at all, so the
    // command never runs -- which silently skipped the kill and left the
    // terminal alive on screen after the chrome had already come down.
    function launchTerminal() {
        // Also guarded here, not just on the bar click: an IPC-driven open
        // never goes through activate(), and would otherwise sit there until
        // the watchdog killed it without saying why.
        if (root.binaryMissing) {
            installHint.running = true
            probeBinary()
            root.close()
            return
        }

        var cmd = "foot --app-id=" + root.appId + " -o colors.alpha=1.0"
        if (root.gameDir !== "") cmd += " -D " + root.gameDir
        cmd += " -- " + root.binary + " " + root.gameArgs.join(" ")

        // Which spelling of `hyprctl dispatch` works depends on which config
        // manager Hyprland 0.56 was started with, and the widget cannot know.
        // Under the Lua manager (Omarchy 4's default) a dispatch request is
        // compiled as `return hl.dispatch(<request>)`, so a classic
        // `exec [float; pin; ...] cmd` is a Lua syntax error -- `;` inside a
        // bare `[...]` -- and the terminal never spawns; under a hyprlang
        // config there is no `hl.dsp` table at all and only the classic form
        // works. Hence: try the Lua dispatcher, and on any reply that is not
        // `ok` fall back. The spec goes in as $1 and is handed to Lua inside a
        // long bracket, which needs no escaping of the rule's brackets,
        // spaces or quotes.
        Quickshell.execDetached(["sh", "-c",
                                 "out=$(hyprctl dispatch \"hl.dsp.exec_cmd([==[$1]==])\" 2>&1); "
                                 + "case $out in ok*) ;; *) hyprctl dispatch exec \"$1\" ;; esac",
                                 "sh", root.spawnRule + " " + cmd])
    }

    // pkill never signals itself, so matching our own app id is safe here.
    function killTerminal() {
        Quickshell.execDetached(["pkill", "-f", "app-id=" + root.appId])
    }

    // --- close on focus loss -------------------------------------------
    // Arm only once the terminal has actually taken focus. Between open() and
    // foot mapping its window the active toplevel is still whatever the user
    // was in, so an unarmed check would slam the popout shut immediately.
    //
    // This also covers quitting Quake: when foot exits, focus moves to some
    // other toplevel and the same path tears the chrome down.
    property bool armed: false

    readonly property string activeAppId: {
        var t = ToplevelManager.activeToplevel
        return t ? String(t.appId || "") : ""
    }

    onActiveAppIdChanged: {
        if (!opened) return
        if (activeAppId === root.appId) {
            root.armed = true
            return
        }
        if (root.armed) root.close()
    }

    // If the terminal never took focus -- a wrong binary path, foot missing, or
    // an IPC-driven open onto a monitor that is not the focused one -- do not
    // leave a chrome ring floating over an empty hole.
    //
    // It kills the terminal explicitly as well as closing. close() already
    // routes through killTerminal(), but a terminal that outlives its chrome is
    // floating, pinned and undecorated, and would sit on top of everything with
    // nothing left to close it.
    Timer {
        id: spawnWatchdog
        interval: 5000
        onTriggered: {
            if (!root.opened || root.armed) return
            root.close()
            root.killTerminal()
        }
    }

    onOpenedChanged: {
        armed = false
        if (opened) {
            Qt.callLater(launchTerminal)
            spawnWatchdog.restart()
        } else {
            spawnWatchdog.stop()
            killTerminal()
        }
    }

    // A stranded terminal outlives a shell restart: it is floating, pinned and
    // has no chrome of its own, so it would sit on top of everything forever.
    Component.onCompleted: {
        killTerminal()
        probeBinary()
    }
    Component.onDestruction: killTerminal()
}
