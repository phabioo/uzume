import QtQuick
import Uzume.App

// Small icons of the in-game header and panel (GameHeader sheet): drawn from primitives so they follow the text color.
// kind: pause | play | speed | layout | diag | fullscreen | diamond | caret (drawn, so it centres like the other icons).
Item {
    id: root
    property string kind: ""
    property color color: Theme.sumi.ink
    implicitWidth: kind === "speed" ? 12 : kind === "caret" ? 8 : kind === "diamond" ? 10 : kind === "play" ? 8 : kind === "pause" ? 8 : 11
    implicitHeight: 14

    Row {
        visible: root.kind === "pause"
        anchors.centerIn: parent
        spacing: 2
        Repeater { model: 2; Rectangle { width: 3; height: 10; radius: 0; color: root.color } }
    }
    Column {
        visible: root.kind === "layout"
        anchors.centerIn: parent
        spacing: 2
        Repeater {
            model: 2
            Rectangle { width: 10; height: 6; radius: 0; color: "transparent"; border.width: 1.5; border.color: root.color }
        }
    }
    Row {
        visible: root.kind === "diag"
        anchors.centerIn: parent
        height: 11
        spacing: 2
        Repeater {
            model: [5, 11, 8]
            Item {
                required property int modelData
                width: 2
                height: 11
                Rectangle { anchors.bottom: parent.bottom; width: 2; height: parent.modelData; color: root.color }
            }
        }
    }
    Rectangle {
        visible: root.kind === "fullscreen"
        anchors.centerIn: parent
        width: 11
        height: 11
        radius: 0
        color: "transparent"
        border.width: 1.5
        border.color: root.color
    }
    Rectangle {
        visible: root.kind === "diamond"
        anchors.centerIn: parent
        width: 7
        height: 7
        rotation: 45
        color: "transparent"
        border.width: 1.5
        border.color: root.color
    }
    Canvas {
        id: caretTri
        visible: root.kind === "caret"
        anchors.centerIn: parent
        width: 8
        height: 5
        onPaint: {
            var c = getContext("2d")
            c.clearRect(0, 0, width, height)
            c.fillStyle = Theme.cssColor(root.color)
            c.beginPath()
            c.moveTo(0, 0)
            c.lineTo(8, 0)
            c.lineTo(4, 5)
            c.closePath()
            c.fill()
        }
        Connections {
            target: root
            function onColorChanged() { caretTri.requestPaint() }
            function onKindChanged() { caretTri.requestPaint() }
        }
    }
    Canvas {
        id: tri
        visible: root.kind === "play" || root.kind === "speed"
        anchors.centerIn: parent
        width: 12
        height: 10
        onPaint: {
            var c = getContext("2d")
            c.clearRect(0, 0, width, height)
            c.fillStyle = Theme.cssColor(root.color)
            var n = root.kind === "speed" ? 2 : 1
            var w = root.kind === "speed" ? 6 : 8
            for (var i = 0; i < n; ++i) {
                c.beginPath()
                c.moveTo(i * w, 0)
                c.lineTo(i * w + w, 5)
                c.lineTo(i * w, 10)
                c.closePath()
                c.fill()
            }
        }
        Connections {
            target: root
            function onColorChanged() { tri.requestPaint() }
            function onKindChanged() { tri.requestPaint() }
        }
    }
}
