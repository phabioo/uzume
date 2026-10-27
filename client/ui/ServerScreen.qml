import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server (usability pass, board "6 Server: one entry, tabs, overview first"): the one place of the admin area. AppNav at the
// left (SERVER group with the single item Server), at the right the page header (server name, address · state · version) and
// the tabs Overview · People · Games · Systems & cores · Tools · Plugins · Reports · Settings. The tab is `player.serverPage`
// (overview | people | games | systems | tools | plugins | reports | settings; showServerPage also takes the old ids status and users). Tab
// pages are built on AdminPage (frame without a page header), UzTable, UzTableRow and UzMetric and talk to the server through
// `player.admin`. Settings holds the pages of ServerSettingsPane. Members (and admins of a server without `admin_v1`) see a
// short block with a way back.
//
// Back (B, Esc, Left): from the page content to the tab row (first the column of the Settings tab), then Main moves to the AppNav.
// Down on the tab row moves into the page.
//
// Adding a tab: write `Server<Name>Page.qml` on AdminPage, add a Component for it below, an entry in `tabs` and a branch in
// `pageComponent`; `showServerPage` (C++) turns unknown names into "overview". Pages are created only while shown.
Rectangle {
    id: root
    objectName: "serverScreen"
    required property PlayerController player
    readonly property AdminController admin: root.player.admin
    readonly property string page: root.player.serverPage
    // The server of "Play on this PC" is not a server to the person: Overview, People and Reports need a server (8j-4); Games,
    // Systems & cores, Plugins and Settings are about the server of this PC itself.
    readonly property bool pcOnly: root.player.localHub.onThisPcOnly
    readonly property bool pcBlocked: root.pcOnly && (root.page === "overview" || root.page === "people" || root.page === "reports")
    readonly property real pageMargin: Theme.space7
    readonly property real contentWidth: Math.max(0, Math.min(Theme.contentMaxWidth, width - Theme.sidebarWidth - 2 * root.pageMargin))
    // id, tab label and the count of the old AppNav badge (open problems, waiting devices, core problems, new reports).
    readonly property var tabs: [
        { id: "overview", label: qsTr("Overview"), count: root.player.attentionCount, needsServer: true },
        { id: "people", label: qsTr("People"), count: root.admin.pendingDevices, needsServer: true },
        { id: "games", label: qsTr("Games"), count: 0, needsServer: false },
        { id: "systems", label: qsTr("Systems & cores"), count: root.player.coreProblemCount, needsServer: false },
        { id: "tools", label: qsTr("Tools"), count: 0, needsServer: false },
        { id: "plugins", label: qsTr("Plugins"), count: 0, needsServer: false },
        { id: "reports", label: qsTr("Reports"), count: root.admin.newReports, needsServer: true },
        { id: "settings", label: qsTr("Settings"), count: 0, needsServer: false }
    ]
    readonly property var shownTabs: root.tabs.filter(function (t) { return !(root.pcOnly && t.needsServer) })
    readonly property var tabCounts: {
        const out = ({})
        for (const t of root.tabs) if (t.count > 0) out[t.label] = t.count
        return out
    }
    readonly property string activeLabel: {
        for (const t of root.tabs) if (t.id === root.page) return t.label
        return ""
    }
    readonly property string metaLine: {
        const parts = []
        if (!root.pcOnly && root.player.hubAddress !== "") parts.push(root.player.hubAddress)
        parts.push(root.player.hubLink === "online" ? qsTr("Online") : root.player.hubLink === "connecting" ? qsTr("Connecting") : qsTr("Offline"))
        const v = root.admin.status.hub_version ?? root.player.hubVersion
        if (v) parts.push(qsTr("Version %1").arg(v))
        return parts.join(" · ")
    }
    readonly property Component pageComponent: {
        switch (root.page) {
        case "overview": return statusPage
        case "people": return usersPage
        case "games": return gamesPage
        case "systems": return systemsPage
        case "tools": return toolsPage
        case "plugins": return pluginsPage
        case "reports": return reportsPage
        case "settings": return settingsPage
        default: return null
        }
    }
    color: Theme.ground

    // B / Esc / Left: the Settings tab first leaves its page for its column, then the content goes to the tab row. False when the
    // focus is on the tab row already (or elsewhere): Main moves on to the AppNav.
    function handleBack(): bool {
        const page = pageLoader.item
        if (page && page.handleBack !== undefined && page.handleBack()) return true
        for (let p = root.Window.activeFocusItem; p; p = p.parent) {
            if (p === pageLoader) {
                serverTabs.forceActiveFocus(Qt.BacktabFocusReason)
                return true
            }
        }
        return false
    }

    Component { id: statusPage; ServerStatusPage { player: root.player } }
    Component { id: usersPage; ServerUsersPage { player: root.player } }
    Component { id: gamesPage; ServerGamesPage { player: root.player } }
    Component { id: reportsPage; ServerReportsPage { player: root.player } }
    Component { id: systemsPage; ServerSystemsPage { player: root.player } }
    Component { id: toolsPage; ServerToolsPage { player: root.player } }
    Component { id: pluginsPage; ServerPluginsPage { player: root.player } }
    Component { id: settingsPage; ServerSettingsPage { player: root.player } }

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

            UzPageHeader {
                id: pageHeader
                objectName: "serverHeader"
                titleName: "serverTitle"
                visible: root.admin.available
                x: root.pageMargin
                y: 48
                width: root.contentWidth
                number: root.pcOnly ? -1 : 1
                label: root.pcOnly ? qsTr("This PC") : qsTr("Server")
                title: root.admin.status.hub_name ? root.admin.status.hub_name : (root.player.hubName !== "" ? root.player.hubName : qsTr("Server"))
                meta: root.metaLine
            }
            // Natural width of the tab row: when it does not fit (wide fonts, translations) the tabs share the width and elide.
            UzTabs {
                id: tabsProbe
                visible: false
                size: "lg"
                items: serverTabs.items
                counts: serverTabs.counts
            }
            UzTabs {
                id: serverTabs
                objectName: "serverTabs"
                visible: root.admin.available
                size: "lg"
                x: root.pageMargin
                y: pageHeader.y + pageHeader.height + Theme.space3
                width: root.contentWidth
                fit: root.contentWidth > 0 && tabsProbe.implicitWidth > root.contentWidth
                items: root.shownTabs.map(function (t) { return t.label })
                active: root.activeLabel
                counts: root.tabCounts
                onPicked: label => {
                    for (const t of root.tabs) if (t.label === label) { root.player.showServerPage(t.id); break }
                }
                // Down: into the page (the first control after the tab row).
                Keys.onDownPressed: (event) => {
                    const next = serverTabs.nextItemInFocusChain(true)
                    for (let p = next; p; p = p.parent) {
                        if (p === pageLoader) { next.forceActiveFocus(Qt.TabFocusReason); event.accepted = true; return }
                    }
                }
            }

            Loader {
                id: pageLoader
                objectName: "serverPageLoader"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: serverTabs.bottom
                anchors.bottom: parent.bottom
                active: root.admin.available && !root.pcBlocked && root.player.screen === "server" && root.pageComponent !== null
                sourceComponent: root.pageComponent
            }

            // People (and the other pages that need a server) on the server of this PC only: the person is alone here.
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
                        title: root.page === "people" ? qsTr("People needs a server") : qsTr("This page needs a server")
                        text: root.page === "people" ? qsTr("On this PC there is only you.") : qsTr("On this PC there is only you, so there is nothing to manage here.")
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
