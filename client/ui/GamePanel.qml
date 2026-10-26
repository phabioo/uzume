import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Side panel of the game view (boards 3g-1, 3r): 320 wide on `ground`, one `rule` line at the left, run tabs on top (Session, the
// plugin tabs, the Plugin console in Developer mode), collapsible to a 48 px rail. The Session tab of your game: Sharing, Save and
// Quit as numbered sections and the hint row (F3, F4) at the bottom. In the Multiview the panel follows the selected tile: your tile =
// the Session tab with a tile header, a remote tile = who, connection, "Audio here", "Remove from multiview". Diagnostics are not a tab
// any more: F3 opens the block on the stage (GameScreen).
Rectangle {
    id: root
    required property PlayerController player
    readonly property SessionController ctl: player.sessions
    readonly property SaveHistoryController saves: player.saveHistory
    readonly property GameSession gameSession: player.gameSession
    property bool collapsed: false
    property bool multi: false
    property string tile: "local"
    readonly property bool localTile: tile === "local"
    readonly property int tileNo: ctl.surfaceOrder.indexOf(tile) + 1
    readonly property var tileInfo: ctl.surfaceInfo[tile] || ({})
    readonly property var tileLink: ctl.surfaceLinks[tile] || null
    readonly property string hubWord: qsTr("this server")
    readonly property bool quitting: player.quitting
    readonly property string shareText: ctl.shared ? qsTr("Shared · %1 watching").arg(ctl.viewerCount) : qsTr("Not shared")
    // "Manage saves" opens the saves view (SavesView, the one of the Library) inside the panel; the game keeps running.
    property bool savesOpen: false
    // Tabs of the panel: "game" | "plugin:<pluginId>" (panel tab of a plugin, in install order) | "console" (Plugin console,
    // Developer mode only, last tab).
    property string panelTab: "game"
    readonly property PluginsController plugins: player.plugins
    readonly property var pluginTabs: plugins.panelTabs
    readonly property bool consoleTab: panelTab === "console" && root.localTile && root.player.plugins.consoleAvailable
    readonly property var pluginTab: {
        if (panelTab.indexOf("plugin:") !== 0) return null
        var id = panelTab.substring(7), t = pluginTabs
        for (var i = 0; i < t.length; ++i) if (t[i].pluginId === id) return t[i]
        return null
    }
    readonly property bool pluginTabOpen: pluginTab !== null && root.localTile
    // Tab model: Session, the plugin tabs, the Plugin console.
    readonly property var tabEntries: {
        var out = [{ id: "game", kind: "game", name: root.localTile ? qsTr("Session") : qsTr("Tile"), icon: "", dot: "", origin: "official", mark: "" }]
        if (!root.localTile) return out   // a remote tile has no plugin tabs
        var t = pluginTabs
        for (var i = 0; i < t.length; ++i)
            out.push({ id: "plugin:" + t[i].pluginId, kind: "plugin", name: t[i].title || t[i].name, pluginName: t[i].name || t[i].title,
                       icon: t[i].icon || "", dot: t[i].dot || "", origin: t[i].origin || "local", mark: "" })
        if (root.plugins.consoleAvailable)
            out.push({ id: "console", kind: "console", name: qsTr("Plugin console"), icon: "", dot: "", origin: "official", mark: ">_" })
        return out
    }
    // Fitting: every tab shows a short label (a plugin tab its icon in front of it; a label never grows past `tabLabelMax`). Session
    // and the open tab always stay; when the rest does not fit, the last tabs move into the "More" tab menu. In a very wide font
    // the labels of Session and the open tab elide to a share of the bar.
    readonly property int tabGap: 18
    readonly property int tabLabelMax: 96
    readonly property int tabIconSize: 16
    readonly property var tabPlan: {
        var all = tabEntries, n = all.length, avail = tabBar.width - 2 * Theme.space3, gapW = root.tabGap
        function labelW(e) { return Math.min(root.tabLabelMax, Math.ceil(tabFm.advanceWidth(e.name))) + (e.dot !== "" ? 10 : 0) }
        function iconW(e) { return e.kind === "plugin" ? root.tabIconSize + 6 : 0 }
        // The "More" tab measured with the real font, so a wide font cannot push it out of the bar.
        function overflowW(count) { return count > 0 ? Math.ceil(tabFm.advanceWidth(qsTr("More") + " ▾")) + gapW : 0 }
        var shown = []
        for (var i = 0; i < n; ++i) shown.push({ e: all[i], label: true })
        function total() {
            var w = (shown.length - 1) * gapW
            for (var j = 0; j < shown.length; ++j) w += labelW(shown[j].e) + iconW(shown[j].e)
            return w
        }
        var overflow = []
        while (total() + overflowW(overflow.length) > avail && shown.length > 1) {
            var idx = -1
            for (var m = shown.length - 1; m > 0; --m) if (shown[m].e.id !== root.panelTab) { idx = m; break }
            if (idx < 0) break
            overflow.unshift(shown.splice(idx, 1)[0].e)
        }
        var textSum = 0
        for (var q = 0; q < shown.length; ++q) textSum += Math.min(root.tabLabelMax, Math.ceil(tabFm.advanceWidth(shown[q].e.name)))
        var fixedW = total() - textSum - (shown.reduce(function (a, s) { return a + (s.e.dot !== "" ? 10 : 0) }, 0)) + overflowW(overflow.length)
        var textMax = shown.length > 0 && fixedW + textSum > avail ? Math.max(16, Math.floor((avail - fixedW) / shown.length)) : root.tabLabelMax
        return { shown: shown, overflow: overflow, textMax: textMax }
    }
    FontMetrics { id: tabFm; font.family: Theme.sans; font.pixelSize: 13 }
    function openTab(id) {
        root.closeSaves()
        root.panelTab = id
        if (root.collapsed) root.collapseToggled()
    }
    // Alt+1 ... Alt+9: tab n of the tab bar (Session, the plugin tabs, the Plugin console); false = no such tab.
    function openTabNumber(n) {
        var t = root.tabEntries
        if (n < 1 || n > t.length || !root.localTile) return false
        root.openTab(t[n - 1].id)
        return true
    }
    function originMarker(origin) { return root.plugins.originMarker(origin) }
    signal collapseToggled()
    signal savesClosed()
    signal selectLocalRequested()
    // The "+n ▾" popover lives in the game screen (it has to open over the play area, not over the sharing section).
    readonly property Item overflowAnchor: overflowTab
    property bool overflowOpen: false
    signal overflowRequested()

    function closeSaves() {
        if (!root.savesOpen) return
        root.savesOpen = false
        root.savesClosed()
    }
    Connections {
        target: root.player.plugins
        function onConsoleAvailableChanged() { if (!root.player.plugins.consoleAvailable && root.panelTab === "console") root.panelTab = "game" }
        function onPanelTabsChanged() { if (root.panelTab.indexOf("plugin:") === 0 && root.pluginTab === null) root.panelTab = "game" }
    }
    // Gone with the game, or when another tile is selected.
    onLocalTileChanged: if (!localTile) root.savesOpen = false
    Connections {
        target: root.gameSession
        function onStateChanged() { if (!root.gameSession.active) root.savesOpen = false }
    }

    objectName: "gamePanel"
    color: Theme.ground
    implicitWidth: collapsed ? Theme.row48 : Theme.sidePanel
    clip: true

    Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.rule }

    function slotLabel() {
        var opts = saves.slotOptions
        for (var i = 0; i < opts.length; ++i) {
            if (opts[i].value === saves.slot) return opts[i].label
        }
        return saves.slot === "default" ? qsTr("default") : saves.slot
    }
    // " · 1 min" behind "Paused by <name>"; refreshed by the minute timer below
    property int nowTick: 0
    Timer { interval: 30000; repeat: true; running: root.tileInfo.paused === true; onTriggered: root.nowTick++ }
    function pausedFor(atMs) {
        root.nowTick   // binding dependency
        if (!atMs) return ""
        var mins = Math.floor((Date.now() - atMs) / 60000)
        return mins < 1 ? "" : " · " + qsTr("%1 min").arg(mins)
    }
    function sinceText(iso) {
        var t = Date.parse(iso)
        if (isNaN(t)) return ""
        var mins = Math.max(0, Math.floor((Date.now() - t) / 60000))
        return mins < 1 ? qsTr("just started") : qsTr("for %1 min").arg(mins)
    }

    // ---- collapsed rail ----
    Column {
        objectName: "panelRail"
        visible: root.collapsed
        anchors.top: parent.top
        anchors.topMargin: Theme.space15
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: Theme.space2
        UzIconButton {
            objectName: "panelExpandButton"
            anchors.horizontalCenter: parent.horizontalCenter
            focusPolicy: Qt.TabFocus
            glyph: "left"
            glyphSize: 14
            Accessible.name: qsTr("Show panel")
            ToolTip.visible: hovered
            ToolTip.delay: 500
            ToolTip.text: qsTr("Show panel")
            onClicked: root.collapseToggled()
        }
        Rectangle {
            visible: root.multi
            anchors.horizontalCenter: parent.horizontalCenter
            width: 20; height: 20
            color: Theme.playerColor(root.tileNo, Theme.dark)
            UzMono { anchors.centerIn: parent; text: String(root.tileNo); font.pixelSize: 11; color: Theme.onPerson }
        }
        Rectangle {
            id: railDot
            objectName: "panelRailShareDot"
            anchors.horizontalCenter: parent.horizontalCenter
            width: 8; height: 8
            color: root.ctl.shared ? Theme.ok : "transparent"
            border.width: root.ctl.shared ? 0 : 1
            border.color: Theme.dim
            HoverHandler { id: dotHover }
            ToolTip.visible: dotHover.hovered
            ToolTip.text: root.shareText
        }
        // One icon per plugin tab; a click opens the panel on that tab
        Repeater {
            model: root.localTile ? root.pluginTabs : []
            delegate: PluginTabIcon {
                id: railTab
                required property var modelData
                objectName: "panelRailTab_" + modelData.pluginId
                anchors.horizontalCenter: parent.horizontalCenter
                size: 22
                chipRadius: 0
                name: modelData.name || modelData.title
                icon: modelData.icon || ""
                dot: modelData.dot || ""
                activeFocusOnTab: true
                Accessible.ignored: false
                Accessible.role: Accessible.Button
                Accessible.name: modelData.title || modelData.name
                Accessible.onPressAction: root.openTab("plugin:" + railTab.modelData.pluginId)
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                        root.openTab("plugin:" + railTab.modelData.pluginId)
                        event.accepted = true
                    }
                }
                UzFocusRing { visible: railTab.activeFocus; inset: true }
                HoverHandler { id: railHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.openTab("plugin:" + railTab.modelData.pluginId) }
                ToolTip.visible: railHover.hovered
                ToolTip.delay: 400
                ToolTip.text: (modelData.title || modelData.name) + (root.originMarker(modelData.origin || "local") !== "" ? " · " + root.originMarker(modelData.origin || "local") : "")
            }
        }
    }

    ColumnLayout {
        visible: !root.collapsed
        anchors.fill: parent
        anchors.leftMargin: 1
        spacing: 0

        // Tile header (Multiview): person square, "TILE 2 · YOUR GAME"
        Item {
            objectName: "panelTileHeader"
            Layout.fillWidth: true
            visible: root.multi
            implicitHeight: Theme.row48
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space3
                anchors.rightMargin: Theme.space2
                spacing: Theme.space1
                Rectangle {
                    objectName: "panelTileBadge"
                    Layout.preferredWidth: 20; Layout.preferredHeight: 20
                    color: Theme.playerColor(root.tileNo, Theme.dark)
                    UzMono { anchors.centerIn: parent; text: String(root.tileNo); font.pixelSize: 11; color: Theme.onPerson }
                }
                UzMono {
                    objectName: "panelTileLabel"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    text: root.localTile ? qsTr("TILE %1 · YOUR GAME").arg(root.tileNo) : qsTr("TILE %1 · WATCHING").arg(root.tileNo)
                    font.pixelSize: 10
                    font.letterSpacing: 1.4
                    color: Theme.dim
                }
            }
        }

        // Tab bar (run tabs): Session, the plugin tabs, the Plugin console. A remote tile has no tabs.
        Item {
            id: tabBar
            objectName: "panelTabBar"
            Layout.fillWidth: true
            visible: !root.savesOpen && root.localTile
            implicitHeight: Theme.row48
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
            Row {
                anchors.fill: parent
                anchors.leftMargin: Theme.space3
                anchors.rightMargin: Theme.space3
                spacing: root.tabGap
                Repeater {
                    model: root.tabPlan.shown
                    delegate: Item {
                        id: tabItem
                        required property var modelData
                        readonly property var entry: modelData.e
                        readonly property bool open: root.panelTab === entry.id
                        readonly property string marker: root.originMarker(entry.origin)
                        objectName: "panelTab_" + entry.id
                        height: Theme.row48
                        width: tabRow.implicitWidth
                        activeFocusOnTab: true
                        Accessible.role: Accessible.PageTab
                        Accessible.name: entry.name
                        Accessible.description: tabItem.marker
                        Accessible.selected: open
                        Keys.onPressed: (event) => {
                            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) { root.panelTab = tabItem.entry.id; event.accepted = true }
                        }
                        Row {
                            id: tabRow
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            PluginTabIcon {
                                objectName: "panelTabIcon"
                                visible: tabItem.entry.kind === "plugin"
                                anchors.verticalCenter: parent.verticalCenter
                                size: root.tabIconSize
                                chipRadius: 0
                                name: tabItem.entry.pluginName || tabItem.entry.name
                                icon: tabItem.entry.icon
                            }
                            Text {
                                objectName: "panelTabLabel"
                                text: tabItem.entry.name
                                textFormat: Text.PlainText
                                width: Math.min(implicitWidth, root.tabPlan.textMax)
                                elide: Text.ElideRight
                                font.family: Theme.sans
                                font.pixelSize: 13
                                font.weight: tabItem.open ? Font.Medium : Font.Normal
                                color: tabItem.open || tabHover.hovered ? Theme.ink : Theme.muted
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Rectangle {   // news / paused marker of a plugin tab
                                objectName: "tabDot"
                                visible: tabItem.entry.dot !== ""
                                anchors.verticalCenter: parent.verticalCenter
                                width: 4; height: 4
                                color: Theme.accent
                            }
                        }
                        Rectangle { visible: tabItem.open; anchors.bottom: parent.bottom; width: parent.width; height: 2; color: Theme.accent }
                        UzFocusRing { visible: tabItem.activeFocus }
                        TapHandler { onTapped: root.panelTab = tabItem.entry.id }
                        HoverHandler { id: tabHover; cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: tabHover.hovered
                        ToolTip.delay: 400
                        ToolTip.text: tabItem.entry.name + (tabItem.marker !== "" ? " · " + tabItem.marker : "")
                    }
                }
                Item {
                    id: overflowTab
                    objectName: "panelTabOverflow"
                    visible: root.tabPlan.overflow.length > 0
                    height: Theme.row48
                    width: overflowLabel.implicitWidth
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("More tabs")
                    ToolTip.visible: overflowHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: qsTr("%n more tab(s)", "", root.tabPlan.overflow.length)
                    Accessible.onPressAction: root.overflowRequested()
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                            root.overflowRequested()
                            event.accepted = true
                        }
                    }
                    UzFocusRing { visible: overflowTab.activeFocus; inset: true }
                    readonly property bool hasNews: {
                        var o = root.tabPlan.overflow
                        for (var i = 0; i < o.length; ++i) if (o[i].dot !== "") return true
                        return false
                    }
                    Text {
                        id: overflowLabel
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("More") + " ▾"
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: 13
                        color: root.overflowOpen || overflowHover.hovered ? Theme.ink : Theme.muted
                    }
                    Rectangle { visible: overflowTab.hasNews; x: parent.width + 2; y: 14; width: 4; height: 4; color: Theme.accent }
                    TapHandler { onTapped: root.overflowRequested() }
                    HoverHandler { id: overflowHover; cursorShape: Qt.PointingHandCursor }
                }
            }
        }

        // ---- plugin tab ----
        Item {
            id: pluginBox
            objectName: "gamePanelPlugin"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            visible: root.pluginTabOpen && !root.savesOpen
            clip: true
            Loader {
                anchors.fill: parent
                active: pluginBox.visible
                sourceComponent: PluginPanelContent { player: root.player; tab: root.pluginTab }
            }
        }

        // ---- Plugin console tab ----
        Item {
            id: consoleBox
            objectName: "gamePanelConsole"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            visible: root.consoleTab && !root.savesOpen
            clip: true
            Loader {
                anchors.fill: parent
                active: consoleBox.visible
                sourceComponent: PluginConsole { player: root.player }
            }
        }

        // ---- your game: Session tab ----
        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.localTile && !root.savesOpen && !root.consoleTab && !root.pluginTabOpen
            contentWidth: width
            contentHeight: col.implicitHeight + 2 * Theme.space3
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            // Plain Columns whose children take exactly the block width: a layout would size its cell to the widest
            // minimum of any child, so one item that cannot shrink (wide platform fonts) would push the panel's buttons
            // out (seen with Qt 6.8 on Windows).
            Column {
                id: col
                x: Theme.space3
                y: 20
                width: flick.width - 2 * Theme.space3
                spacing: Theme.space2

                // Notices as a quiet line with a Yamabuki / danger start rule
                Item {
                    objectName: "panelHubBanner"
                    width: col.width
                    visible: root.ctl.hubLink !== "online"
                    implicitHeight: bannerText.implicitHeight
                    height: implicitHeight
                    Rectangle { width: 2; height: parent.height; color: Theme.accent }
                    Text {
                        id: bannerText
                        x: Theme.space15
                        width: parent.width - Theme.space15
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        lineHeight: Theme.roles.small.line
                        lineHeightMode: Text.FixedHeight
                        color: Theme.ink2
                        text: root.ctl.shared ? qsTr("Server not reachable · reconnecting. The running Session continues until the server ends it.")
                                              : qsTr("Server not reachable · reconnecting. Sharing needs a server connection.")
                    }
                }
                Item {
                    objectName: "panelMessage"
                    width: col.width
                    visible: root.ctl.message !== ""
                    implicitHeight: msgText.implicitHeight
                    height: implicitHeight
                    Rectangle { width: 2; height: parent.height; color: root.ctl.messageIsError ? Theme.danger : Theme.rule }
                    Text {
                        id: msgText
                        x: Theme.space15
                        width: parent.width - Theme.space15
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        lineHeight: Theme.roles.small.line
                        lineHeightMode: Text.FixedHeight
                        color: root.ctl.messageIsError ? Theme.danger : Theme.ink2
                        text: root.ctl.message
                    }
                }

                // Audio of this tile (Multiview; your tile has no remote-style panel, so the control lives here)
                UzButton {
                    objectName: "audioButton_local"
                    width: Math.min(implicitWidth, parent.width)
                    visible: root.multi && root.ctl.hasLocalGame
                    size: "sm"
                    focusPolicy: Qt.NoFocus
                    on: root.ctl.audioFocus === "local"
                    enabled: !on
                    text: on ? qsTr("Audio plays from this tile") : qsTr("Audio here")
                    onClicked: root.ctl.audioHere("local")
                }

                // 01 SHARING
                Column {
                    id: shareBlock
                    objectName: "sharingBlock"
                    width: col.width
                    spacing: Theme.space15
                    UzSectionLabel {
                        objectName: "panelShareState"
                        width: parent.width
                        number: 1
                        keepNumber: true
                        text: qsTr("Sharing")
                        meta: root.player.hubName
                    }
                    Column {
                        width: parent.width
                        spacing: Theme.space05
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            text: qsTr("Who can watch")
                            textFormat: Text.PlainText
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.fieldLabel.size
                            font.weight: Theme.roles.fieldLabel.weight
                            color: Theme.ink2
                        }
                        UzSegmented {
                            objectName: "visibilitySegment"
                            width: parent.width
                            stretch: true
                            current: root.ctl.visibility
                            options: [
                                { value: "private", label: qsTr("Only me"), name: "visPrivate" },
                                { value: "hub_users", label: qsTr("On the server"), name: "visHubUsers" },
                                { value: "invite_only", label: qsTr("Invite only"), name: "visInviteOnly" }
                            ]
                            onPicked: (v) => root.ctl.setVisibility(v)
                        }
                    }
                    Text {
                        objectName: "visibilityHint"
                        width: parent.width
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        lineHeight: Theme.roles.small.line
                        lineHeightMode: Text.FixedHeight
                        color: Theme.muted
                        text: {
                            var v = root.ctl.visibility
                            if (root.ctl.shared)
                                return v === "invite_only" ? qsTr("Only invited users can watch. They send no input and cannot invite others.")
                                                           : qsTr("Everyone on %1 can watch. Viewers send no input.").arg(root.hubWord)
                            if (v === "private") return qsTr("Only you can see this game. Choose On the server or Invite only, then share.")
                            return v === "hub_users" ? qsTr("Not shared yet. Share to let everyone here watch.") : qsTr("Not shared yet. Share to let invited users watch.")
                        }
                    }

                    // Watchers and invited users (only while shared)
                    Column {
                        id: partBlock
                        objectName: "participantsBlock"
                        width: parent.width
                        visible: root.ctl.shared
                        spacing: Theme.space1
                        UzMono {
                            objectName: "participantsTitle"
                            width: parent.width
                            elide: Text.ElideRight
                            text: root.ctl.participantsTitle
                            font.pixelSize: 10
                            font.letterSpacing: 1.4
                            color: Theme.dim
                            visible: root.ctl.participants.length > 0
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            visible: root.ctl.participants.length === 0 && root.ctl.visibility === "invite_only"
                            text: qsTr("Nobody invited yet.")
                            textFormat: Text.PlainText
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.small.size
                            color: Theme.muted
                        }
                        Column {
                            id: plist
                            width: parent.width
                            visible: root.ctl.participants.length > 0
                            Repeater {
                                model: root.ctl.participants
                                // Anchored row: the action is pinned to the right edge, the text takes the rest and
                                // elides, so the action cannot leave the panel.
                                delegate: Item {
                                    id: prow
                                    required property var modelData
                                    required property int index
                                    objectName: "participant_" + modelData.id
                                    width: plist.width
                                    implicitHeight: Theme.row44
                                    height: Theme.row44
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                                    UzAvatar {
                                        id: pav
                                        anchors.left: parent.left
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: prow.modelData.name || ""
                                        person: prow.index + 2
                                        size: 28
                                        status: prow.modelData.online ? "online" : "offline"
                                    }
                                    UzButton {
                                        id: pbtn
                                        objectName: (prow.modelData.kind === "viewer" ? "remove_" : "withdraw_") + prow.modelData.id
                                        kind: "link"
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        hPadding: 8
                                        font.pixelSize: Theme.roles.small.size
                                        focusPolicy: Qt.NoFocus
                                        text: prow.modelData.action
                                        onClicked: prow.modelData.kind === "viewer" ? root.ctl.removeViewer(prow.modelData.id)
                                                                                   : root.ctl.withdrawInvite(prow.modelData.id)
                                    }
                                    UzTag {
                                        id: ppill
                                        objectName: "participantPill"
                                        anchors.right: pbtn.left
                                        anchors.rightMargin: Theme.space05
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: prow.modelData.kind === "viewer" && !!prow.modelData.connection
                                        small: true
                                        text: prow.modelData.connection || ""
                                        tone: prow.modelData.connectionTone || "neutral"
                                    }
                                    ColumnLayout {
                                        anchors.left: pav.right
                                        anchors.leftMargin: Theme.space15
                                        anchors.right: ppill.visible ? ppill.left : pbtn.left
                                        anchors.rightMargin: Theme.space1
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 0
                                        UzLabel { Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight; text: prow.modelData.name; font.pixelSize: Theme.roles.itemTitleSm.size; font.weight: Theme.roles.itemTitleSm.weight; color: Theme.ink }
                                        UzLabel { objectName: "participantStatus"; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight; text: prow.modelData.status; font.pixelSize: Theme.roles.small.size; color: Theme.muted }
                                    }
                                }
                            }
                        }
                        // Relay hint
                        Item {
                            objectName: "relayHint"
                            width: parent.width
                            visible: root.ctl.relayHint !== ""
                            implicitHeight: relayText.implicitHeight
                            height: implicitHeight
                            Rectangle { width: 2; height: parent.height; color: Theme.rule }
                            Text {
                                id: relayText
                                objectName: "relayHintText"
                                x: Theme.space15
                                width: parent.width - Theme.space15
                                text: root.ctl.relayHint
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                font.family: Theme.sans
                                font.pixelSize: Theme.roles.small.size
                                lineHeight: Theme.roles.small.line
                                lineHeightMode: Text.FixedHeight
                                color: Theme.ink2
                            }
                        }
                    }

                    // Invite a server user (Invite only, shared)
                    Column {
                        id: invBlock
                        objectName: "inviteBlock"
                        width: parent.width
                        visible: root.ctl.shared && root.ctl.visibility === "invite_only"
                        spacing: Theme.space1
                        UzField {
                            id: inviteField
                            objectName: "inviteField"
                            width: parent.width
                            implicitHeight: Theme.row32
                            placeholderText: qsTr("Invite someone…")
                            onTextChanged: root.ctl.searchUsers(text)
                        }
                        Text {
                            objectName: "userSearchHint"
                            width: parent.width
                            visible: root.ctl.userSearchHint.length > 0
                            wrapMode: Text.WordWrap
                            text: root.ctl.userSearchHint
                            textFormat: Text.PlainText
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.small.size
                            color: Theme.muted
                        }
                        Repeater {
                            model: root.ctl.userResults
                            delegate: RowLayout {
                                id: urow
                                required property var modelData
                                objectName: "userResult_" + modelData.id
                                width: invBlock.width
                                spacing: Theme.space15
                                Rectangle {
                                    implicitWidth: 8; implicitHeight: 8
                                    color: urow.modelData.online ? Theme.ok : "transparent"
                                    border.width: urow.modelData.online ? 0 : 1
                                    border.color: Theme.dim
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.preferredWidth: 0
                                    spacing: 0
                                    UzLabel { Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight; text: urow.modelData.name; font.pixelSize: Theme.roles.itemTitleSm.size; color: Theme.ink }
                                    UzLabel {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        text: urow.modelData.hint
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: Theme.roles.small.size
                                        color: Theme.muted
                                    }
                                }
                                UzButton {
                                    objectName: "invite_" + urow.modelData.id
                                    size: "sm"
                                    kind: "primary"
                                    focusPolicy: Qt.NoFocus
                                    text: qsTr("Invite")
                                    onClicked: { root.ctl.invite(urow.modelData.id); inviteField.text = "" }
                                }
                            }
                        }
                    }

                    Flow {
                        width: parent.width
                        spacing: Theme.space1
                        UzButton {
                            objectName: "shareButton"
                            visible: !root.ctl.shared
                            size: "sm"
                            focusPolicy: Qt.NoFocus
                            kind: "primary"
                            text: root.ctl.shareBusy ? qsTr("Sharing…") : qsTr("Share Session")
                            enabled: !root.ctl.shareBusy && root.ctl.available && root.ctl.hubLink === "online"
                            onClicked: root.ctl.shareSession()
                        }
                        UzButton {
                            objectName: "shareButton"
                            visible: root.ctl.shared
                            size: "sm"
                            kind: "ghost"
                            focusPolicy: Qt.NoFocus
                            text: qsTr("Stop sharing")
                            onClicked: root.ctl.stopSharing()
                        }
                    }
                }

                // 02 SAVE
                Column {
                    id: saveBlock
                    objectName: "saveSlotBlock"
                    width: col.width
                    visible: root.gameSession.active && root.saves.slotsAvailable
                    spacing: Theme.space15
                    UzSectionLabel {
                        width: parent.width
                        number: 2
                        keepNumber: true
                        text: qsTr("Save")
                        meta: root.slotLabel() + (root.saves.history.length > 0 ? " · " + root.saves.history[0].versionText : "")
                    }
                    Row {
                        spacing: Theme.space1
                        UzTag { objectName: "saveStateTag"; tone: "ok"; text: qsTr("Saved") }
                    }
                    Text {
                        objectName: "gameSnapshotMessage"
                        width: parent.width
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        lineHeight: Theme.roles.small.line
                        lineHeightMode: Text.FixedHeight
                        text: root.saves.message !== "" ? root.saves.message
                              : qsTr("Saved to “%1” · final sync on pause or quit").arg(root.slotLabel())
                        color: root.saves.message === "" ? Theme.muted : (root.saves.messageIsError ? Theme.danger : Theme.ok)
                    }
                    Flow {
                        width: parent.width
                        spacing: Theme.space1
                        UzButton {
                            objectName: "gameSnapshotButton"
                            size: "sm"
                            focusPolicy: Qt.NoFocus
                            text: root.saves.busy ? qsTr("Saving snapshot…") : qsTr("Save snapshot")
                            enabled: root.saves.available && !root.saves.busy
                            onClicked: root.saves.createSnapshotInGame("")
                        }
                        UzKbd {
                            objectName: "snapshotKey"
                            visible: root.player.controllers.hotkeyHints.snapshot !== ""
                            height: Theme.row32
                            keys: root.player.controllers.hotkeyHints.snapshot
                        }
                        UzButton {
                            objectName: "manageSavesButton"
                            size: "sm"
                            kind: "ghost"
                            focusPolicy: Qt.NoFocus
                            text: qsTr("Manage saves")
                            onClicked: root.savesOpen = true
                        }
                    }
                }
            }
        }

        // ---- saves view (Manage saves) ----
        Item {
            id: savesBox
            objectName: "gamePanelSaves"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            visible: root.localTile && root.savesOpen
            clip: true
            Loader {
                anchors.fill: parent
                anchors.topMargin: Theme.space2
                anchors.leftMargin: Theme.space3
                anchors.rightMargin: Theme.space3
                anchors.bottomMargin: Theme.space1
                active: savesBox.visible
                sourceComponent: SavesView {
                    player: root.player
                    inGame: true
                    backTitle: qsTr("Game panel")
                    onBack: root.closeSaves()
                    Component.onCompleted: forceActiveFocus()
                }
            }
        }

        // Quit stays reachable on the Session tab and in the saves view: a footer under the scrolling page.
        Item {
            id: quitFooter
            Layout.fillWidth: true
            visible: root.localTile && !root.consoleTab && !root.pluginTabOpen
            implicitHeight: quitBlock.implicitHeight + 2 * Theme.space2
            Rectangle { width: parent.width; height: 1; color: Theme.rule }
            // 03 QUIT
            Column {
                id: quitBlock
                objectName: "quitBlock"
                x: Theme.space3
                y: Theme.space2
                width: parent.width - 2 * Theme.space3
                spacing: Theme.space15
                UzSectionLabel { width: parent.width; keepNumber: true; number: root.saves.slotsAvailable && root.gameSession.active ? 3 : 2; text: qsTr("Quit") }
                Flow {
                    width: parent.width
                    spacing: Theme.space1
                    UzButton {
                        id: endGameButton
                        objectName: "endGameButton"
                        visible: !root.quitting
                        size: "sm"
                        focusPolicy: Qt.NoFocus
                        text: qsTr("Quit game")
                        onClicked: root.player.requestQuit()
                    }
                    Text {
                        objectName: "quitNote"
                        visible: !root.quitting
                        height: Theme.row32
                        verticalAlignment: Text.AlignVCenter
                        width: Math.min(implicitWidth, quitBlock.width)
                        elide: Text.ElideRight
                        text: qsTr("Saves and syncs first.")
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.muted
                    }
                }
                Column {
                    objectName: "quittingBox"
                    visible: root.quitting
                    width: parent.width
                    spacing: Theme.space05
                    Row {
                        spacing: Theme.space1
                        UzSpinner { size: 14; color: Theme.accent; anchors.verticalCenter: parent.verticalCenter }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Saving and syncing…")
                            textFormat: Text.PlainText
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.itemTitleSm.size
                            font.weight: Theme.roles.itemTitleSm.weight
                            color: Theme.ink
                        }
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: qsTr("Returns to the Library when done.")
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.muted
                    }
                }
            }
        }

        // Hint row (F3, F4): the keys that exist; shown with the Session tab
        UzHintRow {
            objectName: "panelHintRow"
            Layout.fillWidth: true
            visible: flick.visible
            hints: {
                var h = []
                var d = root.player.controllers.hotkeyHints.diagnostics
                if (d) h.push({ kbd: d, label: qsTr("Diagnostics") })
                if (root.plugins.hudWidgets.length > 0) h.push({ kbd: "F4", label: qsTr("Plugin HUD") })
                return h
            }
        }

        // ---- remote tile ----
        Item {
            id: remoteBox
            objectName: "remotePanel"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.localTile
            Column {
                x: Theme.space3
                y: 20
                width: parent.width - 2 * Theme.space3
                spacing: Theme.space2
                RowLayout {
                    width: parent.width
                    spacing: Theme.space15
                    UzAvatar {
                        name: root.tileInfo.who || ""
                        person: root.tileNo
                        size: 40
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 0
                        spacing: 0
                        UzLabel { objectName: "remoteWho"; Layout.fillWidth: true; elide: Text.ElideRight; text: root.tileInfo.who || ""; font.pixelSize: Theme.roles.headerTitle.size; font.weight: Theme.roles.headerTitle.weight; color: Theme.ink }
                        UzLabel { objectName: "remoteGame"; Layout.fillWidth: true; elide: Text.ElideRight; text: root.tileInfo.game || ""; font.pixelSize: Theme.roles.buttonSm.size; color: Theme.muted }
                    }
                }
                RowLayout {
                    width: parent.width
                    spacing: Theme.space15
                    UzTag {
                        objectName: "remoteLinkPill"
                        visible: root.tileLink !== null
                        small: true
                        text: root.tileLink ? (root.tileLink.text + (root.tileLink.rtt ? " · " + root.tileLink.rtt : "")) : ""
                        tone: root.tileLink ? root.tileLink.tone : "neutral"
                    }
                    UzLabel {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        text: (root.tileInfo.visibility || "") + (root.sinceText(root.tileInfo.since || "") !== "" ? " · " + root.sinceText(root.tileInfo.since || "") : "")
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.muted
                    }
                }
                UzTag {
                    objectName: "remotePausedPill"
                    visible: root.tileInfo.paused === true
                    small: true
                    tone: "neutral"
                    text: qsTr("❚❚ Paused by %1").arg(root.tileInfo.pausedBy || root.tileInfo.who || "") + root.pausedFor(root.tileInfo.pausedAt || 0)
                }
                Flow {
                    width: parent.width
                    spacing: Theme.space1
                    UzButton {
                        objectName: "audioButton_" + root.tile
                        size: "sm"
                        focusPolicy: Qt.NoFocus
                        readonly property bool here: root.ctl.audioFocus === root.tile
                        on: here
                        enabled: !here
                        text: here ? qsTr("Audio plays from this tile") : qsTr("Audio here")
                        onClicked: root.ctl.audioHere(root.tile)
                    }
                }
                UzButton {
                    objectName: "removeButton_" + root.tile
                    size: "sm"
                    kind: "ghost"
                    focusPolicy: Qt.NoFocus
                    text: qsTr("Remove from multiview")
                    onClicked: root.ctl.removeSurface(root.tile)
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    lineHeight: Theme.roles.small.line
                    lineHeightMode: Text.FixedHeight
                    color: Theme.muted
                    text: root.tileInfo.paused === true
                          ? qsTr("You only watch and listen. %1’s picture stays frozen until they resume.").arg(root.tileInfo.who || qsTr("The host"))
                          : qsTr("You only watch and listen. %1 sees you as a viewer.").arg(root.tileInfo.who || qsTr("The host"))
                }
            }
            Item {
                visible: root.ctl.hasLocalGame
                anchors.bottom: parent.bottom
                width: parent.width
                implicitHeight: Theme.row48 + Theme.space1
                height: implicitHeight
                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule2 }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space3
                    anchors.rightMargin: Theme.space3
                    spacing: Theme.space15
                    Rectangle {
                        Layout.preferredWidth: 8; Layout.preferredHeight: 8
                        color: Theme.playerColor(root.ctl.surfaceOrder.indexOf("local") + 1, Theme.dark)
                    }
                    UzLabel {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        text: qsTr("Your game runs in tile %1").arg(root.ctl.surfaceOrder.indexOf("local") + 1)
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.muted
                    }
                    UzButton {
                        objectName: "selectLocalTile"
                        kind: "link"
                        focusPolicy: Qt.NoFocus
                        font.pixelSize: Theme.roles.small.size
                        text: qsTr("Select")
                        onClicked: root.selectLocalRequested()
                    }
                }
            }
        }
    }
}
