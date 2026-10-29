import QtQuick
import QtQuick.Dialogs

// Native folder dialog of Settings > Plugins (PluginsSection.qml loads it through a Loader and falls back to a path
// field when QtQuick.Dialogs is not installed). mode: "install" (copies the folder) | "link" (Load unpacked: runs from the folder).
FolderDialog {
    id: dialog
    property var plugins: null
    property string mode: "install"
    title: mode === "link" ? qsTr("Plugin folder to load") : qsTr("Plugin folder to install")
    onAccepted: {
        if (dialog.plugins === null) return
        const path = dialog.selectedFolder.toString()
        if (dialog.mode === "link") dialog.plugins.prepareUnpacked(path)
        else dialog.plugins.installFromFolder(path)
    }
}
