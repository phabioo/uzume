import QtQuick

// AvatarStack (design-system.md "AvatarStack"): watchers as overlapping squares (overlap 4 px, 2 px ground ring), size 20 by
// default, with an optional Mono label ("3 WATCHING"). people: [{ name, person }]
Row {
    id: root
    property var people: []
    property int size: 20
    property string label: ""
    spacing: Theme.space1

    Item {
        id: faces
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, root.people.length) * (root.size - 4) + 4
        height: root.size
        Repeater {
            model: root.people
            delegate: Item {
                id: slot
                required property var modelData
                required property int index
                x: index * (root.size - 4)
                width: root.size
                height: root.size
                Rectangle { anchors.fill: parent; anchors.margins: -2; color: Theme.ground }
                UzAvatar { name: slot.modelData.name || ""; person: slot.modelData.person || (slot.index + 1); size: root.size }
            }
        }
    }
    Text {
        visible: root.label !== ""
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        textFormat: Text.PlainText
        font.family: Theme.mono
        font.pixelSize: Theme.roles.meta.size
        font.letterSpacing: 0.8
        font.capitalization: Font.AllUppercase
        color: Theme.muted
    }
}
