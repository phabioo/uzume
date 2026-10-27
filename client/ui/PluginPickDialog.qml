import QtQuick
import QtQuick.Dialogs

// File dialog of fb.ui.pick_file. The plugin only ever gets the content of the chosen file, never its path.
FileDialog {
    id: dialog
    property var plugins: null
    property string pluginId: ""
    property string requestId: ""
    property var patterns: []
    fileMode: FileDialog.OpenFile
    nameFilters: dialog.patterns.length > 0 ? [qsTr("Files (%1)").arg(dialog.patterns.join(" ")), qsTr("All files (*)")] : [qsTr("All files (*)")]
    property bool done: false
    function ask(pid, rid, ttl, filters) {
        pluginId = pid
        requestId = rid
        title = ttl !== "" ? ttl : qsTr("Choose a file")
        patterns = filters
        done = false
        open()
    }
    onAccepted: { done = true; if (plugins) plugins.completePick(pluginId, requestId, selectedFile.toString()) }
    onRejected: { if (!done && plugins) plugins.completePick(pluginId, requestId, "") }
}
