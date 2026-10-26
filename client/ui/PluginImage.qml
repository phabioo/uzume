import QtQuick

// image block: sprite / icon in the size set l = 68x56, m = 40x30, s = 24x18, nearest-neighbour. States: alive,
// dead (grey + cross at 55 % opacity, never red), missed (dashed outline), empty (placeholder). `owner` = player color dot.
Item {
    id: root
    property var node: ({})
    property bool dark: true
    property string pluginId: ""
    // Direct use (list rows, map markers): overrides of the node.
    property string src: node.src || ""
    property string size: node.size || "m"
    property string imgState: node.state || "alive"
    property int owner: node.owner || 0

    readonly property int w: size === "l" ? 68 : (size === "s" ? 24 : 40)
    readonly property int h: size === "l" ? 56 : (size === "s" ? 18 : 30)
    readonly property url source: Theme.pluginImageUrl(pluginId, src)
    implicitWidth: w
    implicitHeight: h
    width: implicitWidth
    height: implicitHeight

    Rectangle {
        anchors.fill: parent
        radius: 0
        color: root.imgState === "empty" ? "transparent" : Theme.tok("placeholder", root.dark)
        border.width: root.imgState === "empty" ? 0 : 1
        border.color: Theme.tok("placeholderRule", root.dark)
        visible: root.imgState !== "alive" || img.status !== Image.Ready
        opacity: root.imgState === "dead" ? 0.55 : 1
    }
    Image {
        id: img
        objectName: "pluginImageSprite"
        anchors.fill: parent
        source: root.imgState === "empty" ? "" : root.source
        sourceSize: Qt.size(root.w * 2, root.h * 2)  // the provider scales to this while decoding (never the full 4096x4096)
        smooth: false
        asynchronous: true
        fillMode: Image.PreserveAspectFit
        opacity: root.imgState === "dead" ? 0.55 : 1
    }
    Rectangle {  // dead: grey wash over the sprite
        visible: root.imgState === "dead"
        anchors.fill: parent
        radius: 0
        color: Theme.blockColor("textFaint", root.dark)
        opacity: 0.35
    }
    Text {
        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
        visible: root.imgState === "dead"
        anchors.centerIn: parent
        text: "✕"
        opacity: 0.55
        font.pixelSize: Math.round(root.h * 0.6)
        font.weight: Font.DemiBold
        color: Theme.blockColor("textMuted", root.dark)
    }
    Canvas {
        id: dash
        visible: root.imgState === "missed" || root.imgState === "empty"
        anchors.fill: parent
        property color line: Theme.blockColor("missedLine", root.dark)
        onLineChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onVisibleChanged: if (visible) requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = Theme.cssColor(line)
            ctx.lineWidth = 1.5
            ctx.setLineDash([3, 3])
            ctx.strokeRect(1, 1, width - 2, height - 2)
        }
    }
    Rectangle {  // owner dot (6) with a ring in the surface color
        visible: root.owner > 0
        width: 8
        height: 8
        x: parent.width - 6
        y: parent.height - 6
        color: Theme.blockColor("ownerRing", root.dark)
        Rectangle {
            anchors.centerIn: parent
            width: 6
            height: 6
            color: Theme.playerColor(root.owner, root.dark)
        }
    }
}
