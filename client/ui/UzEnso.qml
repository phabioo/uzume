import QtQuick

// Ensō (design-system.md "Enso"): the open brush circle with a Yamabuki point at its end. Only for empty and waiting states
// (UzStateBlock, waiting for approval, the Multiview waiting tile) - nothing else in the app is round. Stroke `ink-2`, 56 box.
Item {
    id: root
    objectName: "uzEnso"
    property int size: 48
    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    Canvas {
        id: cv
        readonly property real dpr: Math.max(1, Screen.devicePixelRatio)
        width: Math.round(root.width * dpr)
        height: Math.round(root.height * dpr)
        scale: root.width / width
        transformOrigin: Item.TopLeft
        renderTarget: Canvas.Image
        onDprChanged: requestPaint()
        onWidthChanged: requestPaint()
        property color stroke: Theme.ink2
        property color point: Theme.accent
        onStrokeChanged: requestPaint()
        onPointChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.scale(width / 56, width / 56)
            // M 41.6 11.6 A 22 22 0 1 0 49.9 31.6: large arc, counter-clockwise, centre (28.06, 28.94)
            const cx = 28.06, cy = 28.94, r = 22
            const a1 = Math.atan2(11.6 - cy, 41.6 - cx), a2 = Math.atan2(31.6 - cy, 49.9 - cx)
            ctx.strokeStyle = Theme.cssColor(cv.stroke)
            ctx.lineWidth = 2.5
            ctx.lineCap = "round"
            ctx.beginPath()
            ctx.arc(cx, cy, r, a1, a2, true)
            ctx.stroke()
            ctx.fillStyle = Theme.cssColor(cv.point)
            ctx.beginPath()
            ctx.arc(49.9, 31.6, 2.2, 0, 2 * Math.PI)
            ctx.fill()
        }
    }
}
