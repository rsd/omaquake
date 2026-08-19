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

    moduleName: "raul.omaquake"
    ipcTarget: "raul.omaquake"

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
        text: "Q"
        onPressed: function (b) { root.toggle() }
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

    // --- terminal lifecycle --------------------------------------------
    // Spawned through Hyprland's exec-with-rules because that is the one
    // dispatcher form that still parses on 0.56 (it replaced hyprctl's
    // argument parsing with a Lua dispatcher API, so `dispatch
    // movewindowpixel ...` and friends now fail). `move` in a window rule is
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
    // With no pak configured fall back to the test pattern -- omaquake exits on
    // usage if given neither game data nor --demo.
    readonly property var gameArgs: {
        var a = ["--no-sound", "--no-mouse"]
        if (root.pak !== "") a.push(root.pak)
        else a.push("--demo")
        return a
    }

    Process {
        id: spawn
        running: false
    }

    Process {
        id: reap
        // pkill never signals itself, so matching our own app-id is safe here.
        command: ["pkill", "-f", "app-id=" + root.appId]
        running: false
    }

    function launchTerminal() {
        var cmd = "foot --app-id=" + root.appId + " -o colors.alpha=1.0"
        if (root.gameDir !== "") cmd += " -D " + root.gameDir
        cmd += " -- " + root.binary + " " + root.gameArgs.join(" ")

        spawn.command = ["hyprctl", "dispatch", "exec", root.spawnRule + " " + cmd]
        spawn.running = true
    }

    function killTerminal() {
        reap.running = false
        reap.running = true
    }

    onOpenedChanged: {
        if (opened) Qt.callLater(launchTerminal)
        else killTerminal()
    }

    // A stranded terminal outlives a shell restart: it is floating, pinned and
    // has no chrome of its own, so it would sit on top of everything forever.
    Component.onCompleted: killTerminal()
    Component.onDestruction: killTerminal()
}
