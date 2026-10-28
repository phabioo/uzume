import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// The people of the scope of the running game (boards 3g-12 and 3g-7): "3 IN THIS SESSION" / "4 IN THIS RUN" over one row per person
// (person colour avatar, name, connection when known) with the role tag HOST / VIEWER in a Session. Voice marks, quality and latency
// come with later packages. `people` is ChatController.people: [{userId, name, role, me, color, connection, tone}].
Column {
    id: root
    property var people: []
    property string scopeKind: "session"
    spacing: 0

    UzSectionLabel {
        objectName: "peopleTitle"
        width: parent.width
        text: root.scopeKind === "run" ? qsTr("%n IN THIS RUN", "", root.people.length) : qsTr("%n IN THIS SESSION", "", root.people.length)
    }
    Item { width: 1; height: Theme.space1 }
    Text {
        objectName: "peopleEmpty"
        visible: root.people.length === 0
        width: parent.width
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        text: qsTr("Nobody else is here yet.")
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        color: Theme.muted
    }
    Repeater {
        model: root.people
        delegate: Item {
            id: prow
            required property var modelData
            objectName: "person_" + (modelData.userId || modelData.name)
            width: root.width
            height: Theme.row44 + 8
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
            UzAvatar {
                id: av
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                name: prow.modelData.name || ""
                person: prow.modelData.color || 1
                size: 28
            }
            UzTag {
                id: roleTag
                objectName: "personRole"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: (prow.modelData.role || "") !== ""
                small: true
                text: prow.modelData.role || ""
                tone: prow.modelData.role === "HOST" ? "accent" : "neutral"
            }
            ColumnLayout {
                anchors.left: av.right
                anchors.leftMargin: Theme.space15
                anchors.right: roleTag.visible ? roleTag.left : parent.right
                anchors.rightMargin: Theme.space1
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0
                UzLabel {
                    objectName: "personName"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    text: prow.modelData.me ? qsTr("%1 · you").arg(prow.modelData.name) : prow.modelData.name
                    font.pixelSize: Theme.roles.itemTitleSm.size
                    font.weight: Theme.roles.itemTitleSm.weight
                    color: Theme.ink
                }
                UzLabel {
                    objectName: "personConnection"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: !!prow.modelData.connection
                    elide: Text.ElideRight
                    text: prow.modelData.connection || ""
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.muted
                }
            }
        }
    }
}
