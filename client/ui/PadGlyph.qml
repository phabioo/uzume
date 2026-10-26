import QtQuick
import Uzume.App

// Controller glyph chip (design "PadGlyph and the D-pad icons", legacy/source/pad-glyph.dc.html), drawn like the key caps of the
// design system (UzKeyCap / UzGamepadHint): square, 1 px `ink-2` frame, Mono; pressed in the input test fills it with Yamabuki.
// g: A B X Y LB RB LT RT View Menu | cross circle square triangle L1 R1 L2 R2 Create Options | up down left right;
// any other text (Select, Start, Shift, Enter, key names) is drawn as text. Round chip for single letters and the four
// shapes, wider for words (padding 0 30 % of the size). Icons are drawn (Canvas),
// not font glyphs, so Windows and Linux look identical; the four arrows are one path rotated about the center of the
// 16 x 16 box, so they share size, weight and optical center. Monochrome only.
Item {
    id: root
    property string g: "A"
    property int size: 24         // chip height, 16..48
    property bool active: false   // pressed in the input test
    property bool flat: false     // no chip, icon only
    property bool square: false   // D-pad cells (the chip is square either way; kept for the input test model)

    readonly property var arrows: ({ "up": 0, "right": 90, "down": 180, "left": 270 })
    readonly property bool isArrow: root.g in root.arrows
    readonly property bool isShape: root.g === "cross" || root.g === "circle" || root.g === "square" || root.g === "triangle"
    readonly property bool isText: !root.isArrow && !root.isShape
    readonly property bool round: root.isArrow || root.isShape || root.g.length === 1
    readonly property int iconSize: Math.round(root.size * 0.62)

    readonly property color chipColor: "transparent"
    readonly property color ringColor: Theme.ink2
    readonly property color glyphColor: root.active ? (root.flat ? Theme.accent : Theme.inkOnAccent)
                                                    : Theme.ink2

    implicitHeight: root.size
    implicitWidth: root.round ? root.size : Math.max(root.size, label.implicitWidth + 2 * Math.round(root.size * 0.3))
    width: implicitWidth
    height: implicitHeight

    Rectangle {
        id: chip
        objectName: "padGlyphChip"
        anchors.fill: parent
        visible: !root.flat
        color: root.active ? Theme.accent : root.chipColor
        border.width: 1
        border.color: root.active ? Theme.accent : root.ringColor
        Behavior on color { ColorAnimation { duration: 60 } }
    }

    Text {
        id: label
        objectName: "padGlyphText"
        anchors.centerIn: parent
        visible: root.isText
        text: root.g
        color: root.glyphColor
        font.family: Theme.mono
        font.weight: Font.Medium
        font.pixelSize: Math.round(root.size * 0.5)
        // A chip with a fixed width (input test cell) shrinks long words to fit instead of overflowing.
        width: Math.min(implicitWidth, Math.max(0, root.width - 8))
        fontSizeMode: Text.Fit
        minimumPixelSize: 7
        horizontalAlignment: Text.AlignHCenter
        renderType: Text.QtRendering
    }

    // The icon is painted in device pixels (a Canvas of iconSize * dpr, scaled back by 1/dpr), so it is crisp at high DPI.
    // (QtQuick.Shapes would be vector, but its QML module is not part of the Qt packages used for Linux CI.)
    Item {
        id: iconBox
        objectName: "padGlyphIcon"
        anchors.centerIn: parent
        visible: !root.isText
        width: root.iconSize
        height: root.iconSize

    Canvas {
        id: icon
        readonly property real dpr: Math.max(1, Screen.devicePixelRatio)
        width: Math.round(iconBox.width * dpr)
        height: Math.round(iconBox.height * dpr)
        scale: iconBox.width / width
        transformOrigin: Item.TopLeft
        renderTarget: Canvas.Image
        onDprChanged: requestPaint()
        // Re-paint on every input of the drawing.
        property color ink: root.glyphColor
        property string shape: root.g
        onInkChanged: requestPaint()
        onShapeChanged: requestPaint()
        onWidthChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var k = width / 16
            ctx.scale(k, k)
            ctx.fillStyle = Theme.cssColor(icon.ink)
            ctx.strokeStyle = Theme.cssColor(icon.ink)
            ctx.lineJoin = "round"
            ctx.lineCap = "round"
            var s = icon.shape
            if (s in root.arrows) {
                // One path for all four directions: rotated about the center, so size, weight and optical center match.
                ctx.translate(8, 8)
                ctx.rotate(root.arrows[s] * Math.PI / 180)
                ctx.translate(-8, -8)
                ctx.lineWidth = 1.6
                ctx.beginPath()
                ctx.moveTo(8, 3.75); ctx.lineTo(12.5, 10.25); ctx.lineTo(3.5, 10.25); ctx.closePath()
                ctx.fill()
                ctx.stroke()
            } else if (s === "cross") {
                ctx.lineWidth = 1.7
                ctx.beginPath()
                ctx.moveTo(3.5, 3.5); ctx.lineTo(12.5, 12.5)
                ctx.moveTo(12.5, 3.5); ctx.lineTo(3.5, 12.5)
                ctx.stroke()
            } else if (s === "circle") {
                ctx.lineWidth = 1.7
                ctx.beginPath()
                ctx.arc(8, 8, 5, 0, 2 * Math.PI)
                ctx.stroke()
            } else if (s === "square") {
                ctx.lineWidth = 1.7
                var x = 3.5, y = 3.5, w = 9, r = 0.8
                ctx.beginPath()
                ctx.moveTo(x + r, y); ctx.lineTo(x + w - r, y); ctx.arcTo(x + w, y, x + w, y + r, r)
                ctx.lineTo(x + w, y + w - r); ctx.arcTo(x + w, y + w, x + w - r, y + w, r)
                ctx.lineTo(x + r, y + w); ctx.arcTo(x, y + w, x, y + w - r, r)
                ctx.lineTo(x, y + r); ctx.arcTo(x, y, x + r, y, r)
                ctx.closePath()
                ctx.stroke()
            } else if (s === "triangle") {
                ctx.lineWidth = 1.7
                ctx.beginPath()
                ctx.moveTo(8, 3.2); ctx.lineTo(13, 11.8); ctx.lineTo(3, 11.8); ctx.closePath()
                ctx.stroke()
            }
        }
    }
    }
}
