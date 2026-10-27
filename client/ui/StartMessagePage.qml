import QtQuick
import QtQuick.Layouts
import Uzume.App

// A stop or a dead end with its way out: the security code differs (8b-2, 8c-3e, 8a-6f), no answer (8b-5e, 8c-3f), server or app
// too old (8b-5f), account turned off (8b-5d), already a member (8c-3b), server already set up (8a-6d), certificate changed.
// The host fills `spec` and handles `action(name)`:
//   spec: { kind: "error" | "info", label, meta, title, text, details, actions: [{ name, text, primary }] }
// An error shows the danger square; colour is never alone (title and sentence say it).
StartPage {
    id: root
    property var spec: ({})
    property bool showDetails: false
    signal action(string name)
    screenLabel: spec.screenLabel || ""
    openLabel: spec.openLabel || "UZUME"
    hints: [{ kbd: "Enter", pad: "A", label: qsTr("Choose") }, { kbd: "Esc", pad: "B", label: qsTr("Back") }]
    onBackRequested: root.action("back")

    Rectangle {
        objectName: "startStopSquare"
        visible: root.spec.kind === "error"
        Layout.preferredWidth: 28
        Layout.preferredHeight: 28
        radius: 0
        color: Theme.danger
        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("Error")
        Text { anchors.centerIn: parent; text: "!"; font.family: Theme.sans; font.weight: Font.Bold; font.pixelSize: 15; color: Theme.ground }
    }
    StartHead {
        step: root.spec.label ? root.screenLabel + " · " + root.spec.label : root.screenLabel
        number: 1
        label: root.spec.label || ""
        meta: root.spec.meta || ""
        title: root.spec.title || ""
        text: root.spec.text || ""
    }
    Text {
        objectName: "startStopDetails"
        visible: root.showDetails && (root.spec.details || "") !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: root.spec.details || ""
        textFormat: Text.PlainText
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
        color: Theme.muted
    }
    RowLayout {
        id: actionRow
        Layout.fillWidth: true
        spacing: Theme.space1
        Repeater {
            model: root.spec.actions || []
            onItemAdded: (index, item) => { if (index === 0) root.initialFocus = item }
            delegate: UzButton {
                required property var modelData
                objectName: "startAction_" + modelData.name
                kind: modelData.primary === true ? "primary" : "outline"
                text: modelData.text
                busyOnClick: modelData.busy === true
                onClicked: modelData.name === "details" ? root.showDetails = !root.showDetails : root.action(modelData.name)
            }
        }
        Item { Layout.fillWidth: true }
    }
}
