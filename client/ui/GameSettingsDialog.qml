import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Settings for this game (boards 3c-3 and 7-6): a lg Dialog over the window around GameSettingsContent (the tabs and rows of one
// game, see there) with "Reset all…" and "Done". It opens from the game page ("Settings for this game...") and from the "⋯" menu and
// the quick menu of the running game. Esc or B closes like Done.
//   open(gameId)
// objectNames: gameSettingsDialog, gameSettingsResetAll, gameSettingsResetConfirm, gameSettingsResetConfirmed, gameSettingsDone;
// the rest is in GameSettingsContent.
Item {
    id: root
    required property PlayerController player
    readonly property EmulationController emu: player.emulation
    readonly property alias gameId: content.gameId
    readonly property alias info: content.info
    readonly property alias content: content
    readonly property alias dialog: dlg
    signal closed()
    readonly property bool opened: dlg.shown

    function open(id: string) {
        content.showGame(id)
        dlg.open()
    }
    function close() { dlg.close() }

    UzDialog {
        id: dlg
        objectName: "gameSettingsDialog"
        parent: Window.contentItem
        size: "lg"
        onClosed: { resetConfirm.shown = false; root.closed() }
        label: content.info.systemLabel === undefined ? ""
               : qsTr("%1 · %2 · %3").arg(content.info.systemLabel).arg(content.info.coreName)
                   .arg(content.info.changedCount > 0 ? qsTr("%n change(s) from your defaults", "", content.info.changedCount) : qsTr("no changes from your defaults"))
        title: content.info.title === undefined ? qsTr("Settings for this game") : qsTr("Settings for %1").arg(content.info.title)

        GameSettingsContent {
            id: content
            player: root.player
            Layout.fillWidth: true
        }
        actions: [
            UzButton {
                objectName: "gameSettingsResetAll"
                text: qsTr("Reset all…")
                enabled: content.changedCount > 0
                onClicked: resetConfirm.open()
            },
            UzButton { objectName: "gameSettingsDone"; kind: "primary"; text: qsTr("Done"); onClicked: dlg.close() }
        ]
    }

    // "Reset all…" asks first: it drops every override of this game.
    UzDialog {
        id: resetConfirm
        objectName: "gameSettingsResetConfirm"
        parent: Window.contentItem
        z: 110
        label: content.info.title ?? ""
        title: qsTr("Reset all settings for this game?")
        text: qsTr("Every row goes back to your default for %1 in Emulation.").arg(content.info.systemLabel ?? "")
        actions: [
            UzButton { objectName: "gameSettingsResetCancel"; text: qsTr("Cancel"); onClicked: resetConfirm.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "gameSettingsResetConfirmed"
                kind: "danger"
                text: qsTr("Reset all")
                onClicked: { root.emu.resetGameOptions(content.gameId); resetConfirm.close() }
            }
        ]
        onClosed: dlg.forceActiveFocus()
    }
}
