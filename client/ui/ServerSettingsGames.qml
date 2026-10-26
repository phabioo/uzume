import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server › Games › settings (accounts-and-server.md 3q-app-1, moved out of General): Import folder (Change...) and Members can add
// games (toggle). A page of ServerSettingsPane that is not in its page list: the Games tab shows it with `forcePage: "games"`.
// From GET /admin/settings and PATCH /admin/settings; the import folder is the network setting `import_dir`.
// objectNames: settingsGames, generalImportRow, generalImportChange, generalUploadsRow, generalUploads.
ColumnLayout {
    id: root
    objectName: "settingsGames"
    required property var pane
    readonly property var s: root.pane.settings
    readonly property var importDir: root.pane.netSetting("import_dir")
    spacing: 0
    // The rows keep the reset column of UzSettingsRow free on the right (72 px); here nothing sits there, so the rows run on into it
    // and are clipped at the page edge: the controls end at the right edge of the page like the other blocks.
    clip: true

    Component.onCompleted: root.pane.helpItems = [
        { title: qsTr("Who this affects"), text: qsTr("These settings affect everyone on %1.").arg(root.pane.player.hubName) }
    ]

    UzSettingsRow {
        objectName: "generalImportRow"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.rightMargin: -Theme.settingsResetWidth
        resetMode: "none"
        idKey: "import"
        label: qsTr("Import folder")
        descriptionName: "generalImport"
        description: (root.importDir.value !== "" ? root.importDir.value + " · " : "") + qsTr("Files in it are added when you rescan the folders.")
        badge: root.importDir.restart_pending ? qsTr("applies after restart") : ""
        ctrl: "buttons"
        buttons: [
            { name: "generalImportChange", text: qsTr("Change…") },
            { name: "generalImportReset", text: qsTr("Use default"), visible: root.importDir.source === "web" }
        ]
        onButtonClicked: name => {
            if (name === "generalImportReset") {
                root.pane.resetNet("import_dir", qsTr("The import folder is back to the server's own value."))
                return
            }
            root.pane.openEdit({
                title: qsTr("Change the import folder"), text: qsTr("A folder on the server's computer. It applies after a restart."),
                label: qsTr("Folder"), value: root.importDir.value, mono: true, action: qsTr("Change"),
                save: function (v) { root.pane.saveNet("import_dir", { value: v }, qsTr("Saved the import folder.")) }
            })
        }
    }
    UzSettingsRow {
        objectName: "generalUploadsRow"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.rightMargin: -Theme.settingsResetWidth
        resetMode: "none"
        idKey: "uploads"
        label: qsTr("Members can add games")
        description: qsTr("Members upload game files from their own PC. Off: only admins add games.")
        ctrl: "toggle"
        controlName: "generalUploads"
        checked: root.s.allow_user_uploads === true
        onToggled: value => root.pane.patchSettings({ allow_user_uploads: value },
                                                    value ? qsTr("Members can add games now.") : qsTr("Only admins can add games now."))
    }
}
