import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// The people of the scope of the running game (boards 3g-12 and 3g-7): "3 IN THIS SESSION" / "4 IN THIS RUN" over one row per person
// (person colour avatar, name, connection line, e.g. "Direct · 18 ms · gets High" or "Relayed · 60 ms · picked Medium") with the role tag
// HOST / VIEWER in a Session. On my own row of a watched Session a "Quality" Segmented (Auto / High / Medium / Low, one click) sits below
// (qualityPick is set only there). Voice marks come with the voice package. `people` is ChatController.people:
// [{userId, name, role, roleKey, me, color, connection, tone, qualityPick?, sessionId?}].
Column {
    id: root
    property var people: []
    property string scopeKind: "session"
    signal qualityPicked(string sessionId, string value)
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
            readonly property bool hasQuality: modelData.qualityPick !== undefined
            width: root.width
            height: Theme.row44 + 8 + (hasQuality ? qualityRow.height : 0)
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
            UzAvatar {
                id: av
                anchors.left: parent.left
                y: (Theme.row44 + 8 - height) / 2
                name: prow.modelData.name || ""
                person: prow.modelData.color || 1
                size: 28
            }
            UzTag {
                id: roleTag
                objectName: "personRole"
                anchors.right: parent.right
                y: (Theme.row44 + 8 - height) / 2
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
                y: (Theme.row44 + 8 - height) / 2
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
            RowLayout {
                id: qualityRow
                objectName: "personQualityRow"
                visible: prow.hasQuality
                y: Theme.row44 + 8 - 6
                x: av.width + Theme.space15
                width: parent.width - x
                height: visible ? Theme.row28 + 8 : 0
                spacing: Theme.space1
                UzLabel {
                    text: qsTr("Quality")
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.muted
                }
                UzSegmented {
                    objectName: "qualitySegmented"
                    Layout.fillWidth: true
                    Layout.maximumWidth: implicitWidth
                    accessibleName: qsTr("Quality")
                    current: prow.modelData.qualityPick || "auto"
                    options: [{ value: "auto", label: qsTr("Auto"), name: "qualityAuto" },
                              { value: "high", label: qsTr("High"), name: "qualityHigh" },
                              { value: "medium", label: qsTr("Medium"), name: "qualityMedium" },
                              { value: "low", label: qsTr("Low"), name: "qualityLow" }]
                    onPicked: (value) => root.qualityPicked(prow.modelData.sessionId || "", value)
                }
            }
        }
    }
}
