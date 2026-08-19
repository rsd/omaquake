import QtQuick
import QtQuick.Shapes
import qs.Commons

// The Quake mark, as vector geometry rather than a glyph -- Nerd Fonts have
// nothing resembling it, and the bar's icon font is where every other widget
// gets its symbol from.
//
// Follows the DropboxIcon pattern: a plain Item exposing iconSize and color so
// BarIconButton can hand it the theme foreground and let it scale with the bar.
Item {
    id: root

    property real iconSize: Style.font.icon
    property color color: Color.foreground

    width: iconSize
    height: iconSize
    implicitWidth: iconSize
    implicitHeight: iconSize

    // Drawn in a 100x100 space and scaled down, so the path data stays legible
    // instead of being written as fractions of the icon size.
    Item {
        anchors.centerIn: parent
        width: 100
        height: 100
        scale: root.iconSize / 100

        Shape {
            anchors.fill: parent
            antialiasing: true
            layer.enabled: true
            layer.samples: 4

            // The ring. OddEvenFill turns the second subpath into the counter.
            // Faceted rather than circular: at bar size the facets vanish, but
            // they keep it from reading as a plain circle at panel size.
            ShapePath {
                fillColor: root.color
                strokeWidth: 0
                fillRule: ShapePath.OddEvenFill
                PathSvg {
                    path: "M 26 0 L 56 0 L 80 24 L 80 50 L 56 74 L 26 74 L 2 50 L 2 24 Z "
                        + "M 30 22 L 52 22 L 60 30 L 60 44 L 52 52 L 30 52 L 22 44 L 22 30 Z"
                }
            }

            // The blade. Kept as its own ShapePath so it does not take part in
            // the ring's fill rule -- sharing one path would punch a hole
            // wherever the two overlap instead of filling it.
            ShapePath {
                fillColor: root.color
                strokeWidth: 0
                PathSvg { path: "M 56 48 L 98 88 L 72 98 L 48 66 Z" }
            }
        }
    }
}
