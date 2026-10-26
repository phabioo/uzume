import QtQuick

// Small busy indicator: a square outline with a rotating square mark (nothing in the app is round except the ensō).
// Only animates while visible.
Item {
    id: root
    property int size: 14
    property color color: Theme.accent
    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    Rectangle {
        anchors.fill: parent
        radius: 0
        color: "transparent"
        border.width: Math.max(1, Math.round(root.size / 10))
        border.color: root.color
        opacity: 0.3
    }
    Item {
        anchors.fill: parent
        RotationAnimator on rotation { running: root.visible && !Theme.reduceMotion; from: 0; to: 360; duration: 900; loops: Animation.Infinite }
        Rectangle {
            width: Math.max(3, root.size / 4)
            height: width
            radius: 0
            x: (root.size - width) / 2
            y: 0
            color: root.color
        }
    }
}
