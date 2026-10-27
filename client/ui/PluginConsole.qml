import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Plugin developer console (boards 4d-1 and 4d-2, Uzume 0.22): the one console of the app. It sits in Settings > This PC > Plugins
// (`settingsMode`, with Load unpacked...) and is the last tab of the game panel (Developer mode only). Plugin select with the Unpacked
// tag and folder, Reload, Permissions, Log (level chips with counts, Pause log, newest first), Watched, Events, Inspect (run state, store
// keys), the memory viewer and the memory map. Everything comes from PluginsController.devConsole, the model the debug MCP tools read too.
FocusScope {
    id: root
    required property PlayerController player
    property bool settingsMode: false     // in Settings: "Load unpacked..." and the wider log
    property int maxLogRows: settingsMode ? 150 : 60
    signal loadUnpackedRequested()
    readonly property PluginsController ctl: player.plugins
    readonly property DevConsole dev: ctl.devConsole
    readonly property var con: ctl.console
    readonly property var pluginList: con.plugins || []
    readonly property string selected: con.selected || ""
    readonly property string status: con.status || "stopped"
    readonly property bool failed: status === "error" || (status === "paused" && (con.error || "") !== "")
    readonly property bool running: con.running === undefined ? true : con.running
    readonly property bool unpacked: !!con.unpacked
    readonly property var watched: con.watched || []
    readonly property var events: con.events || []
    readonly property var logLines: con.log || []
    readonly property var regions: con.regions || []
    readonly property var counts: con.counts || ({ error: 0, warn: 0, info: 0, debug: 0, total: 0 })
    readonly property var filters: con.filters || ({ error: true, warn: true, info: true, debug: true })
    readonly property var perms: con.permissions || ({ granted: [], notGranted: [] })
    readonly property var inspect: con.inspect || ({})
    readonly property var mem: dev.memory
    readonly property var loadResult: ctl.unpackedResult
    objectName: "pluginConsole"
    implicitHeight: flick.contentHeight
    // Taking focus on a click is what lets Ctrl+R mean "reload" here and nowhere else (SoulLink uses Ctrl+R in the game).
    TapHandler { acceptedButtons: Qt.LeftButton; onTapped: root.forceActiveFocus() }

    function dotColor(st: string): color {
        switch (st) {
        case "running": return Theme.ok
        case "error": return Theme.danger
        case "paused": return Theme.accentText
        default: return Theme.dim
        }
    }
    function levelColor(level: string): color {
        switch (level) {
        case "warn": return Theme.accentText
        case "error": return Theme.danger
        case "debug": return Theme.dim
        default: return Theme.ink
        }
    }
    function selectedName(): string {
        for (let i = 0; i < pluginList.length; ++i)
            if (pluginList[i].id === selected) return pluginList[i].name
        return selected
    }

    Shortcut {
        sequence: "Ctrl+Shift+R"
        enabled: root.visible && !root.settingsMode
        onActivated: root.ctl.reloadSelected()
    }
    // Ctrl+R: only while this console has the focus.
    Shortcut {
        sequence: "Ctrl+R"
        enabled: root.visible && root.activeFocus
        onActivated: root.ctl.reloadSelected()
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.implicitHeight + 2 * Theme.space3
        clip: true
        interactive: !root.settingsMode
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        Column {
            id: col
            x: Theme.space3
            y: Theme.space2
            width: flick.width - 2 * Theme.space3
            spacing: Theme.space3

            // ---- plugin select + reload
            RowLayout {
                width: parent.width
                spacing: Theme.space1
                // Select field (design-system.md "Select", 36 px); the list is the anchored popover below.
                Rectangle {
                    id: selectBtn
                    objectName: "consoleSelect"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: Theme.row36
                    color: selectPop.open ? Theme.pressed : (Theme.dark ? "transparent" : Theme.surface)
                    border.width: 1
                    border.color: selectPop.open ? Theme.ink : (selectHover.hovered ? Theme.ink2 : Theme.rule)
                    activeFocusOnTab: true
                    Accessible.role: Accessible.ComboBox
                    Accessible.name: root.selectedName()
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                            selectPop.open = !selectPop.open; event.accepted = true
                        }
                    }
                    Rectangle {
                        objectName: "consoleStatusDot"
                        x: Theme.space15
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8; height: 8
                        color: root.dotColor(root.status)
                    }
                    UzTag {
                        id: unpackedTag
                        objectName: "consoleUnpackedTag"
                        visible: root.unpacked && selectBtn.width >= 320  // narrow panel: the name wins; the folder row below says "unpacked" anyway
                        small: true
                        icon: "none"
                        text: qsTr("Unpacked")
                        anchors.right: chevron.left
                        anchors.rightMargin: Theme.space1
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.space15 + 8 + Theme.space1
                        anchors.right: unpackedTag.visible ? unpackedTag.left : chevron.left
                        anchors.rightMargin: Theme.space1
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        text: root.selectedName()
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.input.size
                        color: Theme.ink
                    }
                    UzGlyph {
                        id: chevron
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.space15
                        anchors.verticalCenter: parent.verticalCenter
                        name: selectPop.open ? "chevronUp" : "chevronDown"
                        size: 14
                        color: Theme.muted
                    }
                    HoverHandler { id: selectHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: selectPop.open = !selectPop.open }
                    UzFocusRing { visible: selectBtn.activeFocus }
                }
                UzButton {
                    objectName: "consoleLoadUnpacked"
                    visible: root.settingsMode
                    textFormat: Text.PlainText
                    text: qsTr("Load unpacked…")
                    onClicked: root.loadUnpackedRequested()
                }
                UzButton {
                    objectName: "consoleReload"
                    kind: "primary"
                    property string tip: qsTr("Reload plugin · Ctrl+R")
                    text: qsTr("Reload")
                    enabled: root.selected !== ""
                    onClicked: root.ctl.reloadPlugin(root.selected)
                    ToolTip.visible: hovered
                    ToolTip.text: tip
                    ToolTip.delay: 600
                    Accessible.description: tip
                }
            }

            // ---- folder of an unpacked plugin and "Reload on file change"
            RowLayout {
                objectName: "consoleUnpackedRow"
                visible: root.unpacked
                width: parent.width
                spacing: Theme.space15
                UzMono {
                    objectName: "consoleFolder"
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: con.folder || ""
                    font.pixelSize: Theme.roles.meta11.size
                    color: Theme.muted
                }
                UzToggle {
                    objectName: "consoleReloadOnChange"
                    text: qsTr("Reload on file change")
                    checked: root.dev.reloadOnChange
                    onToggled: root.dev.reloadOnChange = checked
                    Accessible.description: qsTr("Reloads this plugin when a file in its folder changes.")
                }
            }

            // ---- Load unpacked: manifest problem (board 4d-2B) or loaded
            Column {
                id: loadBox
                objectName: "consoleLoadResult"
                visible: root.settingsMode && !!root.loadResult && root.loadResult.state !== undefined
                width: parent.width
                spacing: Theme.space1
                UzStatusIcon {
                    kind: root.loadResult && root.loadResult.state === "error" ? "error" : "done"
                    text: root.loadResult ? (root.loadResult.title || "") : ""
                    maxWidth: loadBox.width
                }
                UzMono {
                    textFormat: Text.PlainText
                    objectName: "consoleLoadDetail"
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    text: root.loadResult ? (root.loadResult.detail || "") : ""
                    font.pixelSize: Theme.roles.meta11.size
                    color: Theme.ink
                }
                Flow {
                    width: parent.width
                    spacing: Theme.space15
                    UzButton {
                        objectName: "consoleLoadRetry"
                        visible: root.loadResult && root.loadResult.state === "error"
                        size: "sm"
                        textFormat: Text.PlainText
                        text: qsTr("Try again")
                        onClicked: root.loadUnpackedRequested()
                    }
                    UzLabel {
                        textFormat: Text.PlainText
                        width: Math.min(implicitWidth, loadBox.width)
                        wrapMode: Text.WordWrap
                        visible: root.loadResult && root.loadResult.state === "error"
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.muted
                        text: root.loadResult ? (root.loadResult.note || "") : ""
                    }
                    UzButton {
                        objectName: "consoleLoadDismiss"
                        kind: "link"
                        size: "sm"
                        textFormat: Text.PlainText
                        text: qsTr("Dismiss")
                        onClicked: root.ctl.dismissUnpackedResult()
                    }
                }
            }

            // ---- nothing runs for this plugin
            UzLabel {
                objectName: "consoleNotRunning"
                textFormat: Text.PlainText
                visible: root.selected !== "" && !root.running
                width: parent.width
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.roles.small.size
                color: Theme.dim
                text: qsTr("This plugin is not running. It starts with a game it matches; then Reload applies.")
            }
            UzLabel {
                objectName: "consoleNoPlugins"
                textFormat: Text.PlainText
                visible: root.selected === ""
                width: parent.width
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.roles.small.size
                color: Theme.dim
                text: root.settingsMode ? qsTr("No plugin is running. Load an unpacked plugin from a folder, or start a game.") : qsTr("No plugin is running.")
            }

            // ---- error (board 4d-2 "Paused after errors"): an icon and a sentence, never a tinted banner
            Column {
                id: errBox
                objectName: "consoleError"
                visible: root.failed
                width: parent.width
                spacing: Theme.space15
                UzStatusIcon {
                    kind: "error"
                    text: root.con.errorLine || qsTr("Paused after an error. Your game keeps running.")
                    maxWidth: errBox.width
                }
                UzMono {
                    textFormat: Text.PlainText
                    objectName: "consoleErrorText"
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    text: root.con.error || ""
                    font.pixelSize: Theme.roles.meta11.size
                    color: Theme.ink
                    maximumLineCount: 12
                    elide: Text.ElideRight
                }
                UzLabel {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.muted
                    text: qsTr("Three errors within 10 s pause a plugin. Panels from this plugin show “Paused”. Fix the file, then Reload and resume.")
                }
                UzButton {
                    objectName: "consoleRestart"
                    kind: "primary"
                    size: "sm"
                    text: qsTr("Reload and resume")
                    onClicked: root.ctl.reloadPlugin(root.selected)
                }
            }

            // ---- last reload (board 4d-2E): what a reload keeps
            UzLabel {
                objectName: "consoleReloaded"
                textFormat: Text.PlainText
                visible: !!(root.con.reload && root.con.reload.count > 0 && root.con.reload.text)
                width: parent.width
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
                text: (root.con.reload ? root.con.reload.text : "") + " · " + qsTr("Run state, store and cache are kept; Lua variables start fresh.")
            }

            // ---- PERMISSIONS (board 4d-1): what the plugin may do on load
            Column {
                objectName: "consolePermissions"
                visible: root.selected !== ""
                width: parent.width
                spacing: Theme.space1
                Eyebrow { color: Theme.dim; text: qsTr("Permissions") }
                Flow {
                    width: parent.width
                    spacing: Theme.space1
                    Repeater {
                        model: root.perms.granted
                        delegate: UzTag {
                            required property var modelData
                            objectName: "consolePermGranted"
                            tone: "ok"
                            text: modelData.label
                        }
                    }
                    UzLabel {
                        textFormat: Text.PlainText
                        visible: root.perms.granted.length === 0
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.dim
                        text: qsTr("No special permissions")
                    }
                }
                UzLabel {
                    objectName: "consolePermNotGranted"
                    textFormat: Text.PlainText
                    visible: root.perms.notGranted.length > 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.danger
                    text: qsTr("Not granted: %1").arg(root.perms.notGranted.map(function (p) { return p.label }).join(", "))
                }
            }

            // ---- WATCHED
            Column {
                width: parent.width
                spacing: Theme.space1
                RowLayout {
                    width: parent.width
                    Eyebrow { Layout.fillWidth: true; color: Theme.dim; text: qsTr("Watched · %1").arg(root.watched.length) }
                    UzMono { textFormat: Text.PlainText; text: qsTr("live"); font.pixelSize: Theme.roles.meta11.size; color: Theme.ok }
                }
                UzLabel {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    objectName: "consoleWatchedEmpty"
                    visible: root.watched.length === 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.dim
                    text: qsTr("Nothing watched. Use fb.dev.watch(label, address, kind) in the plugin.")
                }
                Repeater {
                    model: root.watched
                    delegate: Row {
                        required property var modelData
                        objectName: "consoleWatched"
                        spacing: Theme.space1
                        UzMono { textFormat: Text.PlainText; width: 92; text: modelData.address; font.pixelSize: Theme.roles.meta11.size; color: Theme.muted }
                        UzMono { textFormat: Text.PlainText; width: 70; text: modelData.value; font.pixelSize: Theme.roles.meta11.size; color: Theme.accentText; elide: Text.ElideRight }
                        UzLabel { textFormat: Text.PlainText; width: col.width - 92 - 70 - 16; text: modelData.label; font.pixelSize: Theme.roles.small.size; color: Theme.ink; elide: Text.ElideRight }
                    }
                }
            }

            // ---- EVENTS
            Column {
                width: parent.width
                spacing: Theme.space1
                Eyebrow { color: Theme.dim; text: qsTr("Events") }
                UzLabel {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    visible: root.events.length === 0
                    width: parent.width
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.dim
                    text: qsTr("No events yet.")
                }
                Repeater {
                    model: root.events.slice(0, 25)
                    delegate: Row {
                        required property var modelData
                        objectName: "consoleEvent"
                        spacing: Theme.space1
                        UzMono { textFormat: Text.PlainText; width: 92; text: modelData.time; font.pixelSize: Theme.roles.meta11.size; color: Theme.dim }
                        UzTag {
                            width: Math.min(110, implicitWidth)
                            small: true
                            tone: "accent"
                            icon: "none"
                            text: modelData.kind
                        }
                        UzMono { textFormat: Text.PlainText; width: col.width - 92 - 110 - 16; text: modelData.text; font.pixelSize: Theme.roles.meta11.size; color: Theme.ink; elide: Text.ElideRight }
                    }
                }
            }

            // ---- LOG (board 4d-1): level chips with counts, Pause log, newest first
            Column {
                width: parent.width
                spacing: Theme.space1
                RowLayout {
                    width: parent.width
                    Eyebrow { Layout.fillWidth: true; color: Theme.dim; text: qsTr("Log") }
                    UzButton {
                        textFormat: Text.PlainText
                        objectName: "consolePauseLog"
                        kind: "link"
                        implicitHeight: 24
                        font.pixelSize: Theme.roles.small.size
                        text: root.con.logPaused ? (root.con.logPending > 0 ? qsTr("Resume log · %1 new").arg(root.con.logPending) : qsTr("Resume log")) : qsTr("Pause log")
                        onClicked: root.dev.setLogPaused(!root.con.logPaused)
                    }
                    UzButton {
                        textFormat: Text.PlainText
                        objectName: "consoleCopy"
                        kind: "link"
                        implicitHeight: 24
                        font.pixelSize: Theme.roles.small.size
                        text: qsTr("Copy")
                        onClicked: root.ctl.copyLog(root.selected)
                    }
                    UzButton {
                        textFormat: Text.PlainText
                        objectName: "consoleClear"
                        kind: "link"
                        implicitHeight: 24
                        font.pixelSize: Theme.roles.small.size
                        text: qsTr("Clear")
                        onClicked: root.ctl.clearLog(root.selected)
                    }
                }
                Flow {
                    width: parent.width
                    spacing: Theme.space1
                    Repeater {
                        model: [{ level: "error", label: qsTr("Error") }, { level: "warn", label: qsTr("Warn") }, { level: "info", label: qsTr("Info") }, { level: "debug", label: qsTr("Debug") }]
                        delegate: UzFilterChip {
                            required property var modelData
                            objectName: "consoleChip_" + modelData.level
                            text: modelData.label
                            count: root.counts[modelData.level] || 0
                            active: root.filters[modelData.level] !== false
                            onClicked: root.dev.setLevel(modelData.level, !active)
                        }
                    }
                }
                UzLabel {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    objectName: "consoleLogEmpty"
                    visible: root.logLines.length === 0
                    width: parent.width
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.dim
                    text: root.counts.total > 0 ? qsTr("No lines at these levels.") : qsTr("The log is empty.")
                }
                Row {
                    visible: root.logLines.length > 0
                    spacing: Theme.space1
                    UzMono { width: 92; text: qsTr("TIME"); font.pixelSize: Theme.roles.meta11.size; color: Theme.dim }
                    UzMono { width: 52; text: qsTr("LEVEL"); font.pixelSize: Theme.roles.meta11.size; color: Theme.dim }
                    UzMono { text: qsTr("MESSAGE"); font.pixelSize: Theme.roles.meta11.size; color: Theme.dim }
                }
                Repeater {
                    model: root.logLines.slice(0, root.maxLogRows)
                    delegate: Row {
                        required property var modelData
                        objectName: "consoleLog_" + modelData.level
                        spacing: Theme.space1
                        UzMono { textFormat: Text.PlainText; width: 92; text: modelData.time; font.pixelSize: Theme.roles.meta11.size; color: Theme.dim }
                        UzTag {
                            width: 52
                            small: true
                            icon: "none"
                            tone: modelData.level === "error" ? "danger" : modelData.level === "warn" ? "accent" : "neutral"
                            text: modelData.level
                        }
                        UzMono { textFormat: Text.PlainText; width: col.width - 92 - 52 - 16; text: modelData.text; font.pixelSize: Theme.roles.meta11.size; color: root.levelColor(modelData.level); wrapMode: Text.WrapAnywhere }
                    }
                }
            }

            // ---- INSPECT: run state and store keys of the plugin (watched values are above)
            Column {
                objectName: "consoleInspect"
                visible: root.selected !== "" && root.running
                width: parent.width
                spacing: Theme.space1
                Eyebrow { color: Theme.dim; text: qsTr("Inspect") }
                UzLabel {
                    objectName: "consoleRunState"
                    textFormat: Text.PlainText
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.dim
                    visible: !root.inspect.runState
                    text: qsTr("No run state. The game was not started in a run, or the plugin wrote none.")
                }
                UzMono {
                    objectName: "consoleRunStateText"
                    textFormat: Text.PlainText
                    visible: !!root.inspect.runState
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 6
                    elide: Text.ElideRight
                    text: qsTr("Run %1 · state %2").arg(root.inspect.runName || "").arg(root.inspect.runState || "")
                    font.pixelSize: Theme.roles.meta11.size
                    color: Theme.ink
                }
                UzLabel {
                    objectName: "consoleStoreEmpty"
                    textFormat: Text.PlainText
                    visible: !root.inspect.store || root.inspect.store.length === 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.dim
                    text: qsTr("No store keys.")
                }
                Repeater {
                    model: root.inspect.store || []
                    delegate: Row {
                        required property var modelData
                        objectName: "consoleStoreKey"
                        spacing: Theme.space1
                        UzMono { textFormat: Text.PlainText; width: 110; text: modelData.key; font.pixelSize: Theme.roles.meta11.size; color: Theme.accentText; elide: Text.ElideRight }
                        UzMono { textFormat: Text.PlainText; width: 48; text: modelData.bytes + " B"; font.pixelSize: Theme.roles.meta11.size; color: Theme.dim }
                        UzMono { textFormat: Text.PlainText; width: col.width - 110 - 48 - 16; text: modelData.preview; font.pixelSize: Theme.roles.meta11.size; color: Theme.ink; elide: Text.ElideRight }
                    }
                }
            }

            // ---- MEMORY VIEWER: region, go to address, hex + ASCII, search (read only)
            Column {
                id: memBox
                objectName: "consoleMemoryViewer"
                visible: !!root.mem.available
                width: parent.width
                spacing: Theme.space1
                Eyebrow { color: Theme.dim; text: qsTr("Memory viewer") }
                Timer {
                    interval: 1000
                    repeat: true
                    running: memBox.visible && root.visible && !!root.mem.available && !root.player.gameSession.paused
                    onTriggered: root.dev.memoryRefresh()
                }
                Flow {
                    width: parent.width
                    spacing: Theme.space1
                    UzSelect {
                        objectName: "consoleMemRegion"
                        width: Math.min(memBox.width, 260)
                        accessibleName: qsTr("Memory region")
                        model: root.mem.regions || []
                        current: String(root.mem.regionIndex)
                        onPicked: value => root.dev.memorySelectRegion(parseInt(value))
                    }
                    UzField {
                        id: gotoField
                        objectName: "consoleMemGoto"
                        mono: true
                        width: 150
                        placeholderText: qsTr("Go to 0x08001A10")
                        onAccepted: root.dev.memoryGoto(text)
                    }
                    UzButton { objectName: "consoleMemGo"; textFormat: Text.PlainText; text: qsTr("Go"); onClicked: root.dev.memoryGoto(gotoField.text) }
                    UzButton { objectName: "consoleMemUp"; textFormat: Text.PlainText; text: "▲"; onClicked: root.dev.memoryScroll(-16); Accessible.name: qsTr("Previous page") }
                    UzButton { objectName: "consoleMemDown"; textFormat: Text.PlainText; text: "▼"; onClicked: root.dev.memoryScroll(16); Accessible.name: qsTr("Next page") }
                }
                UzLabel {
                    objectName: "consoleMemError"
                    textFormat: Text.PlainText
                    visible: (root.mem.error || "") !== ""
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.danger
                    text: root.mem.error || ""
                }
                Repeater {
                    model: root.mem.rows || []
                    delegate: Row {
                        required property var modelData
                        objectName: "consoleMemRow"
                        spacing: Theme.space1
                        UzMono { textFormat: Text.PlainText; width: 84; text: modelData.addr; font.pixelSize: Theme.roles.meta11.size; color: Theme.muted }
                        // In the narrow game panel the 16 bytes wrap onto a second line instead of being cut off; the ASCII column needs room.
                        UzMono { objectName: "consoleMemHex"; textFormat: Text.PlainText; width: Math.min(16 * 22, col.width - 84 - Theme.space1); text: modelData.hex; wrapMode: Text.WordWrap; font.pixelSize: Theme.roles.meta11.size; color: Theme.ink }
                        UzMono { visible: col.width >= 84 + 16 * 22 + 16 * 8 + 2 * Theme.space1; textFormat: Text.PlainText; text: modelData.ascii; font.pixelSize: Theme.roles.meta11.size; color: Theme.dim }
                    }
                }
                Flow {
                    width: parent.width
                    spacing: Theme.space1
                    UzSelect {
                        id: searchKind
                        objectName: "consoleMemSearchKind"
                        width: 130
                        accessibleName: qsTr("Search for")
                        model: [{ value: "u8", label: qsTr("Value u8") }, { value: "u16", label: qsTr("Value u16") }, { value: "u32", label: qsTr("Value u32") }, { value: "bytes", label: qsTr("Bytes (hex)") }]
                        current: "u8"
                        onPicked: value => searchKind.current = value
                    }
                    UzField {
                        id: searchField
                        objectName: "consoleMemSearchField"
                        mono: true
                        width: 170
                        placeholderText: searchKind.current === "bytes" ? qsTr("46 42 4D 4D") : qsTr("42 or 0x2A")
                        onAccepted: searchBtn.clicked()
                    }
                    UzButton {
                        id: searchBtn
                        objectName: "consoleMemSearch"
                        textFormat: Text.PlainText
                        text: qsTr("Search region")
                        onClicked: root.dev.memorySearch(searchKind.current === "bytes" ? "bytes" : "value", searchField.text, searchKind.current)
                    }
                }
                UzLabel {
                    objectName: "consoleMemSearchError"
                    textFormat: Text.PlainText
                    visible: !!root.mem.search && (root.mem.search.error || "") !== ""
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.danger
                    text: root.mem.search ? (root.mem.search.error || "") : ""
                }
                UzLabel {
                    objectName: "consoleMemSearchNone"
                    textFormat: Text.PlainText
                    visible: !!root.mem.search && root.mem.search.active === true && (root.mem.search.error || "") === "" && root.mem.search.matches.length === 0
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.dim
                    text: qsTr("No matches in this region.")
                }
                Flow {
                    width: parent.width
                    spacing: Theme.space1
                    Repeater {
                        model: root.mem.search && root.mem.search.matches ? root.mem.search.matches.slice(0, 40) : []
                        delegate: UzChip {
                            required property var modelData
                            objectName: "consoleMemMatch"
                            text: modelData
                            onClicked: root.dev.memoryGoto(modelData)
                        }
                    }
                }
            }

            // ---- MEMORY MAP
            Column {
                width: parent.width
                spacing: Theme.space1
                Eyebrow { color: Theme.dim; text: qsTr("Memory map") }
                UzLabel {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    objectName: "consoleNoRegions"
                    visible: root.regions.length === 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.dim
                    text: root.con.regionsText || qsTr("The core exposes no memory map")
                }
                Repeater {
                    model: root.regions
                    delegate: Row {
                        required property var modelData
                        objectName: "consoleRegion"
                        spacing: Theme.space1
                        UzMono { textFormat: Text.PlainText; width: 92; text: modelData.start; font.pixelSize: Theme.roles.meta11.size; color: Theme.muted }
                        UzMono { textFormat: Text.PlainText; width: 92; text: "+" + modelData.length; font.pixelSize: Theme.roles.meta11.size; color: Theme.ink }
                        UzMono { textFormat: Text.PlainText; width: col.width - 92 - 92 - 16; text: modelData.addrspace !== "" ? modelData.addrspace : "-"; font.pixelSize: Theme.roles.meta11.size; color: Theme.dim; elide: Text.ElideRight }
                    }
                }
            }
        }
    }

    AnchoredPopover {
        id: selectPop
        objectName: "consoleSelectPopover"
        anchorItem: selectBtn
        popWidth: Math.min(300, root.width - 2 * Theme.space15)
        padding: Theme.space05
        align: "center"
        onCloseRequested: selectPop.open = false
        Column {
            width: parent.width
            Repeater {
                model: root.pluginList
                delegate: PopoverItem {
                    required property var modelData
                    objectName: "consolePlugin_" + modelData.id
                    width: parent.width
                    text: modelData.name
                    trailing: modelData.unpacked ? qsTr("Unpacked") + " · " + modelData.statusText : modelData.statusText
                    current: modelData.id === root.selected
                    onActivated: { root.ctl.selectConsolePlugin(modelData.id); selectPop.open = false }
                }
            }
        }
    }
}
