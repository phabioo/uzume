import QtQuick

// StatusLine (design-system.md "StatusLine"): the server line of the nav footer. Online = a 6 px ok square + name + ONLINE,
// offline = an empty dim square + OFFLINE; the address in Mono below (hidden in compact density). No amber border.
Column {
    id: root
    property string status: "online"      // online | offline
    property string name: qsTr("Home server")
    property string address: ""
    property bool compact: false
    property real maxWidth: 100000   // the name elides rather than widening the line
    readonly property bool online: status === "online"
    spacing: 2
    Accessible.role: Accessible.StaticText
    Accessible.name: name + ", " + (online ? qsTr("online") : qsTr("offline")) + (address !== "" ? ", " + address : "")

    Row {
        spacing: Theme.space1
        Rectangle {
            objectName: "statusLineSquare"
            anchors.verticalCenter: parent.verticalCenter
            width: 6
            height: 6
            radius: 0
            color: root.online ? Theme.ok : "transparent"
            border.width: root.online ? 0 : 1
            border.color: Theme.muted
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(0, root.maxWidth - 6 - stateText.implicitWidth - 2 * Theme.space1))
            elide: Text.ElideRight
            text: root.name
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: 13
            font.weight: Font.Medium
            color: Theme.ink
        }
        Text {
            id: stateText
            anchors.verticalCenter: parent.verticalCenter
            text: root.online ? qsTr("ONLINE") : qsTr("OFFLINE")
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            color: Theme.muted
        }
    }
    Text {
        visible: root.address !== "" && !root.compact
        width: Math.min(implicitWidth, root.maxWidth)
        elide: Text.ElideRight
        text: root.address
        textFormat: Text.PlainText
        font.family: Theme.mono
        font.pixelSize: 11
        color: Theme.muted
    }
}
