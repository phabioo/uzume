import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server › Settings › General (accounts-and-server.md 3q-app-1): Server name (Rename...) and Run setup again (Open..., only where
// the app has the setup assistant). From GET /admin/settings and PATCH /admin/settings. The import folder and "Members can add
// games" moved to the Games tab (ServerSettingsGames).
// Left out, the API carries nothing for it: "Joining" (Invite only is the only way to join).
// objectNames: settingsGeneral, generalNameRow, generalRename, generalSetupRow, generalSetupOpen.
ColumnLayout {
    id: root
    objectName: "settingsGeneral"
    required property var pane
    readonly property var s: root.pane.settings
    readonly property var importDir: root.pane.netSetting("import_dir")
    readonly property bool setupAvailable: typeof root.pane.player.showSetupAssistant === "function"
    spacing: 0

    Component.onCompleted: root.pane.helpItems = [
        { title: qsTr("Who this affects"), text: qsTr("Settings here affect everyone on %1. Your own settings are under This PC.").arg(root.pane.player.hubName) },
        { title: qsTr("Invites"), text: qsTr("Invites are made in People › Invites.") }
    ]

    UzSettingsRow {
        objectName: "generalNameRow"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        idKey: "name"
        label: qsTr("Server name")
        descriptionName: "generalName"
        description: root.s.hub_name ?? ""
        ctrl: "buttons"
        buttons: [{ name: "generalRename", text: qsTr("Rename…") }]
        onButtonClicked: root.pane.openEdit({
            title: qsTr("Rename the server"), text: qsTr("Apps show this name in their server list."), label: qsTr("Server name"),
            value: root.s.hub_name, action: qsTr("Rename"),
            save: function (v) { root.pane.patchSettings({ name: v }, qsTr("Renamed the server to %1.").arg(v)) }
        })
    }
    UzSettingsRow {
        objectName: "generalSetupRow"
        visible: root.setupAvailable
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        idKey: "setup"
        label: qsTr("Run setup again")
        description: qsTr("Opens the setup assistant. Nothing is reset.")
        ctrl: "buttons"
        buttons: [{ name: "generalSetupOpen", text: qsTr("Open…") }]
        onButtonClicked: root.pane.player.showSetupAssistant()
    }
    UzSettingsRow {
        objectName: "generalRunsOnRow"
        visible: root.pane.player.localHub.mode === "service"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        idKey: "runson"
        label: qsTr("Runs on")
        description: qsTr("This PC · Windows service")
        ctrl: "buttons"
        buttons: [{ name: "generalRunsOnStatus", text: qsTr("Open Overview") }]
        onButtonClicked: root.pane.player.showServerPage("overview")
    }
    UzSettingsRow {
        objectName: "generalStopSharingRow"
        visible: root.pane.player.localHub.canStopSharing
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        idKey: "stopsharing"
        label: qsTr("Sharing")
        description: qsTr("Stop sharing and go back to using the server only on this PC.")
        ctrl: "buttons"
        buttons: [{ name: "generalStopSharing", text: qsTr("Stop sharing…"), kind: "danger" }]
        onButtonClicked: root.pane.player.localHub.openShareDialog("stop")
    }
}
