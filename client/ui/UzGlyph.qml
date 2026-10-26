import QtQuick

// Small line glyph (design-system bundle "Glyph": 16 x 16 box, 1.5 px stroke, square caps, no curves). Painted in device
// pixels so it is crisp at any scale. name: chevronDown chevronUp check cross dash more bell volume volumeOff mic micOff
// up down left right.
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

    Canvas {
        id: cv
        readonly property real dpr: Math.max(1, Screen.devicePixelRatio)
        width: Math.round(root.width * dpr)
        height: Math.round(root.height * dpr)
        scale: root.width / width
        transformOrigin: Item.TopLeft
        renderTarget: Canvas.Image
        onDprChanged: requestPaint()
        property string glyph: root.name
        property color ink: root.color
        property real sw: root.strokeWidth
        onGlyphChanged: requestPaint()
        onInkChanged: requestPaint()
        onSwChanged: requestPaint()
        onWidthChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const k = width / 16
            ctx.scale(k, k)
            if (cv.glyph === "more") {
                ctx.fillStyle = cv.ink
                for (const x of [2, 7, 12]) ctx.fillRect(x, 7, 2, 2)
                return
            }
            const d = root.paths[cv.glyph]
            if (!d) return
            ctx.strokeStyle = cv.ink
            ctx.lineWidth = cv.sw
            ctx.lineCap = "square"
            ctx.lineJoin = "miter"
            const t = d.split(/\s+/)
            let i = 0, x = 0, y = 0, sx = 0, sy = 0
            ctx.beginPath()
            while (i < t.length) {
                const c = t[i++]
                if (c === "M") { x = +t[i++]; y = +t[i++]; sx = x; sy = y; ctx.moveTo(x, y) }
                else if (c === "L") { x = +t[i++]; y = +t[i++]; ctx.lineTo(x, y) }
                else if (c === "H") { x = +t[i++]; ctx.lineTo(x, y) }
                else if (c === "V") { y = +t[i++]; ctx.lineTo(x, y) }
                else if (c === "Z") { ctx.closePath(); x = sx; y = sy }
            }
            ctx.stroke()
        }
    }
}
