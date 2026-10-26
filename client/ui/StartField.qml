import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Labelled text field of the start screens (design-system.md "TextField"): label above (12/16 500), the field (36 px), then a
// hint (12 muted) or the error line (square + sentence). `name` is the objectName of the field itself; a secret field has a
// "Show" / "Hide" link (the gamepad key X).
ColumnLayout {
    id: root
    property string name: ""
    property string label: ""
    property string hint: ""
    property string error: ""
    property string placeholder: ""
    property bool secret: false
    property bool mono: false
    property bool readOnly: false
    property bool revealed: false
    property color fieldFill: Theme.dark ? "transparent" : Theme.surface   // Theme.ground inside a dialog
    property alias text: field.text
    property alias field: field
    signal accepted()
    spacing: Theme.space1
    Layout.fillWidth: true

    RowLayout {
        Layout.fillWidth: true
        Text {
            Layout.fillWidth: true
            text: root.label
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.fieldLabel.size
            font.weight: Font.Medium
            color: Theme.ink
        }
        UzButton {
            objectName: root.name + "Reveal"
            visible: root.secret
            kind: "link"
            size: "sm"
            implicitHeight: 20
            focusPolicy: Qt.ClickFocus   // Tab goes Name -> Password -> Sign in; the link stays a click / gamepad (X) target
            text: root.revealed ? qsTr("Hide") : qsTr("Show")
            onClicked: root.revealed = !root.revealed
        }
    }
    UzField {
        id: field
        objectName: root.name
        Layout.fillWidth: true
        mono: root.mono
        fill: root.fieldFill
        readOnly: root.readOnly
        invalid: root.error !== ""
        placeholderText: root.placeholder
        echoMode: root.secret && !root.revealed ? TextInput.Password : TextInput.Normal
        inputMethodHints: root.secret ? Qt.ImhSensitiveData | Qt.ImhNoPredictiveText : Qt.ImhNoPredictiveText
        onAccepted: root.accepted()
        Accessible.name: root.label
    }
    UzStatusIcon {
        objectName: root.name + "Error"
        visible: root.error !== ""
        kind: "error"
        small: true
        text: root.error
        maxWidth: root.width
    }
    Text {
        objectName: root.name + "Hint"
        visible: root.error === "" && root.hint !== ""
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: root.hint
        textFormat: Text.PlainText
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
        color: Theme.muted
    }
}
