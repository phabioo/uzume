import QtQuick
import QtQuick.Layouts
import Uzume.App

// One-time question (ADR 0035 D1): "Send error messages to your server automatically?" Non-modal, bottom left over every screen but
// the game. Shown the first time an error event is queued while the setting is unanswered; both buttons answer for good ("Not now"
// keeps it off; the setting stays in Settings > Help & diagnostics).
Rectangle {
    id: root
    objectName: "errorsPrompt"
    required property PlayerController player
    visible: root.player.errors.promptVisible && root.player.screen !== "game"
    z: 80
    width: Math.min(460, parent ? parent.width - 2 * Theme.space3 : 460)
    height: col.implicitHeight + 2 * Theme.space2
    x: Theme.space3
    y: parent ? parent.height - height - Theme.space3 : 0
    color: Theme.surface
    border.width: 1
    border.color: Theme.rule
    Accessible.role: Accessible.AlertMessage
    Accessible.name: qsTr("Send error messages to your server automatically?")

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: Theme.space2
        spacing: Theme.space15
        Text {
            objectName: "errorsPromptTitle"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("Send error messages to your server automatically?")
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.body.size
            font.weight: Font.Medium
            color: Theme.ink
        }
        Text {
            objectName: "errorsPromptText"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("The app just hit an error. If you agree, a short message (no logs, no files) goes to the admins of your server whenever the app, a core, a plugin, the save sync or an update fails. It never goes anywhere else. You can change this in Settings.")
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: Theme.ink2
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space1
            UzButton {
                objectName: "errorsPromptAllow"
                size: "sm"
                kind: "primary"
                text: qsTr("Send automatically")
                onClicked: root.player.errors.answerPrompt(true)
            }
            UzButton {
                objectName: "errorsPromptLater"
                size: "sm"
                text: qsTr("Not now")
                onClicked: root.player.errors.answerPrompt(false)
            }
            Item { Layout.fillWidth: true }
        }
    }
}
