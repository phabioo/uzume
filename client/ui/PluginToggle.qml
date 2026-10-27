import QtQuick
import QtQuick.Layouts

// toggle block: optional label, switch (UzToggle, design-system.md "Toggle"). Emits action(action, newBool); the state
// shown always follows the plugin's `checked`.
Item {
    id: root
    property var node: ({})
    property bool dark: true
    property string pluginId: ""
    readonly property bool checked: node.checked === true
    signal action(string action, var value)

    implicitHeight: toggle.implicitHeight
    implicitWidth: toggle.implicitWidth

    UzToggle {
        id: toggle
        objectName: "pluginToggleSwitch"
        anchors.fill: parent
        text: root.node.label || ""
        checked: root.checked
        enabled: root.node.disabled !== true
        onToggled: {
            // with a `value` the action gets {checked, value}, otherwise the new bool
            root.action(root.node.action || "", root.node.value !== undefined ? { checked: toggle.checked, value: root.node.value } : toggle.checked)
            toggle.checked = Qt.binding(() => root.checked)
        }
    }
}
