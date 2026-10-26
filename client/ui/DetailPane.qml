import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Detail pane (3c-1-r6) for the selected game: cover, title and meta line, the primary action directly below, at most one
// problem sentence (when it blocks play its fix is the primary: "Install core and play", "Retry connection", "Ask an admin"),
// tabs Overview · Saves · Details, then the secondary actions and the `⋯` menu. The start checklist only shows while a start
// runs or after it failed.
Rectangle {
    id: root
    objectName: "detailPane"
    required property PlayerController player
    property bool closable: false     // Grid view: the column opens over the grid and has a close button
    signal closeRequested()
    readonly property var game: player.selectedGame
    readonly property bool hasGame: game.id !== undefined
    // 3c-5: the selected game is the one paused in the background (detail column with Resume / Quit game).
    readonly property bool running: game.running === true
    // Per person (0.19, 3c-1): play time, last played, sessions, favourite, collections; supported = false on an older server.
    readonly property var personal: game.personal ?? ({})
    readonly property bool personalShown: root.personal.supported === true
    // "overview" (facts, SAVE summary), "saves" (3c-3 saves view in the same column) or "details" (ROM, core, firmware rows)
    property string mode: "overview"
    readonly property SaveHistoryController hist: player.saveHistory
    // A restore, delete or upload confirmation is open, or an upload runs: Play is disabled (decision af).
    readonly property bool confirming: hist.confirmationOpen
    readonly property string gameId: game.id === undefined ? "" : game.id
    onGameIdChanged: {
        root.leaveSaves()
        if (gameId !== "") fadeIn.restart()
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
    Component.onCompleted: syncPluginGame()
    readonly property var bg: player.backgroundGame
    readonly property string sinceText: !running || !bg.startedMs ? "" :
        qsTr("%1 · paused %2").arg(Qt.formatTime(new Date(bg.startedMs), "HH:mm")).arg(Qt.formatTime(new Date(bg.pausedMs), "HH:mm"))
    function openSaves(upload) {
        root.mode = "saves"
        savesLoader.active = true
        if (upload === true) Qt.callLater(() => { if (savesLoader.item !== null) savesLoader.item.openUpload() })
        else Qt.callLater(() => { if (savesLoader.item !== null) savesLoader.item.forceActiveFocus() })
    }
    function leaveSaves() {
        if (savesLoader.item !== null) savesLoader.item.cancelAll()
        root.mode = "overview"
    }
    NumberAnimation { id: fadeIn; target: flick; property: "opacity"; from: 0.35; to: 1; duration: Theme.durPage }
    // The one problem sentence and its fix (GameDetail: problemText, fixKind).
    readonly property string problem: running ? "" : (game.problemText || "")
    readonly property string fixKind: running ? "" : (game.fixKind || "")
    readonly property bool admin: player.admin.available
    readonly property string primaryText: {
        if (running) return qsTr("▶ Resume")
        if (fixKind === "admin") return admin ? qsTr("Open Systems & cores") : qsTr("Ask an admin")
        if (fixKind === "retry") return qsTr("Retry connection")
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
    readonly property var tabItems: [qsTr("Overview"), qsTr("Saves"), qsTr("Details")]
    readonly property string tabActive: mode === "saves" ? tabItems[1] : mode === "details" ? tabItems[2] : tabItems[0]
    function pickTab(label) {
        if (label === tabItems[1]) root.openSaves(false)
        else if (label === tabItems[2]) { root.leaveSaves(); root.mode = "details" }
        else root.leaveSaves()
    }

    color: Theme.ground
    implicitWidth: Theme.detailWidth

    Rectangle {
        anchors.left: parent.left
        width: 1
        height: parent.height
        color: Theme.rule
    }
    UzIconButton {
        objectName: "detailClose"
        visible: root.closable && root.hasGame
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.space1
        z: 2
        glyph: "cross"
        glyphSize: 12
        text: qsTr("Close")
        Accessible.name: qsTr("Close the game")
        onClicked: root.closeRequested()
    }

    UzLabel {
        objectName: "detailEmptyHint"
        // An empty Library already says what to do in its own column: no second hint with nothing to select (R-P14).
        visible: !root.hasGame && root.player.library.totalCount > 0
        anchors.centerIn: parent
        text: qsTr("Select a game from the Library.")
        color: Theme.dim
        font.pixelSize: Theme.roles.input.size
    }

    ColumnLayout {
        visible: root.hasGame
        anchors.fill: parent
        anchors.margins: Theme.space3
        spacing: Theme.space2

        // Cover, title and meta line
        RowLayout {
            id: titleRow
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.space2
            // Cover placeholder (S3c1R6): `placeholder` plane, monogram, the system chip at the top left.
            Rectangle {
                implicitWidth: 88
                implicitHeight: 88
                Layout.alignment: Qt.AlignTop
                color: Theme.placeholder
                border.width: 1
                border.color: Theme.placeholderRule
                UzLabel {
                    anchors.centerIn: parent
                    text: root.game.monogram || ""
                    font.pixelSize: 32
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
                    Layout.preferredWidth: 1
                    wrapMode: Text.WordWrap
                }
                UzLabel {
                    objectName: "detailTitle"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    wrapMode: Text.Wrap
                    text: root.game.title || ""
                    font.pixelSize: Theme.roles.dialogTitle.size
                    font.weight: Theme.roles.dialogTitle.weight
                    font.letterSpacing: Theme.roles.dialogTitle.spacing
                }
                // Meta line: `SYSTEM · PLAY TIME · LAST PLAYED` (S3c1R6); on a server without per-person data `SYSTEM · CORE`.
                UzMono {
                    objectName: "detailSystem"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    readonly property string coreLine: (root.game.coreVersionText || "") !== ""
                          ? qsTr("%1 · %2 %3").arg(root.game.systemName || "").arg(root.game.coreLabelText || "").arg(root.game.coreVersionText)
                          : ((root.game.coreLabelText || "") !== "" ? qsTr("%1 · %2").arg(root.game.systemName || "").arg(root.game.coreLabelText)
                                                                      : (root.game.systemName || ""))
                    text: root.personalShown
                          ? [(root.game.system || "").toUpperCase(),
                             ...(root.personal.playTimeText && root.personal.playTimeText !== "–" ? [root.personal.playTimeText] : []),
                             root.personal.lastPlayedText && root.personal.lastPlayedText !== "–" ? qsTr("last %1").arg(root.personal.lastPlayedText) : qsTr("never played")].join(" · ")
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
            }
        }

        // The primary action directly below the title; the problem sentence and its fix follow.
        UzButton {
            objectName: "playButton"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: Theme.row44
            kind: "primary"
            busy: root.game.busy === true
            busyOnClick: true
            text: root.primaryText
            enabled: root.running ? !root.player.quitting
                     : (root.fixKind === "admin" || root.fixKind === "retry") ? !root.confirming
                     : (root.game.canPlay === true && !root.confirming)
            onClicked: root.primaryAction()
        }
        Item {
            id: problemBox
            objectName: "detailProblem"
            visible: root.problem !== ""
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: problemText.implicitHeight
            Rectangle {
                y: 2
                width: 14; height: 14
                color: Theme.accent
                Text { anchors.centerIn: parent; text: "!"; font.family: Theme.sans; font.pixelSize: 10; font.weight: Font.Bold; color: Theme.inkOnAccent }
            }
            UzLabel {
                id: problemText
                objectName: "playDisabledReason"
                x: 14 + Theme.space15
                width: Math.max(0, parent.width - x)
                wrapMode: Text.Wrap
                color: Theme.ink
                font.pixelSize: Theme.roles.input.size
                text: root.problem
            }
        }
        UzButton {
            objectName: "firmwareRecheck"
            visible: root.game.firmwareBlocked === true
            kind: "link"
            text: qsTr("Check firmware again")
            onClicked: root.player.recheckFirmware()
        }

        UzTabs {
            id: tabs
            objectName: "detailTabs"
            fit: true
            Layout.fillWidth: true
            items: root.tabItems
            active: root.tabActive
            onPicked: (label) => root.pickTab(label)
        }

        Flickable {
            id: flick
            visible: root.mode !== "saves"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            contentWidth: width
            contentHeight: content.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: content
                width: flick.width
                spacing: Theme.space2

                // Overview: status tag, facts of the person, saves, screenshots, plugins; the start checklist only while a start
                // runs or after it failed.
                // (Flat children of `content`: a nested layout around wrapped texts made the layouts polish in loops.)
                    UzTag {
                        objectName: "detailPill"
                        visible: root.mode === "overview" && (root.game.pillText || "") !== "" && root.problem === ""
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: content.width
                        tone: root.game.pillTone || "neutral"
                        text: root.game.pillText || ""
                    }
                    // Your play (3c-1 facts): play time and last played, sessions, collections with "+ Collection".
                    ColumnLayout {
                        objectName: "personalFacts"
                        visible: root.mode === "overview" && root.personalShown
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 0
                        Repeater {
                            model: [
                                { label: qsTr("Play time"), text: root.personal.playTimeText ?? "–", name: "factPlayTime" },
                                { label: qsTr("Last played"), text: root.personal.lastPlayedText ?? "–", name: "factLastPlayed" },
                                { label: qsTr("Sessions"), text: root.personal.sessions > 0 ? String(root.personal.sessions) : "–", name: "factSessions" },
                                { label: qsTr("Collections"), text: (root.personal.collectionNames ?? []).length > 0 ? root.personal.collectionNames.join(", ") : "–",
                                  name: "factCollections" }
                            ]
                            delegate: Item {
                                id: fact
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                implicitHeight: Math.max(factLabel.implicitHeight, factValue.implicitHeight) + 2 * Theme.space1
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                                UzMono {
                                    id: factLabel
                                    y: Theme.space1
                                    width: 96
                                    text: fact.modelData.label
                                    elide: Text.ElideRight
                                    color: Theme.dim
                                    font.pixelSize: Theme.roles.sectionLabel.size
                                    font.weight: Theme.roles.sectionLabel.weight
                                    font.letterSpacing: Theme.roles.sectionLabel.spacing
                                    font.capitalization: Font.AllUppercase
                                }
                                UzLabel {
                                    id: factValue
                                    objectName: fact.modelData.name
                                    y: Theme.space1
                                    x: 96 + Theme.space15
                                    width: fact.width - x
                                    wrapMode: Text.Wrap
                                    text: fact.modelData.text
                                    font.pixelSize: Theme.roles.input.size
                                    color: Theme.ink2
                                }
                            }
                        }
                    }

                    ScreenshotStrip {
                        placed: root.mode === "overview"
                        player: root.player
                        gameId: root.hasGame ? root.gameId : ""
                        onOpenRequested: (id) => screenshotViewer.openAt(id)
                    }

                    SaveSummary {
                        visible: root.mode === "overview"
                        player: root.player
                        Layout.maximumWidth: content.width
                        onManage: root.openSaves(false)
                        onUploadRequested: root.openSaves(true)
                    }

                    PluginDetailSection {
                        objectName: "pluginDetailSections"
                        placed: root.mode === "overview"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        player: root.player
                    }

                    UzLabel {
                        objectName: "runningNote"
                        visible: root.mode === "overview" && root.running
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
                    // Local-only game (0.12): the file stays on a Player until it is uploaded.
                    ColumnLayout {
                        objectName: "localOnlyBox"
                        visible: root.mode === "overview" && root.game.localOnly === true && !root.running
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
                    // Start checklist
                    ColumnLayout {
                        objectName: "startChecklist"
                        visible: root.mode === "overview" && !root.running && (root.game.busy === true || (root.game.error || "") !== "")
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

                    Rectangle {
                        visible: root.mode === "overview" && (root.game.error || "") !== "" && !root.running
                        objectName: "startError"
                        Layout.fillWidth: true
                        implicitHeight: errText.implicitHeight + 2 * Theme.space15
                        color: Theme.dangerTint
                        Rectangle { width: 2; height: parent.height; color: Theme.danger }
                        UzLabel {
                            id: errText
                            anchors.fill: parent
                            anchors.margins: Theme.space15
                            text: root.game.error || ""
                            wrapMode: Text.WordWrap
                            font.pixelSize: Theme.roles.input.size
                            color: Theme.danger
                        }
                    }

                // Status table
                ColumnLayout {
                    objectName: "detailFacts"
                    visible: root.mode === "details"
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
                            // A game that starts from a library folder (3c-7): the file, the core and how it relates to the Hub.
                            { label: qsTr("File"), text: root.game.localPath || "", tone: "neutral", hint: "", objName: "detailLocalPath", mono: true },
                            { label: root.game.onHub ? qsTr("Matches") : qsTr("On the server"),
                              text: root.game.onHub ? qsTr("Server ROM %1").arg(root.game.hubMatchText || "") : qsTr("Not uploaded"), tone: "neutral", hint: "" },
                            { label: qsTr("Core"), text: root.game.coreText, tone: root.game.coreTone, hint: root.game.coreHint }
                        ] : [
                            { label: qsTr("ROM"), text: root.game.romText, tone: root.game.romTone, hint: "" },
                            { label: qsTr("Size"), text: root.game.fileSizeText || "–", tone: "neutral", hint: "" },
                            { label: qsTr("Core"), text: root.game.coreText, tone: root.game.coreTone, hint: root.game.coreHint },
                            { label: qsTr("Firmware"), text: root.game.firmwareText, tone: root.game.firmwareTone, hint: root.game.firmwareHint }
                        ].concat(root.gamePlugins.length > 0 ? [
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
                                    // Label column of the facts table (board S3c1): Mono section-label role, 96 px, value to its right.
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
                                        // The value takes the rest of the row and wraps: a long status text must never widen the pane.
                                        // A file path is cut in the middle instead (the full path is the tooltip).
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
                    visible: root.mode === "details" && (root.game.coreNotice || "") !== ""
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    text: root.game.coreNotice || ""
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                    wrapMode: Text.WordWrap
                }
            }
        }

        Loader {
            id: savesLoader
            active: false
            visible: root.mode === "saves"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            sourceComponent: SavesView {
                player: root.player
                onBack: root.leaveSaves()
                onStartRunRequested: runDialogLoader.openFor(root.gameId)
            }
        }

        // Secondary actions and the `⋯` menu (S3c1R6). Play and share Session / Quit game keeps its name for the tests and the
        // running state (a paused game offers Quit here).
        Item {
          id: secondaryBox
          Layout.fillWidth: true
          Layout.minimumWidth: 0
          implicitHeight: secondary.height
          Flow {
            id: secondary
            objectName: "detailSecondaryRow"
            width: secondaryBox.width
            spacing: Theme.space1
            UzButton {
                id: favoriteButton
                objectName: "favoriteButton"
                visible: root.personalShown && !root.running
                on: root.personal.favorite === true
                text: root.personal.favorite === true ? qsTr("★ Favourite") : qsTr("☆ Favourite")
                size: "sm"
                Accessible.checkable: true
                Accessible.checked: root.personal.favorite === true
                Accessible.name: qsTr("Favourite")
                onClicked: root.player.personal.toggleFavorite(root.gameId)
            }
            UzButton {
                objectName: "addToCollection"
                visible: root.personalShown && !root.running
                size: "sm"
                text: qsTr("Add to collection")
                onClicked: detailCollections.openAdd(root.gameId)
            }
            UzButton {
                objectName: "startRunButton"
                visible: !root.running && root.mode !== "saves" && root.player.runs.available && root.game.onHub !== false
                size: "sm"
                text: qsTr("Start a run")
                onClicked: runDialogLoader.openFor(root.gameId)
            }
            UzButton {
                objectName: "uploadToHubButton"
                visible: !root.running && root.game.onHub === false && root.game.localFile === true
                size: "sm"
                text: qsTr("Upload to server")
                enabled: root.player.canUpload && !root.player.upload.active
                onClicked: root.player.uploadGameToHub(root.game.id)
            }
            UzButton {
                objectName: "playShareButton"
                visible: (root.player.sessions.available && root.game.onHub !== false) || root.running
                size: "sm"
                text: root.game.running === true ? qsTr("Quit game") : qsTr("Play and share")
                enabled: (root.running && !root.player.quitting) || (root.game.canPlay === true && !root.confirming)
                onClicked: root.running ? root.player.requestQuit() : root.player.playAndShareSelected()
            }
            UzIconButton {
                id: moreButton
                objectName: "detailMoreButton"
                visible: !root.running
                glyph: "more"
                on: moreMenu.opened
                text: qsTr("More actions")
                Accessible.name: qsTr("More actions for %1").arg(root.game.title || "")
                onClicked: moreMenu.openFor(moreButton, "above")
            }
          }
        }
        UzLabel {
            objectName: "quitNote"
            visible: root.running
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: Theme.dim
            font.pixelSize: Theme.roles.small.size
            text: root.player.quitting ? qsTr("Saving and syncing…") : qsTr("Quit saves and syncs to the server first")
        }
    }

    // `⋯`: per-game settings, the file, and removing a local game from the library (the file stays where it is).
    UzMenu {
        id: moreMenu
        objectName: "detailMoreMenu"
        readonly property int changes: { root.player.emulation.gameOverrideCount; return root.player.emulation.gameChangedCount(root.gameId) }
        items: [
            { label: moreMenu.changes > 0 ? qsTr("Settings for this game… (%n)", "", moreMenu.changes) : qsTr("Settings for this game…"), name: "gameSettings" }
        ].concat(root.game.localFile === true ? [{ label: qsTr("Show in folder"), name: "showInFolder" }] : [])
         .concat(root.game.onHub === false ? [{ separator: true }, { label: qsTr("Remove from library"), name: "removeFromLibrary", danger: true, disabled: root.player.upload.active }] : [])
        onChosen: (item) => {
            if (item.name === "gameSettings") gameSettings.open(root.gameId)
            else if (item.name === "showInFolder") root.player.showInFolder(root.game.localPath || "")
            else if (item.name === "removeFromLibrary") root.player.removeFromLibrary(root.game.id)
        }
    }
    CollectionDialogs { id: detailCollections; player: root.player }
    GameSettingsDialog { id: gameSettings; objectName: "detailGameSettings"; player: root.player }
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
            pcOnly: root.player.localHub.onThisPcOnly
        }
        function openFor(gameId) {
            active = true
            if (item !== null) item.openStartFor(gameId)
        }
    }
    ScreenshotViewer { id: screenshotViewer; player: root.player; parent: Window.contentItem }
}
