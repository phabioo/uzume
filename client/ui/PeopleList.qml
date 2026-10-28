import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// The people of the scope of the running game (boards 3g-12 and 3g-7): "3 IN THIS SESSION" / "4 IN THIS RUN" over one row per person
// (person colour avatar, name, connection line, e.g. "Direct · 18 ms · gets High" or "Relayed · 60 ms · picked Medium") with the role tag
// HOST / VIEWER in a Session. On my own row of a watched Session a "Quality" Segmented (Auto / High / Medium / Low, one click) sits below
// (qualityPick is set only there). Voice marks, the Volume slider and the host's Mute come from the VoiceController. `people` is ChatController.people:
// [{userId, name, role, roleKey, me, color, connection, tone, qualityPick?, sessionId?}].
Column {
    id: root
    property var people: []
    property string scopeKind: "session"
    signal qualityPicked(string sessionId, string value)
    signal volumePicked(string userId, int percent)
    signal muteToggled(string userId, bool muted)
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
            // Voice (board 3g-12): a person in the voice room has a mark next to the name and, if it is somebody else, a Volume slider
            // (and for the Session owner a mute button) on a line of their own.
            readonly property bool hasVoiceLine: modelData.inVoice === true && modelData.me !== true
            width: root.width
            height: Theme.row44 + 8 + (hasQuality ? qualityRow.height : 0) + (hasVoiceLine ? voiceRow.height : 0)
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
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: Theme.space1
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
                    UzVoiceMark {
                        objectName: "personVoice"
                        voice: prow.modelData.voice || ""
                        word: false
                        Layout.alignment: Qt.AlignVCenter
                    }
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
                id: voiceRow
                objectName: "personVoiceRow"
                visible: prow.hasVoiceLine
                y: Theme.row44 + 8 - 6 + (prow.hasQuality ? qualityRow.height : 0)
                x: av.width + Theme.space15
                width: parent.width - x
                height: visible ? Theme.row28 + 8 : 0
                spacing: Theme.space1
                UzSlider {
                    id: volSlider
                    objectName: "personVolume"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Accessible.name: qsTr("Volume of %1").arg(prow.modelData.name)   // no visible label: the narrow panel needs the room for the track
                    from: 0
                    to: 100
                    stepSize: 5
                    onMoved: root.volumePicked(prow.modelData.userId || "", Math.round(value))
                    Binding { target: volSlider; property: "value"; value: prow.modelData.voiceGain === undefined ? 100 : prow.modelData.voiceGain }
                }
                UzButton {
                    objectName: "personMute"
                    visible: prow.modelData.canMute === true
                    size: "sm"
                    focusPolicy: Qt.TabFocus
                    text: prow.modelData.mutedByHost === true ? qsTr("Unmute") : qsTr("Mute")
                    onClicked: root.muteToggled(prow.modelData.userId || "", prow.modelData.mutedByHost !== true)
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
