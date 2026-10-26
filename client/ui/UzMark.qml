import QtQuick

// Uzume mark, logo 03a "Rise" (docs/design/uzume/frame.md "0-4 A"; bundle `Mark`): a U with a rising right arm and a light
// point inside, drawn as a vector in a 96 x 96 box, crisp at any size. The ink follows the ground (`ink` by default, so it
// is light on Sumi and dark on Kinari); the light point is Yamabuki. `mono` draws both in the ink colour (single-colour use).
Item {
    id: root
    objectName: "uzMark"
    property int size: 20
    property bool mono: false
    property color ink: Theme.ink
    property color light: Theme.accent
    implicitWidth: size
    implicitHeight: size
    width: size
    height: size
    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Uzume")

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
        property color inkColor: root.ink
        property color lightColor: root.mono ? root.ink : root.light
        onInkColorChanged: requestPaint()
        onLightColorChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const k = width / 96
            ctx.scale(k, k)
            // M14 42 L30 38 L30 56 A18 18 0 0 0 66 56 L66 16 L82 10 L82 56 A34 34 0 0 1 14 56 Z
            ctx.fillStyle = Theme.cssColor(cv.inkColor)
            ctx.beginPath()
            ctx.moveTo(14, 42)
            ctx.lineTo(30, 38)
            ctx.lineTo(30, 56)
            ctx.arc(48, 56, 18, Math.PI, 0, true)     // inner bowl, left to right through the bottom
            ctx.lineTo(66, 16)
            ctx.lineTo(82, 10)
            ctx.lineTo(82, 56)
            ctx.arc(48, 56, 34, 0, Math.PI, false)    // outer bowl, right to left through the bottom
            ctx.closePath()
            ctx.fill()
            ctx.fillStyle = Theme.cssColor(cv.lightColor)
            ctx.beginPath()
            ctx.arc(48, 30, 9, 0, 2 * Math.PI)
            ctx.fill()
        }
    }
}
