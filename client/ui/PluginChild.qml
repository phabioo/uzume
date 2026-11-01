import QtQuick

// A nested block of a container. PluginBlock cannot instantiate its own type statically, so the child is loaded by file
// name; node, dark and pluginId stay bound, and the child's actions are re-emitted.
Loader {
    id: child
    property var node: ({})
    property bool dark: true
    property string pluginId: ""
    signal action(string action, var value)

    Component.onCompleted: setSource("PluginBlock.qml", {"nested": true, "node": child.node, "dark": child.dark, "pluginId": child.pluginId})
    Binding { target: child.item; property: "node"; value: child.node; when: child.item !== null }
    Binding { target: child.item; property: "dark"; value: child.dark; when: child.item !== null }
    Binding { target: child.item; property: "pluginId"; value: child.pluginId; when: child.item !== null }
    Connections {
        target: child.item
        function onAction(a, v) { child.action(a, v) }
    }
}
