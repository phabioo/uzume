import QtQuick
import QtQuick.Layouts

// toggle block: optional label (Accessible.name; else the optional `accessible`), switch (UzToggle, design-system.md "Toggle"). Emits action(action, newBool); the state
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
        // The spoken name: the label, else the optional `accessible` field (a toggle without a visible label, e.g. one per list row).
        Accessible.name: root.node.label || root.node.accessible || ""
        checked: root.checked
        enabled: root.node.disabled !== true
        onToggled: {
            // with a `value` the action gets {checked, value}, otherwise the new bool
            root.action(root.node.action || "", root.node.value !== undefined ? { checked: toggle.checked, value: root.node.value } : toggle.checked)
            toggle.checked = Qt.binding(() => root.checked)
        }
    }
}
