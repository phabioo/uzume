import QtQuick

// Kbd (design-system.md "GamepadHint / Kbd"): the key frame for keyboard keys and combos, "Enter", "Esc", "Ctrl+Shift+U", with
// an optional label after it (muted 12). `keys` is the combo joined with "+" or a list.
Row {
    id: root
    property var keys: ""
    property string label: ""
    property bool pressed: false
    property bool disabled: false
    property real maxWidth: 0       // > 0: the label is elided so that the whole key stays inside this width (hint rows)
    readonly property var parts: Array.isArray(keys) ? keys : String(keys).split("+").map(s => s.trim()).filter(s => s !== "")
    spacing: 6
    Accessible.role: Accessible.StaticText
    Accessible.name: root.parts.join(" plus ") + (root.label !== "" ? " " + root.label : "")

    Row {
        id: caps
        spacing: 3
        anchors.verticalCenter: parent.verticalCenter
        Repeater {
            model: root.parts
            delegate: Row {
                required property string modelData
                required property int index
                spacing: 3
                Text {
                    visible: index > 0
                    anchors.verticalCenter: parent.verticalCenter
                    text: "+"
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta.size
                    color: Theme.muted
                }
                UzKeyCap { text: modelData; pressed: root.pressed; disabled: root.disabled }
            }
        }
    }
    Text {
        visible: root.label !== ""
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        textFormat: Text.PlainText
        elide: Text.ElideRight
        width: root.maxWidth > 0 ? Math.max(0, Math.min(implicitWidth, root.maxWidth - caps.width - root.spacing)) : implicitWidth
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        color: root.disabled ? Theme.disabledInk : Theme.muted
    }
}
