import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Game page (usability pass 5): the Library column shows one game instead of the list. "‹ Library" (or Esc, or B on the pad)
// returns to the list with the same row selected. Header: cover tile, mono meta line, title and three actions (Favourite, Start a
// run…, Play) plus a `⋯` menu (Add to collection, Play and share). Tabs:
//   Overview  one problem box (a start failure or what blocks play, with its fix and "Copy details"), "Continue from" (the save
//             slots), "How it plays" (core, screens, controller, speed-up with their source), the person's facts, screenshots,
//             plugin sections and the start checklist while a start runs or after it failed
//   Saves     the saves view (slots, history, upload a save file, admins: all users)
//   Settings  the per-game settings (GameSettingsContent: stored values, or the live ones while the game runs)
//   Manage    the file and core facts, local file actions, and for admins Rename, Delete and the details under the list
// player.gamePageTab asks for a tab (Settings › Emulation › Game overrides opens the Settings tab).
Rectangle {
    id: root
    objectName: "gamePage"
    required property PlayerController player
    signal closeRequested()
    signal deleted(string note)   // an admin deleted this game: the Library closes the page and shows the note
    readonly property var game: player.selectedGame
    readonly property bool hasGame: game.id !== undefined
    // 3c-5: the selected game is the one paused in the background (Resume / Quit game).
    readonly property bool running: game.running === true
    // Per person (0.19, 3c-1): play time, last played, sessions, favourite, collections; supported = false on an older server.
    readonly property var personal: game.personal ?? ({})
    readonly property bool personalShown: root.personal.supported === true
    // "overview" | "saves" | "settings" | "manage"
    property string mode: "overview"
    readonly property SaveHistoryController hist: player.saveHistory
    // A restore, delete or upload confirmation is open, or an upload runs: Play is disabled (decision af).
    readonly property bool confirming: hist.confirmationOpen
    readonly property string gameId: game.id === undefined ? "" : game.id
    readonly property EmulationController emu: player.emulation
    color: "transparent"

    // Variants (0.21, design sheet 5): Randomize… in the header, the Variants section and its dialogs.
    readonly property VariantsController vc: player.variants
    // registryRevision is part of the result on purpose: a bare read whose value is unused is dropped by the compiled (qmlsc) bindings,
    // and the binding would then never re-evaluate when a plugin registers or leaves.
    readonly property string randomizeState: root.vc.registryRevision < 0 || !root.hasGame ? "none" : root.vc.randomizeState(root.gameId)
    // A build of a small game (GB, GBC, GBA) shows no dialog: the Randomize… button reads Building… until it is done (10c-3a).
    readonly property bool smallBuilding: root.vc.building && root.vc.current.gameId === root.gameId && root.vc.smallGame(root.gameId)
    property bool cancelEngineBuild: false
    function openRandomize() { randomizeDialog.openFor(root.gameId, root.game.title || "", root.game.system || "") }
    function watchBuild(id: string) {
        const v = root.vc.variantMap(id)
        if (v.name === undefined || root.vc.smallGame(root.gameId)) return
        const f = root.vc.gameFile(root.gameId)
        buildDialog.watch(id, v.name, root.game.title || "", f.sizeText !== "" ? qsTr("%1 · checked").arg(f.sizeText) : "")
    }
    Connections {
        target: root.vc
        function onNeedsBuild(id: string) { if (root.vc.gameOf(id) === root.gameId) root.watchBuild(id) }
        function onBuildFinished(id: string, ok: bool, error: string) {
            if (ok || error === "cancelled" || root.vc.gameOf(id) !== root.gameId) return
            variantDialogs.showProblem(root.vc.variantMap(id))
        }
    }
    onGameIdChanged: {
        root.vc.setGame(root.gameId)
        root.leaveSaves()
        root.mode = "overview"
        root.reloadInfo()
        if (settingsLoader.item !== null) settingsLoader.item.showGame(root.gameId)
        if (gameId !== "") fadeIn.restart()
        else root.closeRequested()   // the game is gone (deleted, filtered out of a reload)
    }
    // Plugins (0.11 design 4h-2): the game code comes from the cached ROM; the plugins matching the selected game feed the
    // status row and are told about the game (empty = none selected) so they can fill their detail section.
    readonly property var pluginsApi: player.plugins
    readonly property string pluginCode: game.id === undefined ? "" : pluginsApi.gameCode(game.system || "", game.cachePath || "")
    readonly property var gamePlugins: {
        const _dep = pluginsApi.plugins
        return hasGame ? pluginsApi.pluginsForGame(game.system || "", pluginCode) : []
    }
    function syncPluginGame() {
        // game.id, not hasGame: this runs from onGameChanged, before the dependent bindings are re-evaluated.
        if (game.id !== undefined) pluginsApi.setDetailGame(game.system || "", pluginCode, game.sha || "", game.title || "")
        else pluginsApi.setDetailGame("", "", "", "")
    }
    onGameChanged: syncPluginGame()
    onPluginCodeChanged: syncPluginGame()
    Component.onCompleted: {
        root.vc.setGame(root.gameId)
        syncPluginGame()
        root.reloadInfo()
        root.applyTab(root.player.gamePageTab)
        Qt.callLater(() => root.forceActiveFocus())
    }
    Component.onDestruction: pluginsApi.setDetailGame("", "", "", "")
    readonly property var bg: player.backgroundGame
    readonly property string sinceText: !running || !bg.startedMs ? "" :
        qsTr("%1 · paused %2").arg(Qt.formatTime(new Date(bg.startedMs), "HH:mm")).arg(Qt.formatTime(new Date(bg.pausedMs), "HH:mm"))

    // ---- tabs
    readonly property var tabModes: ["overview", "saves", "settings", "manage"]
    readonly property var tabItems: [qsTr("Overview"), qsTr("Saves"), qsTr("Settings"), qsTr("Manage")]
    readonly property string tabActive: tabItems[Math.max(0, tabModes.indexOf(mode))]
    function applyTab(tab: string) {
        const m = root.tabModes.indexOf(tab) >= 0 ? tab : "overview"
        if (m === "saves") root.openSaves(false)
        else { root.leaveSaves(); root.mode = m }
    }
    function pickTab(label: string) { root.applyTab(root.tabModes[Math.max(0, root.tabItems.indexOf(label))]) }
    function openSaves(upload) {
        root.mode = "saves"
        savesLoader.active = true
        if (upload === true) Qt.callLater(() => { if (savesLoader.item !== null) savesLoader.item.openUpload() })
        else Qt.callLater(() => { if (savesLoader.item !== null) savesLoader.item.forceActiveFocus() })
    }
    function leaveSaves() {
        if (savesLoader.item !== null) savesLoader.item.cancelAll()
        if (root.mode === "saves") root.mode = "overview"
    }
    Connections {
        target: root.player
        function onGamePageChanged() { if (root.player.gamePageOpen) root.applyTab(root.player.gamePageTab) }
    }
    NumberAnimation { id: fadeIn; target: body; property: "opacity"; from: 0.35; to: 1; duration: Theme.durPage }

    // ---- the one problem (GameDetail: error, problemText, fixKind) and its fix
    readonly property string error: running ? "" : (game.error || "")
    readonly property string problem: running ? "" : (game.problemText || "")
    readonly property string problemShown: root.error !== "" ? root.error : root.problem
    readonly property string fixKind: running ? "" : (game.fixKind || "")
    readonly property bool admin: player.admin.available
    readonly property string fixText: {
        if (fixKind === "admin") return admin ? qsTr("Open Systems & cores") : qsTr("Ask an admin")
        if (fixKind === "retry") return qsTr("Retry connection")
        return ""
    }
    readonly property string primaryText: {
        if (running) return qsTr("▶ Resume")
        if (fixText !== "") return fixText
        if (mode === "saves" && game.canPlay === true && game.busy !== true && (game.playLabel || "") === qsTr("Play"))
            return qsTr("Play from %1").arg(hist.slotLabel || qsTr("Main"))
        return game.playLabel || qsTr("Play")
    }
    function primaryAction() {
        if (running) root.player.resumeGame()
        else if (fixKind === "admin") {
            if (admin) root.player.showServerPage("systems")
            else askAdminLoader.ask({
                label: qsTr("Library · %1").arg(root.game.title || ""),
                title: qsTr("Ask an admin"),
                text: qsTr("%1 cannot start on this PC: %2 Copy the message and send it to the person who runs your server.").arg(root.game.title || "").arg(root.problem),
                action: qsTr("Copy message"),
                run: (value, finish) => {
                    askAdminLoader.copy(qsTr("%1 cannot start on my PC: %2").arg(root.game.title || "").arg(root.problem))
                    finish(true, "")
                }
            })
        } else if (fixKind === "retry") root.player.retryConnection()
        else root.player.playSelected()
    }
    function problemDetails(): string {
        const lines = [root.game.title || "", root.problemShown]
        for (const s of (root.game.checklist || [])) lines.push("- " + s.label + (s.meta ? " (" + s.meta + ")" : ""))
        return lines.filter(l => l !== "").join("\n")
    }

    // ---- "How it plays": the effective values of this game (EmulationController.gameSettings), with where they come from
    property var info: ({})
    function reloadInfo() { root.info = root.gameId === "" ? ({}) : root.emu.gameSettings(root.gameId) }
    Connections {
        target: root.emu
        function onGameSettingsChanged(id: string) { if (id === "" || id === root.gameId) root.reloadInfo() }
    }
    function infoRow(key: string): var {
        for (const t of (root.info.tabs ?? [])) for (const r of t.rows) if (r.key === key) return r
        return null
    }
    readonly property var playRows: {
        const out = []
        const own = root.infoRow("uzume.core")
        const coreName = (own !== null && own.isSet) ? own.valueLabel : (root.info.coreName ?? "")
        if (coreName !== "") out.push({ name: "core", label: qsTr("Core"), value: coreName, isDefault: own === null || !own.isSet })
        const rows = [{ name: "screens", label: qsTr("Screens"), keys: ["uzume.screen_layout"] },
                      { name: "controller", label: qsTr("Controller"), keys: ["uzume.pad_profile", "uzume.keyboard_profile"] },
                      { name: "speedup", label: qsTr("Speed-up"), keys: ["uzume.speedup_ratio"] }]
        for (const spec of rows) {
            for (const k of spec.keys) {
                const r = root.infoRow(k)
                if (r === null) continue
                out.push({ name: spec.name, label: spec.label, value: r.valueLabel, isDefault: !r.isSet })
                break
            }
        }
        return out
    }

    // The height the page needs for its header, the tabs and a usable body; the Library column scrolls below it.
    readonly property real minPageHeight: tabs.y + tabs.height + 280

    // ---- layout
    ColumnLayout {
        id: body
        anchors.fill: parent
        visible: root.hasGame
        spacing: Theme.space2

        // Back row
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space2
            UzButton {
                objectName: "gameBack"
                Layout.fillWidth: false
                kind: "link"
                text: qsTr("‹ Library")
                Accessible.name: qsTr("Back to the Library")
                onClicked: root.closeRequested()
            }
            Item { Layout.fillWidth: true }
        }

        // Header: cover, meta line, title, the actions
        RowLayout {
            id: headRow
            objectName: "gameHeader"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.space3
            // Cover placeholder: `placeholder` plane, monogram, the system chip at the top left.
            Rectangle {
                implicitWidth: 112
                implicitHeight: 112
                Layout.alignment: Qt.AlignTop
                color: Theme.placeholder
                border.width: 1
                border.color: Theme.placeholderRule
                UzLabel {
                    anchors.centerIn: parent
                    text: root.game.monogram || ""
                    font.pixelSize: 40
                    font.weight: Font.Light
                    color: Theme.ink2
                }
                UzSystemChip {
                    x: -1; y: -1
                    system: (root.game.system || "").toUpperCase()
                    visible: (root.game.system || "") !== ""
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space1
                UzMono {
                    objectName: "detailCaption"
                    visible: root.game.localFile === true && !root.running
                    Layout.fillWidth: true
                    text: root.game.onHub ? qsTr("LOCAL COPY OF A SERVER GAME") : qsTr("LOCAL FILE · NOT ON SERVER")
                    color: Theme.accentText
                    font.pixelSize: Theme.roles.meta11.size
                    wrapMode: Text.WordWrap
                }
                // Meta line: `SYSTEM · 14 H PLAYED · LAST PLAYED YESTERDAY`; on a server without per-person data `SYSTEM · CORE`.
                UzMono {
                    objectName: "detailSystem"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    readonly property string coreLine: (root.game.coreVersionText || "") !== ""
                          ? qsTr("%1 · %2 %3").arg(root.game.systemName || "").arg(root.game.coreLabelText || "").arg(root.game.coreVersionText)
                          : ((root.game.coreLabelText || "") !== "" ? qsTr("%1 · %2").arg(root.game.systemName || "").arg(root.game.coreLabelText)
                                                                      : (root.game.systemName || ""))
                    text: root.personalShown
                          ? [(root.game.system || "").toUpperCase(),
                             ...(root.personal.playTimeText && root.personal.playTimeText !== "–" ? [qsTr("%1 played").arg(root.personal.playTimeText)] : []),
                             root.personal.lastPlayedText && root.personal.lastPlayedText !== "–" ? qsTr("last played %1").arg(root.personal.lastPlayedText) : qsTr("never played")].join(" · ")
                          : coreLine
                    color: Theme.muted
                    font.pixelSize: Theme.roles.meta.size
                    font.letterSpacing: Theme.roles.meta.spacing
                    font.capitalization: Font.AllUppercase
                    elide: Text.ElideRight
                    HoverHandler { id: systemHover }
                    ToolTip.visible: systemHover.hovered && truncated
                    ToolTip.text: text
                    ToolTip.delay: 400
                }
                UzLabel {
                    objectName: "detailTitle"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    wrapMode: Text.Wrap
                    text: root.game.title || ""
                    font.pixelSize: Theme.roles.title.size
                    font.weight: Font.Light
                    font.letterSpacing: Theme.roles.title.spacing
                    lineHeight: Theme.roles.title.line
                    lineHeightMode: Text.FixedHeight
                }
            }
            // Three actions and the `⋯` menu. Running (paused in the background): Quit game and Resume.
            Flow {
                id: actions
                objectName: "gameActions"
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: Math.min(naturalWidth, Math.max(0, headRow.width - 112 - 2 * Theme.space3 - 240))
                Layout.minimumWidth: 0
                spacing: Theme.space1
                layoutDirection: Qt.RightToLeft
                readonly property real naturalWidth: {
                    let w = 0
                    let n = 0
                    for (let i = 0; i < children.length; ++i) {
                        const c = children[i]
                        if (!c.visible || c.implicitWidth === undefined) continue
                        w += c.implicitWidth
                        ++n
                    }
                    return w + Math.max(0, n - 1) * spacing
                }
                // Right to left: the first child sits at the far right.
                UzIconButton {
                    id: moreButton
                    objectName: "detailMoreButton"
                    visible: !root.running && moreMenu.items.length > 0
                    glyph: "more"
                    outlined: true
                    on: moreMenu.opened
                    text: qsTr("More actions")
                    implicitHeight: Theme.row36
                    implicitWidth: Theme.row36
                    Accessible.name: qsTr("More actions for %1").arg(root.game.title || "")
                    onClicked: moreMenu.openFor(moreButton, "", true)
                }
                UzButton {
                    objectName: "playButton"
                    kind: "primary"
                    busy: root.game.busy === true
                    busyOnClick: true
                    text: root.primaryText
                    enabled: root.running ? !root.player.quitting
                             : (root.fixKind === "admin" || root.fixKind === "retry") ? !root.confirming
                             : (root.game.canPlay === true && !root.confirming)
                    onClicked: root.primaryAction()
                }
                UzButton {
                    objectName: "startRunButton"
                    visible: !root.running && root.player.runs.available && root.game.onHub !== false
                    text: qsTr("Start a run…")
                    onClicked: runDialogLoader.openFor(root.gameId)
                }
                // Running: Quit game (the paused game offers it here); not running it is "Play and share" in the `⋯` menu.
                UzButton {
                    objectName: "playShareButton"
                    visible: root.running
                    text: qsTr("Quit game")
                    enabled: !root.player.quitting
                    onClicked: root.player.requestQuit()
                }
                // Randomize… (design 10a-1): shown when a plugin offers variants for this game's system; off, with its reason as the
                // tooltip, when the game itself is not on the plugin's list (10a-3a).
                Item {
                    id: randomizeWrap
                    objectName: "randomizeWrap"
                    visible: !root.running && root.randomizeState !== "none" && root.game.onHub !== false
                    implicitWidth: randomizeButton.implicitWidth
                    implicitHeight: randomizeButton.implicitHeight
                    readonly property string reason: root.randomizeState === "unsupported" ? qsTr("%1 can’t be randomized.").arg(root.game.title || "") : ""
                    HoverHandler { id: randomizeHover }
                    ToolTip.visible: randomizeHover.hovered && randomizeWrap.reason !== ""
                    ToolTip.text: randomizeWrap.reason
                    ToolTip.delay: 400
                    UzButton {
                        id: randomizeButton
                        objectName: "randomizeButton"
                        anchors.fill: parent
                        text: root.smallBuilding ? qsTr("Building…") : qsTr("Randomize…")
                        busy: root.smallBuilding
                        enabled: root.randomizeState === "ok" && !root.confirming && !root.smallBuilding
                        Accessible.description: randomizeWrap.reason
                        onClicked: root.openRandomize()
                    }
                }
                UzButton {
                    id: favoriteButton
                    objectName: "favoriteButton"
                    visible: root.personalShown && !root.running
                    kind: "ghost"
                    on: root.personal.favorite === true
                    text: root.personal.favorite === true ? qsTr("★ Favourite") : qsTr("☆ Favourite")
                    Accessible.checkable: true
                    Accessible.checked: root.personal.favorite === true
                    Accessible.name: qsTr("Favourite")
                    onClicked: root.player.personal.toggleFavorite(root.gameId)
                }
            }
        }
        UzLabel {
            objectName: "quitNote"
            visible: root.running
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            horizontalAlignment: Text.AlignRight
            wrapMode: Text.Wrap
            color: Theme.dim
            font.pixelSize: Theme.roles.small.size
            text: root.player.quitting ? qsTr("Saving and syncing…") : qsTr("Quit saves and syncs to the server first")
        }

        UzTabs {
            id: tabs
            objectName: "detailTabs"
            size: "lg"
            Layout.fillWidth: true
            Layout.topMargin: Theme.space1
            items: root.tabItems
            active: root.tabActive
            onPicked: (label) => root.pickTab(label)
        }

        // ---------------------------------------------------------------- Overview
        Flickable {
            id: overviewFlick
            objectName: "gameOverview"
            visible: root.mode === "overview"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            contentWidth: width
            contentHeight: overview.implicitHeight + Theme.space2
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            ColumnLayout {
                id: overview
                width: overviewFlick.width - 14
                spacing: Theme.space3

                // The one problem: a start failure, else what blocks play; its fix and "Copy details".
                Rectangle {
                    id: problemBox
                    objectName: "detailProblem"
                    visible: root.problemShown !== ""
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    readonly property bool failed: root.error !== ""
                    implicitHeight: problemCol.implicitHeight + 2 * Theme.space15
                    color: failed ? Theme.dangerTint : Theme.selected
                    Rectangle { width: 2; height: parent.height; color: problemBox.failed ? Theme.danger : Theme.accent }
                    ColumnLayout {
                        id: problemCol
                        anchors.fill: parent
                        anchors.margins: Theme.space15
                        anchors.leftMargin: Theme.space2
                        spacing: Theme.space1
                        UzLabel {
                            id: problemText
                            objectName: "playDisabledReason"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredWidth: 1
                            wrapMode: Text.Wrap
                            color: problemBox.failed ? Theme.danger : Theme.ink
                            font.pixelSize: Theme.roles.input.size
                            text: root.problemShown
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            UzButton {
                                objectName: "problemFixButton"
                                visible: root.fixText !== ""
                                Layout.fillWidth: false
                                size: "sm"
                                text: root.fixText
                                enabled: !root.confirming
                                onClicked: root.primaryAction()
                            }
                            UzButton {
                                objectName: "firmwareRecheck"
                                visible: root.game.firmwareBlocked === true
                                Layout.fillWidth: false
                                kind: "link"
                                text: qsTr("Check firmware again")
                                onClicked: root.player.recheckFirmware()
                            }
                            UzButton {
                                objectName: "problemCopyDetails"
                                Layout.fillWidth: false
                                kind: "link"
                                text: qsTr("Copy details")
                                onClicked: root.hist.copyText(root.problemDetails())
                            }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }
                UzTag {
                    objectName: "detailPill"
                    visible: (root.game.pillText || "") !== "" && root.problemShown === ""
                    Layout.minimumWidth: 0
                    Layout.maximumWidth: overview.width
                    tone: root.game.pillTone || "neutral"
                    text: root.game.pillText || ""
                }

                UzLabel {
                    objectName: "runningNote"
                    visible: root.running
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    wrapMode: Text.Wrap
                    lineHeight: 18
                    lineHeightMode: Text.FixedHeight
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                    text: qsTr("The game stays loaded and paused while you browse, change settings or watch someone else’s session.")
                }
                // 01 Continue from | 02 How it plays
                GridLayout {
                    objectName: "gameOverviewColumns"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    columns: overview.width >= 720 ? 2 : 1
                    columnSpacing: Theme.space5
                    rowSpacing: Theme.space3
                    SaveSummary {
                        player: root.player
                        Layout.alignment: Qt.AlignTop
                        onManage: root.openSaves(false)
                        onUploadRequested: root.openSaves(true)
                    }
                    ColumnLayout {
                        objectName: "howItPlays"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        Layout.alignment: Qt.AlignTop
                        spacing: Theme.space1
                        UzSectionLabel { Layout.fillWidth: true; number: 2; text: qsTr("How it plays") }
                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule }
                        Repeater {
                            model: root.playRows
                            delegate: Item {
                                id: playRow
                                required property var modelData
                                objectName: "playRow_" + modelData.name
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                implicitHeight: Theme.row44
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                                RowLayout {
                                    anchors.fill: parent
                                    spacing: Theme.space2
                                    UzMono {
                                        Layout.preferredWidth: 88
                                        Layout.minimumWidth: 0
                                        text: playRow.modelData.label
                                        elide: Text.ElideRight
                                        color: Theme.dim
                                        font.pixelSize: Theme.roles.sectionLabel.size
                                        font.weight: Theme.roles.sectionLabel.weight
                                        font.letterSpacing: Theme.roles.sectionLabel.spacing
                                        font.capitalization: Font.AllUppercase
                                    }
                                    UzLabel {
                                        objectName: "playRowValue_" + playRow.modelData.name
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        elide: Text.ElideRight
                                        font.pixelSize: Theme.roles.input.size
                                        color: Theme.ink
                                        text: playRow.modelData.isDefault ? qsTr("%1 (default)").arg(playRow.modelData.value) : playRow.modelData.value
                                    }
                                    UzButton {
                                        objectName: "playRowChange_" + playRow.modelData.name
                                        kind: "link"
                                        Layout.fillWidth: false
                                        text: qsTr("Change")
                                        Accessible.name: qsTr("Change %1").arg(playRow.modelData.label)
                                        onClicked: root.applyTab("settings")
                                    }
                                }
                            }
                        }
                        UzLabel {
                            objectName: "howItPlaysEmpty"
                            visible: root.playRows.length === 0
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("This game uses your defaults from Emulation.")
                            color: Theme.muted
                            font.pixelSize: Theme.roles.small.size
                        }
                    }
                }

                // Your play (3c-1 facts): play time and last played, sessions, collections, as one compact row of facts.
                Flow {
                    objectName: "personalFacts"
                    visible: root.personalShown
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: Theme.space4
                    Repeater {
                        model: [
                            { label: qsTr("Play time"), text: root.personal.playTimeText ?? "–", name: "factPlayTime" },
                            { label: qsTr("Last played"), text: root.personal.lastPlayedText ?? "–", name: "factLastPlayed" },
                            { label: qsTr("Sessions"), text: root.personal.sessions > 0 ? String(root.personal.sessions) : "–", name: "factSessions" },
                            { label: qsTr("Collections"), text: (root.personal.collectionNames ?? []).length > 0 ? root.personal.collectionNames.join(", ") : "–",
                              name: "factCollections" }
                        ]
                        delegate: Column {
                            id: fact
                            required property var modelData
                            spacing: 2
                            UzMono {
                                text: fact.modelData.label
                                color: Theme.dim
                                font.pixelSize: Theme.roles.sectionLabel.size
                                font.weight: Theme.roles.sectionLabel.weight
                                font.letterSpacing: Theme.roles.sectionLabel.spacing
                                font.capitalization: Font.AllUppercase
                            }
                            UzLabel {
                                objectName: fact.modelData.name
                                width: Math.min(implicitWidth, overview.width)
                                elide: Text.ElideRight
                                text: fact.modelData.text
                                font.pixelSize: Theme.roles.input.size
                                color: Theme.ink2
                            }
                        }
                    }
                }

                VariantsSection {
                    id: variantsSection
                    player: root.player
                    gameId: root.gameId
                    playEnabled: !root.running && root.game.busy !== true && !root.confirming
                    onRandomizeRequested: root.openRandomize()
                    onAskDelete: (v, versions) => variantDialogs.askDelete(v, versions)
                    onAskRename: (v) => variantDialogs.askRename(v)
                    onAskSpoiler: (v) => variantDialogs.askSpoiler(v)
                    onShowProblem: (v) => variantDialogs.showProblem(v)
                    onShowBuild: (v) => root.watchBuild(v.id)
                }

                ScreenshotStrip {
                    placed: root.mode === "overview"
                    player: root.player
                    gameId: root.hasGame ? root.gameId : ""
                    onOpenRequested: (id) => screenshotViewer.openAt(id)
                }

                PluginDetailSection {
                    objectName: "pluginDetailSections"
                    placed: root.mode === "overview"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    player: root.player
                }

                // Start checklist: only while a start runs or after it failed.
                ColumnLayout {
                    objectName: "startChecklist"
                    visible: !root.running && (root.game.busy === true || (root.game.error || "") !== "")
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: Theme.space15
                    Eyebrow { text: qsTr("Start") }
                    Repeater {
                        model: root.game.checklist || []
                        delegate: RowLayout {
                            id: step
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: Theme.space15
                            Item {
                                Layout.alignment: Qt.AlignTop
                                Layout.preferredWidth: 18
                                Layout.preferredHeight: 18
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 10
                                    height: 10
                                    radius: 0
                                    color: step.modelData.state === "done" ? Theme.accent : (step.modelData.state === "error" ? Theme.accentText : "transparent")
                                    border.width: (step.modelData.state === "done" || step.modelData.state === "error") ? 0 : 1.5
                                    border.color: step.modelData.state === "active" ? Theme.accent : Theme.disabledInk
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 2
                                UzLabel {
                                    Layout.fillWidth: true
                                    text: step.modelData.label
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: Theme.roles.input.size
                                    color: step.modelData.state === "pending" ? Theme.dim : Theme.ink
                                }
                                UzMono {
                                    visible: (step.modelData.meta || "") !== ""
                                    Layout.fillWidth: true
                                    text: step.modelData.meta || ""
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: Theme.roles.meta11.size
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---------------------------------------------------------------- Saves
        Loader {
            id: savesLoader
            active: false
            visible: root.mode === "saves"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            sourceComponent: SavesView {
                player: root.player
                onBack: root.closeRequested()
                onStartRunRequested: runDialogLoader.openFor(root.gameId)
            }
        }

        // ---------------------------------------------------------------- Settings
        Flickable {
            id: settingsFlick
            objectName: "gameSettingsPane"
            visible: root.mode === "settings"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            contentWidth: width
            contentHeight: settingsCol.implicitHeight + Theme.space2
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }
            ColumnLayout {
                id: settingsCol
                width: settingsFlick.width - 14
                spacing: Theme.space2
                RowLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: Theme.space2
                    UzLabel {
                        objectName: "gameSettingsIntro"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        wrapMode: Text.WordWrap
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                        text: root.info.changedCount > 0
                              ? qsTr("%n change(s) from your defaults. Everything else follows the system.", "", root.info.changedCount)
                              : qsTr("No changes from your defaults. Only what you change here applies to this game.")
                    }
                    UzButton {
                        objectName: "gamePageResetAll"
                        text: qsTr("Reset all…")
                        enabled: (root.info.changedCount ?? 0) > 0
                        onClicked: resetConfirm.open()
                    }
                }
                Loader {
                    id: settingsLoader
                    active: root.mode === "settings"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    sourceComponent: GameSettingsContent {
                        player: root.player
                        Component.onCompleted: showGame(root.gameId)
                    }
                }
            }
        }

        // ---------------------------------------------------------------- Manage
        Flickable {
            id: manageFlick
            objectName: "gameManage"
            visible: root.mode === "manage"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            contentWidth: width
            contentHeight: manage.implicitHeight + Theme.space2
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            ColumnLayout {
                id: manage
                width: manageFlick.width - 14
                spacing: Theme.space2

                // Status table: the file, size, core and firmware of the game.
                ColumnLayout {
                    objectName: "detailFacts"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 0
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule }
                    Repeater {
                        model: root.running ? [
                            { label: qsTr("Running since"), text: root.sinceText, tone: "neutral", hint: "" },
                            { label: qsTr("Sharing"), text: root.player.sessions.shared ? qsTr("Shared") : qsTr("Not shared"), tone: "neutral", hint: "" },
                            { label: qsTr("Core"), text: root.game.coreText, tone: root.game.coreTone, hint: "" }
                        ] : root.game.localFile === true ? [
                            // A game that starts from a library folder (3c-7): the file, the core and how it relates to the server.
                            { label: qsTr("File"), text: root.game.localPath || "", tone: "neutral", hint: "", objName: "detailLocalPath", mono: true },
                            { label: root.game.onHub ? qsTr("Matches") : qsTr("On the server"),
                              text: root.game.onHub ? qsTr("Server ROM %1").arg(root.game.hubMatchText || "") : qsTr("Not uploaded"), tone: "neutral", hint: "" },
                            { label: qsTr("Core"), text: root.game.coreText, tone: root.game.coreTone, hint: root.game.coreHint }
                        ] : [
                            { label: qsTr("ROM"), text: root.game.romText, tone: root.game.romTone, hint: "" },
                            { label: qsTr("Size"), text: root.game.fileSizeText || "–", tone: "neutral", hint: "" },
                            { label: qsTr("Core"), text: root.game.coreText, tone: root.game.coreTone, hint: root.game.coreHint },
                            { label: qsTr("Firmware"), text: root.game.firmwareText, tone: root.game.firmwareTone, hint: root.game.firmwareHint }
                        ].concat(root.randomizeState === "ok" ? [
                            { label: qsTr("Randomizer"), text: qsTr("Supported"), tone: "neutral", hint: "", objName: "detailRandomizerRow" }
                        ] : root.randomizeState === "unsupported" ? [
                            { label: qsTr("Randomizer"), text: qsTr("Not supported"), tone: "neutral", hint: "", objName: "detailRandomizerRow" }
                        ] : []).concat(root.gamePlugins.length > 0 ? [
                            { label: qsTr("Plugins"), text: qsTr("%1 on · %2").arg(root.gamePlugins.length).arg(root.gamePlugins.map(p => p.name).join(", ")), tone: "neutral", hint: "", objName: "detailPluginsRow" }
                        ] : [])
                        delegate: Item {
                            id: row
                            required property var modelData
                            objectName: modelData.objName || ""
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            implicitHeight: rowCol.height + 2 * Theme.space1
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 1
                                color: Theme.rule2
                            }
                            // Plain anchors and a Column, not nested layouts: the wrapped value used to get a stale height (hundreds of
                            // pixels, from a 1 px wide first layout pass) that the layouts never recomputed (R-P1).
                            Column {
                                id: rowCol
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.topMargin: Theme.space1
                                spacing: Theme.space05
                                Item {
                                    width: parent.width
                                    height: Math.max(rowLabel.implicitHeight, rowValue.implicitHeight)
                                    UzMono {
                                        id: rowLabel
                                        anchors.left: parent.left
                                        width: 96
                                        text: row.modelData.label
                                        elide: Text.ElideRight
                                        color: Theme.dim
                                        font.pixelSize: Theme.roles.sectionLabel.size
                                        font.weight: Theme.roles.sectionLabel.weight
                                        font.letterSpacing: Theme.roles.sectionLabel.spacing
                                        font.capitalization: Font.AllUppercase
                                    }
                                    UzLabel {
                                        id: rowValue
                                        // The value takes the rest of the row and wraps. A file path is cut in the middle instead (the
                                        // full path is the tooltip).
                                        anchors.left: rowLabel.right
                                        anchors.leftMargin: Theme.space15
                                        anchors.right: parent.right
                                        horizontalAlignment: Text.AlignLeft
                                        wrapMode: row.modelData.mono === true ? Text.NoWrap : Text.Wrap
                                        elide: row.modelData.mono === true ? Text.ElideMiddle : Text.ElideNone
                                        HoverHandler { id: valHover }
                                        ToolTip.visible: row.modelData.mono === true && valHover.hovered
                                        ToolTip.text: row.modelData.text || ""
                                        ToolTip.delay: 400
                                        objectName: "detailRowValue"
                                        text: (row.modelData.tone === "ok" ? "✓ " : ((row.modelData.tone === "warn" || row.modelData.tone === "amber") ? "▲ " : (row.modelData.tone === "error" ? "✕ " : "")))
                                              + (row.modelData.text || "")
                                        font.pixelSize: Theme.roles.input.size
                                        color: row.modelData.tone === "neutral" ? Theme.ink2 : Theme.toneColor(row.modelData.tone)
                                    }
                                }
                                UzMono {
                                    visible: (row.modelData.hint || "") !== ""
                                    x: 96 + Theme.space15
                                    width: parent.width - x
                                    text: row.modelData.hint || ""
                                    wrapMode: Text.WrapAnywhere
                                    font.pixelSize: Theme.roles.meta11.size
                                }
                            }
                        }
                    }
                }
                UzLabel {
                    objectName: "detailCoreNotice"
                    visible: (root.game.coreNotice || "") !== ""
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    text: root.game.coreNotice || ""
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                    wrapMode: Text.WordWrap
                }

                // Local-only game (0.12): the file stays on this PC until it is uploaded.
                ColumnLayout {
                    objectName: "localOnlyBox"
                    visible: root.game.localOnly === true && !root.running
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: Theme.space1
                    UzLabel {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        wrapMode: Text.Wrap
                        lineHeight: 18
                        lineHeightMode: Text.FixedHeight
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                        text: root.game.localFile === true
                              ? qsTr("Only you see this game. Its file stays on this device until you upload it to the server.")
                              : qsTr("Only you see this game. Its file is not on this device and not on the server.")
                    }
                    UzLabel {
                        objectName: "localUploadStatus"
                        visible: root.player.upload.gameId === root.game.id && (root.player.upload.active || root.player.upload.message !== "")
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        wrapMode: Text.Wrap
                        font.pixelSize: Theme.roles.small.size
                        color: root.player.upload.isError ? Theme.danger : Theme.ink
                        text: root.player.upload.active
                              ? qsTr("Uploading · %1 %").arg(Math.round(root.player.upload.progress * 100))
                              : root.player.upload.message
                    }
                    Rectangle {
                        objectName: "localUploadProgress"
                        visible: root.player.upload.active && root.player.upload.gameId === root.game.id
                        Layout.fillWidth: true
                        Layout.preferredHeight: 2
                        color: Theme.rule
                        Rectangle {
                            height: parent.height
                            width: parent.width * root.player.upload.progress
                            color: Theme.accent
                        }
                    }
                }

                // The file on this PC: show it, put it on the server, or take it out of the library (the file stays where it is).
                Flow {
                    objectName: "manageLocalActions"
                    visible: !root.running && (root.game.localFile === true || root.game.onHub === false)
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: Theme.space1
                    UzButton {
                        objectName: "showInFolder"
                        visible: root.game.localFile === true
                        size: "sm"
                        text: qsTr("Show in folder")
                        onClicked: root.player.showInFolder(root.game.localPath || "")
                    }
                    UzButton {
                        objectName: "uploadToHubButton"
                        visible: root.game.onHub === false && root.game.localFile === true
                        size: "sm"
                        text: qsTr("Upload to server")
                        enabled: root.player.canUpload && !root.player.upload.active
                        onClicked: root.player.uploadGameToHub(root.game.id)
                    }
                    UzButton {
                        objectName: "removeFromLibrary"
                        visible: root.game.onHub === false
                        size: "sm"
                        kind: "danger"
                        text: qsTr("Remove from library")
                        enabled: !root.player.upload.active
                        onClicked: { root.player.removeFromLibrary(root.game.id); root.closeRequested() }
                    }
                }

                // Admins: uploaded by, file, SHA-256, who has saves; Rename… and Delete game….
                LibraryAdminDetails {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.space2
                    visible: root.player.admin.available && root.mode === "manage"
                    player: root.player
                    onGameRemoved: (note) => root.deleted(note)
                }
            }
        }
    }

    // `⋯`: what a game does besides the three actions.
    UzMenu {
        id: moreMenu
        objectName: "detailMoreMenu"
        items: (root.personalShown ? [{ label: qsTr("Add to collection"), name: "addToCollection" }] : [])
               .concat((root.player.sessions.available && root.game.onHub !== false) ? [{ label: qsTr("Play and share"), name: "playShare",
                                   disabled: !(root.game.canPlay === true && !root.confirming) }] : [])
        onChosen: (item) => {
            if (item.name === "addToCollection") detailCollections.openAdd(root.gameId)
            else if (item.name === "playShare") root.player.playAndShareSelected()
        }
    }
    CollectionDialogs { id: detailCollections; player: root.player }
    // "Reset all…" of the Settings tab asks first: it drops every override of this game.
    UzDialog {
        id: resetConfirm
        objectName: "gamePageResetConfirm"
        parent: Window.contentItem
        z: 110
        label: root.info.title ?? root.game.title ?? ""
        title: qsTr("Reset all settings for this game?")
        text: qsTr("Every row goes back to your default for %1 in Emulation.").arg(root.info.systemLabel ?? "")
        actions: [
            UzButton { objectName: "gamePageResetCancel"; text: qsTr("Cancel"); onClicked: resetConfirm.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "gamePageResetConfirmed"
                kind: "danger"
                text: qsTr("Reset all")
                onClicked: { root.emu.resetGameOptions(root.gameId); resetConfirm.close() }
            }
        ]
    }
    // "Ask an admin": the question dialog is created when it is first asked for.
    Loader {
        id: askAdminLoader
        active: false
        sourceComponent: AdminAskDialog { objectName: "detailAskAdminDialog" }
        function ask(spec) {
            active = true
            if (item !== null) item.ask(spec)
        }
        function copy(text) {
            active = true
            if (item !== null) item.copy(text)
        }
    }
    // "Start a run" with this game chosen: the dialog is created when it is first asked for.
    Loader {
        id: runDialogLoader
        active: false
        sourceComponent: RunStartDialog {
            objectName: "detailRunStartDialog"
            runs: root.player.runs
            serverName: root.player.hubName
            pcOnly: root.player.localHub.onThisPcOnly
        }
        // The dialog fills its parent: inside the Loader (width 0) it was 26 px wide and invisible, so it hangs on the window content.
        onLoaded: item.parent = Window.contentItem
        function openFor(gameId) {
            active = true
            if (item !== null) item.openStartFor(gameId)
        }
    }
    RandomizeDialog {
        id: randomizeDialog
        player: root.player
        onEngineNeeded: { root.cancelEngineBuild = false; engineDialog.openFor(randomizeDialog.nameText) }
        onCreated: (id, small) => {
            if (root.cancelEngineBuild) { root.cancelEngineBuild = false; root.vc.cancel(id); return }
            if (!small) root.watchBuild(id)
        }
    }
    VariantEngineDialog {
        id: engineDialog
        player: root.player
        onConfirmed: randomizeDialog.startBuild()
        onCancelled: {
            if (root.vc.current.variantId !== undefined && root.vc.current.variantId !== "") root.vc.cancel(root.vc.current.variantId)
            else root.cancelEngineBuild = randomizeDialog.awaiting
        }
    }
    VariantBuildDialog { id: buildDialog; player: root.player }
    VariantDialogs {
        id: variantDialogs
        anchors.fill: parent
        player: root.player
        gameTitle: root.game.title || ""
        onRebuilding: (id) => root.watchBuild(id)
    }
    ScreenshotViewer { id: screenshotViewer; player: root.player; parent: Window.contentItem }
}
