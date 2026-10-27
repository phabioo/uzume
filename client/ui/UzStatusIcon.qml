import QtQuick

// StatusIcon (design-system.md "StatusIcon"): a 20 px square (sm 14) with a glyph: done = ink-2 frame with a check, warning =
// Yamabuki plane with "!", error = danger plane with a cross, info = ink-2 frame with an "i" (notices that only tell, e.g. toasts). With `text` it is followed by the sentence (13 px, ink): warn /
// error / done / info are an icon plus a sentence, never a tinted banner. Without an icon a plain ink-2 sentence is used.
Row {
    id: root
    property string kind: "done"     // done | warning | error | info
    property string text: ""
    property bool small: false
    property real maxWidth: 100000   // the sentence wraps at this width
    spacing: Theme.space1
    Accessible.role: Accessible.StaticText
    Accessible.name: (kind === "done" ? qsTr("Done") : kind === "warning" ? qsTr("Warning") : kind === "info" ? qsTr("Info") : qsTr("Error")) + (text !== "" ? ": " + text : "")

    Rectangle {
        objectName: "statusIconBox"
        anchors.verticalCenter: parent.verticalCenter
        width: root.small ? 14 : 20
        height: width
        radius: 0
        color: root.kind === "warning" ? Theme.accent : root.kind === "error" ? Theme.danger : "transparent"
        border.width: 1
        border.color: root.kind === "warning" ? Theme.accent : root.kind === "error" ? Theme.danger : Theme.ink2
        UzGlyph {
            visible: root.kind !== "warning" && root.kind !== "info"
            anchors.centerIn: parent
            name: root.kind === "done" ? "check" : "cross"
            size: root.small ? 10 : 14
            strokeWidth: 1.6
            color: root.kind === "done" ? Theme.ink : Theme.ground
        }
        Text {
            visible: root.kind === "warning" || root.kind === "info"
            anchors.centerIn: parent
            text: root.kind === "info" ? "i" : "!"
            font.family: Theme.sans
            font.weight: Font.Bold
            font.pixelSize: root.small ? 10 : 12
            color: root.kind === "info" ? Theme.ink : Theme.inkOnAccent
        }
    }
    // The unwrapped width comes from a hidden copy: reading the wrapping label's own implicitWidth in its width binding
    // is a binding loop once the sentence wraps.
    Text {
        id: natural
        visible: false
        text: root.text
        textFormat: Text.PlainText
        font: label.font
    }
    Text {
        id: label
        visible: root.text !== ""
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(natural.implicitWidth, Math.max(0, root.maxWidth - (root.small ? 14 : 20) - root.spacing))
        text: root.text
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.roles.input.size
        color: root.kind === "error" ? Theme.danger : Theme.ink
    }
}
