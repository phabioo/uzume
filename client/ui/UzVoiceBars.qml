import QtQuick

// Speaking bars of the voice mark: three 2 px Yamabuki bars (5 / 10 / 7 px) that breathe; `still` freezes them.
Item {
    id: root
    property bool still: false
    implicitWidth: 10
    implicitHeight: 10
    width: 10
    height: 10
    Row {
        anchors.bottom: parent.bottom
        spacing: 2
        Repeater {
            model: [5, 10, 7]
            delegate: Rectangle {
                id: bar
                required property int modelData
                required property int index
                width: 2
                height: modelData
                radius: 0
                color: Theme.accent
                transformOrigin: Item.Bottom
                SequentialAnimation on scale {
                    running: !root.still && root.visible && !Theme.reduceMotion
                    loops: Animation.Infinite
                    PauseAnimation { duration: bar.index * 300 }
                    NumberAnimation { to: 0.5; duration: 450; easing.type: Easing.InOutQuad }
                    NumberAnimation { to: 1; duration: 450; easing.type: Easing.InOutQuad }
                }
            }
        }
    }
}
