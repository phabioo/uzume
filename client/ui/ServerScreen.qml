import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server (0.18 package 8, accounts-and-server.md "Server"): the admin area. AppNav at the left (SERVER group), the page chosen
// by `player.serverPage` at the right. Admin pages are built on AdminPage (frame), UzTable, UzTableRow and UzMetric and talk to the
// server through `player.admin`. Members (and admins of a server without `admin_v1`) see a short block with a way back.
//
// Adding a page: write `Server<Name>Page.qml` on AdminPage, add a Component for it below and a branch in `pageComponent`;
// `showServerPage` turns unknown names into "status". Pages are created only while shown.
Rectangle {
    id: root
    objectName: "serverScreen"
    required property PlayerController player
    readonly property AdminController admin: root.player.admin
    readonly property string page: root.player.serverPage
    // The server of "Play on this PC" is only Systems & cores and Plugins to the person; Users, Status and Reports need a server (8j-4).
    readonly property bool pcOnly: root.player.localHub.onThisPcOnly
    readonly property bool pcBlocked: root.pcOnly && root.page !== "systems" && root.page !== "plugins"
    readonly property Component pageComponent: {
        switch (root.page) {
        case "status": return statusPage
        case "users": return usersPage
        case "reports": return reportsPage
        case "systems": return systemsPage
        case "plugins": return pluginsPage
        default: return null
        }
    }
    color: Theme.ground

    Component { id: statusPage; ServerStatusPage { player: root.player } }
    Component { id: usersPage; ServerUsersPage { player: root.player } }
    Component { id: reportsPage; ServerReportsPage { player: root.player } }
    Component { id: systemsPage; ServerSystemsPage { player: root.player } }
    Component { id: pluginsPage; ServerPluginsPage { player: root.player } }

    UzPageHints {
        id: pageHints
        player: root.player
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }

    RowLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: pageHints.top
        spacing: 0

        UzAppNav {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.sidebarWidth
            player: root.player
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Loader {
                id: pageLoader
                objectName: "serverPageLoader"
                anchors.fill: parent
                active: root.admin.available && !root.pcBlocked && root.player.screen === "server" && root.pageComponent !== null
                sourceComponent: root.pageComponent
            }

            // Users (and the other server pages) on the server of this PC only: the person is alone here.
            Item {
                objectName: "serverNeedsServer"
                anchors.fill: parent
                visible: root.pcBlocked
                Column {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 2 * Theme.space7, 480)
                    spacing: Theme.space2
                    UzStateBlock {
                        objectName: "serverNeedsServerState"
                        width: parent.width
                        kind: "empty"
                        title: root.page === "users" ? qsTr("Users needs a server") : qsTr("This page needs a server")
                        text: root.page === "users" ? qsTr("On this PC there is only you.") : qsTr("On this PC there is only you, so there is nothing to manage here.")
                        action: root.player.localHub.shareSupported ? qsTr("Share with others…") : ""
                        onActionClicked: root.player.localHub.openShareDialog("intro")
                    }
                }
            }

            // Members and admins of a server without the admin pages.
            Item {
                objectName: "serverMemberBlock"
                anchors.fill: parent
                visible: !root.admin.available && !root.pcBlocked
                Column {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 2 * Theme.space7, 480)
                    spacing: Theme.space2
                    UzStateBlock {
                        objectName: "serverMemberState"
                        width: parent.width
                        kind: "empty"
                        title: root.player.isAdmin ? qsTr("This server can't be managed from the app yet")
                                                   : qsTr("Only admins see server status")
                        text: root.player.isAdmin ? qsTr("It does not offer the admin pages. Update the server, then come back.")
                                                  : qsTr("These pages are for the admins of %1. Ask one of them if something needs to change.").arg(root.player.hubName)
                        action: qsTr("Back to Library")
                        onActionClicked: root.player.showLibrary()
                    }
                    UzButton {
                        objectName: "serverMemberReport"
                        anchors.horizontalCenter: parent.horizontalCenter
                        kind: "link"
                        text: qsTr("Report a problem…")
                        onClicked: root.player.report.show()
                    }
                }
            }
        }
    }
}
