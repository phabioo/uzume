import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// textarea block (0.20): multi-line input in the TextField frame; `mono` for codes, `lines` sets the visible height (default 6).
// the change fires action(action || id || "change", text) 300 ms after typing pauses and when focus leaves. `error` draws the
// danger frame and the message below. The text a plugin set is only taken over while the person is not typing in it.
ColumnLayout {
    id: root
    objectName: "pluginTextArea"
    property var node: ({})
    property bool dark: true
    property string pluginId: ""
    signal action(string action, var value)

    readonly property int lineCount: Math.max(1, Math.min(40, node.lines || 6))
    enabled: node.disabled !== true  // `disabled` (0.22)
    spacing: Theme.space05

    function fire() {
        changeTimer.stop()
        if (area.text === (root.node.value || "")) return
        root.action(root.node.action || root.node.id || "change", area.text)
    }

    Timer { id: changeTimer; interval: 300; onTriggered: root.fire() }

    ScrollView {
        id: scroll
        Layout.fillWidth: true
        Layout.preferredHeight: area.font.pixelSize * 1.35 * root.lineCount + 2 * Theme.space1
        clip: true
        background: Rectangle {
            color: Theme.dark ? "transparent" : Theme.surface
            border.width: 1
            border.color: !root.enabled ? Theme.disabledRule
                        : !!root.node.error ? Theme.danger
                        : area.activeFocus ? (Theme.dark ? Theme.accent : Theme.ink)
                        : Theme.ruleControl
        }
        TextArea {
            id: area
            objectName: "pluginTextAreaInput"
            text: root.node.value || ""
            placeholderText: root.node.placeholder || ""
            wrapMode: TextEdit.Wrap
            selectByMouse: true
            padding: Theme.space1
            font.family: root.node.mono === true ? Theme.mono : Theme.sans
            font.pixelSize: Theme.roles.input.size
            color: Theme.ink
            placeholderTextColor: Theme.muted
            selectionColor: Theme.accent
            selectedTextColor: Theme.inkOnAccent
            background: null
            Accessible.role: Accessible.EditableText
            Accessible.multiLine: true
            Accessible.name: root.node.placeholder || qsTr("Text")
            onTextChanged: if (activeFocus) changeTimer.restart()
            onActiveFocusChanged: if (!activeFocus) root.fire()
        }
    }
    Text {
        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
        objectName: "pluginTextAreaError"
        visible: !!root.node.error
        Layout.fillWidth: true
        text: root.node.error || ""
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.roles.small.size
        color: Theme.danger
    }
}
