import QtQuick

// Small line glyph (design-system bundle "Glyph": 16 x 16 box, 1.5 px stroke, square caps, no curves). Built from
// scene-graph rectangles (no Canvas: Qt 6.8 on Windows did not draw coloured Canvas strokes), so it is crisp at any scale.
// name: chevronDown chevronUp check cross dash more bell volume volumeOff mic micOff up down left right fullscreen.
Item {
    id: root
    property string name: "check"
    property int size: 16
    property color color: Theme.ink
    property real strokeWidth: 1.5
    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    readonly property var paths: ({
        chevronDown: "M3 6 L8 11 L13 6", chevronUp: "M3 10 L8 5 L13 10", check: "M3 8.5 L6.5 12 L13 4.5",
        cross: "M3.5 3.5 L12.5 12.5 M12.5 3.5 L3.5 12.5", dash: "M3 8 H13",
        bell: "M5 3 H11 L12.5 11 H3.5 Z M2 11 H14 M7 13.5 H9",
        volume: "M2 6 H5 L9 3 V13 L5 10 H2 Z M11.5 6 V10 M14 4.5 V11.5",
        volumeOff: "M2 6 H5 L9 3 V13 L5 10 H2 Z M11 6 L15 10 M15 6 L11 10",
        mic: "M6 2 H10 V9 H6 Z M3.5 7.5 V11 H12.5 V7.5 M8 11 V14 M5.5 14 H10.5",
        micOff: "M6 2 H10 V9 H6 Z M3.5 7.5 V11 H12.5 V7.5 M8 11 V14 M5.5 14 H10.5 M2 2 L14 14",
        up: "M8 13 V3 M4 7 L8 3 L12 7", down: "M8 3 V13 M4 9 L8 13 L12 9",
        fullscreen: "M2 6 V2 H6 M10 2 H14 V6 M14 10 V14 H10 M6 14 H2 V10",
        left: "M13 8 H3 M7 4 L3 8 L7 12", right: "M3 8 H13 M9 4 L13 8 L9 12"
    })

    // Segments [x1, y1, x2, y2] in the 16 x 16 box, parsed from the path data; "more" is three dots.
    readonly property var segments: {
        if (name === "more") return []
        const d = paths[name]
        if (!d) return []
        const t = d.split(/\s+/)
        const out = []
        let i = 0, x = 0, y = 0, sx = 0, sy = 0
        while (i < t.length) {
            const c = t[i++]
            if (c === "M") { x = +t[i++]; y = +t[i++]; sx = x; sy = y }
            else if (c === "L") { const nx = +t[i++], ny = +t[i++]; out.push([x, y, nx, ny]); x = nx; y = ny }
            else if (c === "H") { const nx = +t[i++]; out.push([x, y, nx, y]); x = nx }
            else if (c === "V") { const ny = +t[i++]; out.push([x, y, x, ny]); y = ny }
            else if (c === "Z") { out.push([x, y, sx, sy]); x = sx; y = sy }
        }
        return out
    }
    readonly property real k: root.width / 16

    Repeater {
        model: root.name === "more" ? 0 : root.segments.length
        Rectangle {
            required property int index
            readonly property var s: root.segments[index]
            readonly property real dx: s[2] - s[0]
            readonly property real dy: s[3] - s[1]
            // Square caps: extend by strokeWidth / 2 at both ends.
            readonly property real len: Math.sqrt(dx * dx + dy * dy) + root.strokeWidth
            width: len * root.k
            height: root.strokeWidth * root.k
            x: (s[0] + s[2]) / 2 * root.k - width / 2
            y: (s[1] + s[3]) / 2 * root.k - height / 2
            rotation: Math.atan2(dy, dx) * 180 / Math.PI
            color: root.color
            antialiasing: true
        }
    }
    Repeater {
        model: root.name === "more" ? [2, 7, 12] : []
        Rectangle {
            required property int modelData
            x: modelData * root.k
            y: 7 * root.k
            width: 2 * root.k
            height: 2 * root.k
            color: root.color
        }
    }
}
