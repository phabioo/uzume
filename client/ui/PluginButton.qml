import QtQuick
import QtQuick.Layouts

// button block: variant primary | secondary | quiet | danger; emits action(action, value) after the optional confirmation.
// Drawn by UzButton (design-system.md "Button", 32 px): primary = Yamabuki plane, secondary = outline, quiet = ghost,
// danger = outlined danger.
Item {
    id: root
    property var node: ({})
    property bool dark: true
    property string pluginId: ""
    readonly property string variant: node.variant || "secondary"
    signal action(string action, var value)

    enabled: node.disabled !== true  // `disabled` (0.22): greyed out, no action
    implicitHeight: button.implicitHeight
    implicitWidth: button.implicitWidth
    Layout.minimumWidth: implicitWidth

    function fire() {
        if (node.disabled === true) return
        if (node.confirm) {
            dialogLoader.active = true
            dialogLoader.item.open()
        } else {
            root.action(node.action || "", node.value)
        }
    }

    UzButton {
        id: button
        anchors.fill: parent
        size: "sm"
        kind: root.variant === "primary" ? "primary" : root.variant === "danger" ? "danger" : root.variant === "quiet" ? "ghost" : "outline"
        text: root.node.label || ""
        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
        onClicked: root.fire()
    }

    Loader {
        id: dialogLoader
        active: false
        sourceComponent: PluginConfirmDialog {
            dark: root.dark
            confirm: root.node.confirm || ({})
            onAccepted: root.action(root.node.action || "", root.node.value)
        }
    }
}
