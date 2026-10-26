import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server › Settings: the seven pages of ServerSettingsPane (General · Network & sharing · Sign-in · Updates · Backups · Storage ·
// Diagnostics) as a column of their own at the left (UzColumnNav, the same column as in Settings) and the shown page at the right:
// AppNav, column and page are the three columns at 1280 px. The page is `player.serverSettingsPage` (setServerSettingsPage), so
// `showSettingsSection("server:updates")` opens it here. Right moves from the column into the page, Left / B / Esc back
// (`handleBack`, which ServerScreen asks before it moves the focus to the tab row).
// objectNames: serverSettingsTab, serverSettingsColumnNav (plus serverSettingsPane, serverSettingsNav_<id> of the pane).
AdminPage {
    id: root
    objectName: "serverSettingsTab"
    area: qsTr("Settings")
    title: qsTr("Settings")
    label: ""

    // True when the focus moved from the page back to the column.
    function handleBack(): bool { return nav.backFromContent() }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space4

        UzColumnNav {
            id: nav
            objectName: "serverSettingsColumnNav"
            Layout.fillWidth: false
            Layout.minimumWidth: 200
            Layout.preferredWidth: 200
            Layout.maximumWidth: 200
            Layout.alignment: Qt.AlignTop
            content: pane
            spacing: 0
            onContentRequested: nav.focusFirstIn(pane)
            Repeater {
                model: pane.pages
                delegate: pane.navDelegate
            }
        }
        ServerSettingsPane {
            id: pane
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            player: root.player
        }
    }
}
