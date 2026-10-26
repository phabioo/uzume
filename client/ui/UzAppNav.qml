import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// AppNav (frame.md "0-4 A" to "0-4 C" and "0-4 G"): the 232 px navigation of every screen. Lockup, PLAY (Library, Runs;
// Emulation, Controls and Hotkeys are Settings sections), the SERVER group for admins or THIS PC when there is no server, ONLINE (people with a Session),
// Settings, the server line and the person. Compact (window below 900 px high): 28 px items, one-line ONLINE rows, no address line.
// SERVER: Status (count of open problems), Users (count of waiting devices), Systems & cores (count of core problems),
// Plugins, Reports (count of New). Settings carries a badge while a server update is on offer. All badges are admin-only, above 0.
Rectangle {
    id: root
    required property PlayerController player
    readonly property bool compact: height < 900
    readonly property bool hasServer: root.player.hubAddress !== ""
    // The server of "Play on this PC" is not a server to the person (B-3): THIS PC instead of SERVER, no ONLINE, no server line.
    readonly property bool thisPcOnly: root.player.localHub.onThisPcOnly
    readonly property bool serverMode: root.hasServer && !root.thisPcOnly
    // SERVER (admins of a server with admin_v1): the pages of the server screen; `page` goes to player.showServerPage().
    readonly property var serverItems: [
        { label: qsTr("Status"), name: "navServerStatus", page: "status" },
        { label: qsTr("Users"), name: "navServerUsers", page: "users" },
        { label: qsTr("Systems & cores"), name: "navServerSystems", page: "systems" },
        { label: qsTr("Plugins"), name: "navServerPlugins", page: "plugins" },
        { label: qsTr("Reports"), name: "navServerReports", page: "reports" }
    ]
    readonly property var thisPcItems: [{ label: qsTr("Systems & cores"), name: "navThisPcSystems", page: "systems" },
                                        { label: qsTr("Plugins"), name: "navThisPcPlugins", section: "plugins" }]
    // The Repeater below gets every entry once and hides the ones that do not apply. Swapping the model with the connection state
    // would destroy the delegates inside a layout that is hidden at that moment, and Qt 6.4 then crashes in
    // QQuickGridLayoutBase::rearrange on the deleted items.
    readonly property var groupEntries: serverItems.map(function (e) { return { label: e.label, name: e.name, page: e.page, server: true } })
                                        .concat(thisPcItems.map(function (e) { return { label: e.label, name: e.name, page: e.page, section: e.section, server: false } }))
    readonly property bool hasGroupItems: root.serverMode ? (root.player.admin.available && serverItems.length > 0) : thisPcItems.length > 0
    readonly property var online: root.serverMode ? root.player.sessions.sessions.filter(function (s) { return !s.invited }) : []
    color: Theme.ground
    implicitWidth: Theme.sidebarWidth
    objectName: "appNav"

    // True when the focus is inside the AppNav.
    function hasFocus(): bool {
        for (let p = root.Window.activeFocusItem; p; p = p.parent) {
            if (p === root) return true
        }
        return false
    }
    // B / Esc on a main page: the active item (else the first one) of the AppNav takes the focus.
    function focusActive(): bool {
        let first = null
        let active = null
        const walk = function (it) {
            for (let i = 0; i < it.children.length; ++i) {
                const c = it.children[i]
                if (!c.visible) continue
                if (c.activeFocusOnTab === true && c.enabled !== false) {
                    if (first === null) first = c
                    if (active === null && c.active === true) active = c
                }
                walk(c)
            }
        }
        walk(root)
        const target = active || first
        if (!target) return false
        target.forceActiveFocus(Qt.BacktabFocusReason)
        return true
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.rule
    }

    UzMenu {
        id: serverMenu
        objectName: "serverMenu"
        items: [{ label: qsTr("Switch server"), name: "switchServer" },
                { label: qsTr("Switch user…"), name: "switchUser", shortcut: "Ctrl+Shift+U" },
                { label: qsTr("Details"), name: "serverDetails" }]
        onChosen: (item) => {
            if (item.name === "switchServer") root.player.switchHub()
            else if (item.name === "switchUser") root.player.accounts.open()
            else root.player.showSettingsSection("hubs")
        }
    }
    UzMenu {
        id: personMenu
        objectName: "personMenu"
        items: [{ label: qsTr("My account"), name: "account" },
                { label: qsTr("Switch user…"), name: "switchUser", shortcut: "Ctrl+Shift+U" },
                { separator: true },
                { label: qsTr("Settings"), name: "settings", shortcut: "Ctrl+," },
                { label: qsTr("Switch server"), name: "switchServer" }]
        onChosen: (item) => {
            if (item.name === "account") root.player.showSettingsSection("account")
            else if (item.name === "switchUser") root.player.accounts.open()
            else if (item.name === "settings") root.player.showSettings()
            else root.player.switchHub()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 32
        anchors.bottomMargin: Theme.space2
        spacing: 0

        // Lockup: 32 px from the top, 28 px from the left. It never widens the column (the wordmark is short).
        Item {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: lockup.implicitHeight
            UzLockup { id: lockup; objectName: "sidebarLockup"; x: Theme.navInset }
        }

        UzNavGroupHead { Layout.fillWidth: true; Layout.topMargin: root.compact ? 20 : 28; text: qsTr("Play") }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.space1
            spacing: 0
            UzNavItem {
                objectName: "navLibrary"
                compact: root.compact
                label: qsTr("Library")
                active: root.player.screen === "library"
                onActivated: root.player.showLibrary()
            }
            // Runs: always listed; badge = open invites.
            UzNavItem {
                objectName: "navRuns"
                compact: root.compact
                label: qsTr("Runs")
                active: root.player.screen === "runs"
                badge: root.player.runs.pendingInvites
                badgeName: "navRunsBadge"
                onActivated: root.player.showRuns()
            }
        }

        // SERVER (admins of a server with admin_v1) or THIS PC (no server).
        UzNavGroupHead {
            objectName: "navServerHead"
            Layout.fillWidth: true
            Layout.topMargin: root.compact ? 16 : 24
            visible: root.hasGroupItems
            text: root.serverMode ? qsTr("Server") : qsTr("This PC")
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.space1
            spacing: 0
            visible: root.hasGroupItems
            Repeater {
                id: groupItems
                model: root.groupEntries
                delegate: UzNavItem {
                    required property var modelData
                    // Systems & cores of THIS PC is a page of the server on this PC: only while that one answers.
                    visible: modelData.server ? (root.serverMode && root.player.admin.available)
                                              : (!root.serverMode && (modelData.page === undefined || (root.hasServer && root.player.admin.available)))
                    objectName: modelData.name
                    compact: root.compact
                    label: modelData.label
                    active: modelData.page !== undefined && root.player.screen === "server" && root.player.serverPage === modelData.page
                    // Neutral count badges, admins only and only above 0: open problems (Status), waiting devices (Users), core problems
                    // (Systems & cores), new reports (Reports).
                    badge: !modelData.server ? 0
                           : modelData.page === "status" ? root.player.attentionCount
                           : modelData.page === "users" ? root.player.admin.pendingDevices
                           : modelData.page === "systems" ? root.player.coreProblemCount
                           : modelData.page === "reports" ? root.player.admin.newReports : 0
                    badgeName: !modelData.server ? ""
                               : modelData.page === "status" ? "navServerStatusBadge"
                               : modelData.page === "users" ? "navServerUsersBadge"
                               : modelData.page === "systems" ? "navServerSystemsBadge"
                               : modelData.page === "reports" ? "navServerReportsBadge" : ""
                    onActivated: modelData.page !== undefined ? root.player.showServerPage(modelData.page)
                                                              : root.player.showSettingsSection(modelData.section)
                }
            }
        }

        // ONLINE: people with a shared Session and what they play; hidden without data.
        UzNavGroupHead {
            objectName: "navOnlineHead"
            Layout.fillWidth: true
            Layout.topMargin: root.compact ? 16 : 24
            visible: root.online.length > 0
            text: qsTr("Online")
        }
        ColumnLayout {
            objectName: "navOnline"
            Layout.fillWidth: true
            Layout.topMargin: Theme.space1
            spacing: 0
            visible: root.online.length > 0
            // Six fixed delegates, shown while there is an entry: a model that follows the Sessions list would rebuild the rows on
            // every refresh (see groupEntries above).
            Repeater {
                model: 6
                delegate: Item {
                    id: person
                    required property int index
                    readonly property var modelData: root.online[index] || ({ who: "", game: "" })
                    visible: index < root.online.length
                    objectName: "navOnlineRow"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: root.compact ? Theme.row28 : 40
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("%1 plays %2").arg(modelData.who).arg(modelData.game)
                    UzAvatar {
                        x: Theme.navInset
                        anchors.verticalCenter: parent.verticalCenter
                        size: 20
                        name: person.modelData.who
                        person: person.index + 1
                    }
                    Column {
                        x: Theme.navInset + 20 + Theme.space1
                        width: parent.width - x - Theme.space2
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: root.compact ? person.modelData.who + " · " + person.modelData.game : person.modelData.who
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.input.size
                            color: Theme.ink2
                        }
                        Text {
                            visible: !root.compact
                            width: parent.width
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: person.modelData.game
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.small.size
                            color: Theme.muted
                        }
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }

        // Now running (3c-5): the game paused in the background, on every page; disappears when the game quits.
        Rectangle {
            id: nowRunning
            objectName: "nowRunningStrip"
            readonly property var game: root.player.backgroundGame
            readonly property bool shown: game.id !== undefined
            readonly property bool quitting: root.player.quitting
            property real nowMs: Date.now()
            readonly property int minutes: shown && game.startedMs ? Math.max(0, Math.floor((nowMs - game.startedMs) / 60000)) : 0
            visible: shown
            Layout.fillWidth: true
            Layout.leftMargin: Theme.space2
            Layout.rightMargin: Theme.space2
            implicitHeight: nrCol.implicitHeight + 24
            radius: 0
            color: Theme.selected
            border.width: 1
            border.color: Theme.ruleControl
            Timer { running: nowRunning.shown; interval: 30000; repeat: true; onTriggered: nowRunning.nowMs = Date.now() }
            onShownChanged: nowMs = Date.now()
            ColumnLayout {
                id: nrCol
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    UzMono {
                        Layout.minimumWidth: 0
                        text: qsTr("NOW RUNNING")
                        font.pixelSize: Theme.roles.meta.size
                        font.letterSpacing: 0.8
                        font.weight: Font.Medium
                        color: Theme.ink
                    }
                    Item { Layout.fillWidth: true }
                    UzLabel {
                        objectName: "nowRunningTime"
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        text: qsTr("%1 min").arg(nowRunning.minutes)
                        font.pixelSize: 11
                        color: Theme.muted
                    }
                }
                UzLabel {
                    objectName: "nowRunningTitle"
                    Layout.fillWidth: true
                    text: nowRunning.game.title || ""
                    elide: Text.ElideRight
                    font.pixelSize: Theme.roles.body.size
                    font.weight: Font.DemiBold
                }
                UzLabel {
                    objectName: "nowRunningStatus"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.ink2
                    text: nowRunning.quitting ? qsTr("Saving and syncing…")
                          : root.player.sessions.watching ? qsTr("Paused while you watch %1 · %2").arg(root.player.sessions.watchedWho).arg(root.player.sessions.watchedGame)
                          : qsTr("Paused")
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    spacing: 6
                    visible: !nowRunning.quitting
                    UzButton {
                        objectName: "nowRunningResume"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        kind: "primary"
                        implicitHeight: 30
                        implicitWidth: 0
                        font.pixelSize: Theme.roles.input.size
                        text: qsTr("▶ Resume")
                        onClicked: root.player.resumeGame()
                    }
                    UzButton {
                        objectName: "nowRunningQuit"
                        implicitHeight: 30
                        implicitWidth: 0
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: quitProbe.implicitWidth + 20
                        font.pixelSize: Theme.roles.input.size
                        text: qsTr("Quit")
                        background: Rectangle {
                            radius: Theme.radiusNone
                            color: parent.hovered ? Theme.hover : "transparent"
                            border.width: 1
                            border.color: Theme.ruleControl
                        }
                        onClicked: root.player.requestQuit()
                        Text { id: quitProbe; visible: false; text: qsTr("Quit"); font.pixelSize: Theme.roles.input.size }
                    }
                }
            }
        }

        UzNavItem {
            objectName: "navSettings"
            compact: root.compact
            Layout.topMargin: Theme.space1
            label: qsTr("Settings")
            active: root.player.screen === "settings"
            // A newer app is available: small Yamabuki square on Settings.
            dot: root.player.updates.updateAvailable
            dotName: "settingsUpdateDot"
            // A server update is on offer (admins only): count badge 1.
            badge: root.player.serverUpdatePending ? 1 : 0
            badgeName: "navSettingsBadge"
            onActivated: root.player.showSettings()
        }

        // Server line: online (ok square + ONLINE, address in Mono) or offline (empty square + OFFLINE); opens Settings > Servers.
        // The page text for offline ("Games on this PC still run...") lives on the screens (hubLinkBanner).
        Rectangle { Layout.fillWidth: true; Layout.topMargin: Theme.space1; Layout.preferredHeight: 1; color: Theme.rule2; visible: root.serverMode }
        Item {
            id: serverLine
            visible: root.serverMode
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.topMargin: Theme.space15
            implicitHeight: Math.max(statusLine.implicitHeight, Theme.row32)
            UzStatusLine {
                id: statusLine
                objectName: "hubStatusDot"
                x: Theme.navInset
                anchors.verticalCenter: parent.verticalCenter
                status: root.player.hubLink === "online" ? "online" : "offline"
                name: root.player.hubName
                address: root.player.hubAddress
                compact: root.compact
                maxWidth: parent.width - Theme.navInset - Theme.row32 - Theme.space2
            }
            TapHandler { onTapped: root.player.showSettingsSection("hubs") }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            UzIconButton {
                id: serverMore
                objectName: "serverMenuButton"
                x: parent.width - width - Theme.space1
                anchors.verticalCenter: parent.verticalCenter
                glyph: "more"
                Accessible.name: qsTr("Server menu")
                onClicked: serverMenu.openFor(serverMore, "above", true)
            }
        }
        UzMono {
            objectName: "hubCardLinkNote"
            visible: root.serverMode && root.player.hubLink === "connecting"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.navInset
            Layout.rightMargin: Theme.space2
            text: qsTr("Connecting…")
            color: Theme.muted
            font.pixelSize: Theme.roles.meta11.size
            elide: Text.ElideRight
        }

        // The person at the foot: user name above the device name (both lines are always present, only their text changes).
        Item {
            id: userBlock
            objectName: "userBlock"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.topMargin: Theme.space15
            implicitHeight: 40
            readonly property bool hasUser: root.player.userName !== ""
            Rectangle {
                id: avatarBox
                x: Theme.navInset
                anchors.verticalCenter: parent.verticalCenter
                width: 28
                height: 28
                radius: 0
                color: Theme.hover
                border.width: 1
                border.color: Theme.rule
                Text {
                    objectName: "userAvatarInitial"
                    anchors.centerIn: parent
                    text: (root.player.userName || root.player.deviceName || "?").charAt(0).toUpperCase()
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    font.weight: Font.DemiBold
                    color: Theme.ink2
                }
            }
            ColumnLayout {
                x: avatarBox.x + avatarBox.width + Theme.space15
                width: parent.width - x - Theme.space2
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0
                UzLabel {
                    objectName: "userName"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: userBlock.hasUser ? root.player.userName : root.player.deviceName
                    font.pixelSize: Theme.roles.input.size
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                UzLabel {
                    objectName: "userDevice"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: userBlock.hasUser ? root.player.deviceName : qsTr("This device")
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.muted
                    elide: Text.ElideRight
                }
            }
            activeFocusOnTab: true
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                    personMenu.openFor(userBlock, "above", false)
                    event.accepted = true
                }
            }
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("%1, menu").arg(userBlock.hasUser ? root.player.userName : root.player.deviceName)
            Accessible.onPressAction: personMenu.openFor(userBlock, "above", false)
            UzFocusRing { visible: userBlock.activeFocus; inset: true }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: personMenu.openFor(userBlock, "above", false) }
        }
    }
}
