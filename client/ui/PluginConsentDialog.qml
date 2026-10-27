import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Consent question of the plugin API (cheats, game memory; docs/design/uzume/cheats.md 11d). Shown while
// plugins.consentRequest is not empty; "Cancel" refuses for this session, the confirm button is remembered for the game.
Popup {
    id: root
    objectName: "pluginConsentDialog"
    property var plugins: null
    readonly property var request: plugins ? plugins.consentRequest : ({})
    readonly property bool wanted: request && request.kind !== undefined && request.kind !== ""
    property bool answered: false

    parent: Window.window ? Window.window.contentItem : null
    anchors.centerIn: parent
    modal: true
    dim: true
    focus: true
    width: Math.min(Theme.dialog, parent ? parent.width - 2 * Theme.space2 : Theme.dialog)
    padding: 28
    closePolicy: Popup.CloseOnEscape
    z: 90

    onWantedChanged: {
        if (wanted) { answered = false; open() }
        else if (opened) close()
    }
    Component.onCompleted: if (wanted) open()
    onClosed: if (!answered && wanted && plugins) { answered = true; plugins.answerConsent(false) }
    onOpened: okBtn.forceActiveFocus()

    background: Rectangle {
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
    }

    contentItem: ColumnLayout {
        spacing: Theme.space15
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: root.request.label || ""
            font.family: Theme.sans
            font.pixelSize: Theme.roles.body.size
            font.letterSpacing: 1
            color: Theme.muted
        }
        Text {
            objectName: "pluginConsentTitle"
            textFormat: Text.PlainText  // game titles are never HTML
            Layout.fillWidth: true
            text: root.request.title || ""
            wrapMode: Text.WordWrap
            font.family: Theme.sans
            font.pixelSize: Theme.roles.dialogTitle.size
            font.letterSpacing: Theme.roles.dialogTitle.spacing
            color: Theme.ink
        }
        // Numbered points 01 / 02 / 03 (design 11d): Mono number, then the sentence.
        Repeater {
            model: root.request.items || []
            delegate: RowLayout {
                id: point
                required property int index
                required property var modelData
                objectName: "pluginConsentPoint_" + point.index
                Layout.fillWidth: true
                spacing: Theme.space15
                Text {
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: 22
                    text: (point.index + 1 < 10 ? "0" : "") + (point.index + 1)
                    textFormat: Text.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta.size
                    font.weight: Font.Medium
                    topPadding: 2
                    color: Theme.muted
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: point.modelData
                    wrapMode: Text.WordWrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.body.size
                    color: Theme.ink2
                }
            }
        }
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            visible: text !== ""
            text: root.request.meta || ""
            wrapMode: Text.WordWrap
            font.family: Theme.sans
            font.pixelSize: Theme.roles.body.size
            color: Theme.muted
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.space1
            spacing: Theme.space1
            Item { Layout.fillWidth: true }
            UzButton {
                objectName: "pluginConsentCancel"
                text: qsTr("Cancel")
                onClicked: root.close()
            }
            UzButton {
                id: okBtn
                objectName: "pluginConsentOk"
                kind: "primary"
                text: root.request.confirmLabel || qsTr("Allow")
                textFormat: Text.PlainText
                onClicked: { root.answered = true; root.plugins.answerConsent(true); root.close() }
            }
        }
    }
}
