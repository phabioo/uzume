import QtQuick
import QtQuick.Layouts

// select block: options { value, label }; selected = value. Emits action(action, value). Drawn by UzSelect
// (design-system.md "Select", 36 px).
Item {
    id: root
    property var node: ({})
    property bool dark: true
    property string pluginId: ""
    signal action(string action, var value)

    enabled: node.disabled !== true  // `disabled` (0.22)
    implicitHeight: select.implicitHeight
    implicitWidth: 200
    Layout.minimumWidth: 0

    // UzSelect compares string values; the plugin's own value (number or string) is sent back unchanged.
    function originalValue(v) {
        const o = root.node.options || []
        for (let i = 0; i < o.length; ++i) {
            if (String(o[i].value) === String(v)) return o[i].value
        }
        return v
    }

    UzSelect {
        id: select
        anchors.fill: parent
        model: (root.node.options || []).map(o => ({ value: String(o.value), label: String(o.label) }))
        current: root.node.selected === undefined ? "" : String(root.node.selected)
        onPicked: (value) => root.action(root.node.action || "", root.originalValue(value))
    }
}
