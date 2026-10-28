import QtQuick
import QtQuick.Layouts
import Uzume.App

// uz.ui.pick_file without the file dialogs of QtQuick.Dialogs (not installed, or without Qt.labs.folderlistmodel): the person types or
// pastes the path of a file, like the other pickers do (Upload ROM, plugin install). The plugin still only gets the content.
// objectNames: pluginPickPathDialog, pluginPickPathField, pluginPickPathOk, pluginPickPathCancel.
UzDialog {
    id: root
    objectName: "pluginPickPathDialog"
    property var plugins: null
    property string pluginId: ""
    property string requestId: ""
    property var patterns: []
    property bool answered: false
    label: qsTr("Plugin · Choose a file")
    title: root.askTitle !== "" ? root.askTitle : qsTr("Choose a file")
    text: root.patterns.length > 0 ? qsTr("Type or paste the path of a file (%1).").arg(root.patterns.join(" ")) : qsTr("Type or paste the path of a file.")
    property string askTitle: ""

    function ask(pid: string, rid: string, ttl: string, filters: var) {
        root.pluginId = pid
        root.requestId = rid
        root.askTitle = ttl
        root.patterns = filters ?? []
        root.answered = false
        pathField.text = ""
        root.open()
        pathField.forceActiveFocus()
    }
    function finish(path: string) {
        if (root.answered) return
        root.answered = true
        if (root.plugins) root.plugins.completePick(root.pluginId, root.requestId, path)
    }
    onClosed: root.finish("")   // Esc / Cancel
    Connections {
        target: root.plugins
        // answered elsewhere (the debug MCP, or the plugin stopped): close without answering again
        function onPickCompleted(pid, rid) {
            if (pid !== root.pluginId || rid !== root.requestId || root.answered) return
            root.answered = true
            root.close()
        }
    }

    UzField {
        id: pathField
        objectName: "pluginPickPathField"
        Layout.fillWidth: true
        fill: Theme.ground
        mono: true
        placeholderText: qsTr("File path")
        onAccepted: okButton.clicked()
    }
    actions: [
        UzButton {
            id: okButton
            objectName: "pluginPickPathOk"
            kind: "primary"
            enabled: pathField.text.trim() !== ""
            text: qsTr("Choose")
            onClicked: { root.finish(pathField.text.trim()); root.close() }
        },
        UzButton { objectName: "pluginPickPathCancel"; text: qsTr("Cancel"); onClicked: root.close() }
    ]
}
