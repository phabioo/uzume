import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Saves mode of the Library detail column (3c-3, 3c-4): back row, slot tabs or switcher, CURRENT bar, snapshot / upload
// actions with inline confirmations, and the HISTORY timeline (SaveRow). Reuses SaveHistoryController for everything the
// Hub does; the column stays 392 wide and Play / Play and share Session stay pinned below (DetailPane).
//
// Also used inside the game (GamePanel, inGame: true): the game keeps running; snapshots, history, delete, restore and
// upload work live (they load the save into the running core and restart the game from it; upload not while the game is
// shared as a Session); slot changes (select, rename, delete) are blocked while the game runs. "Resolve conflict" opens the inline conflict resolution in both places (it never
// starts the game).
FocusScope {
    id: root
    required property PlayerController player
    property bool inGame: false              // shown inside the game panel (live)
    property string backTitle: ""            // "← <backTitle>"; default: the game title (Library)
    readonly property SaveHistoryController hist: player.saveHistory
    readonly property var game: player.selectedGame
    readonly property string syncKind: game.syncKind || "none"
    readonly property bool conflict: syncKind === "conflict"
    // Decision ad: the switcher above three slots; also whenever the tabs would not fit the column (wide fonts), so no slot is clipped.
    readonly property bool tabsCollapsed: hist.slotCount > 3 || (tabsFitProbe.minSum > 0 && tabs.width > 0 && tabsFitProbe.minSum + 8 + 28 > tabs.width)
    readonly property bool hasCurrent: hist.current.revision !== undefined
    readonly property bool uploadConfirming: hist.uploadRequest.path !== undefined
    readonly property bool uploadFailed: hist.uploadFailure.path !== undefined && !hist.uploading && !uploadConfirming
    property string filter: "all"            // "all" | "snapshots"
    property bool newSlotOpen: false
    property bool renameOpen: false
    property string renameError: ""
    readonly property bool slotDeleteConfirming: hist.slotDeleteRequest.slot !== undefined
    property bool snapshotOpen: false
    property bool detailsOpen: false
    property bool pathPromptOpen: false
    property string newSlotError: ""
    readonly property bool live: inGame && hist.liveApply   // restore / upload restart the running game from the save
    signal back()
    signal startRunRequested()

    objectName: "savesView"
    readonly property bool historyAvailable: hist.available   // Hub advertises saves_v2 (history, restore, snapshots)

    // Only the saves view inside the game may load a save into the running core; the Library view never touches it.
    onInGameChanged: if (root.hist) root.hist.liveMode = root.inGame
    Component.onCompleted: if (root.inGame) hist.liveMode = true
    Component.onDestruction: if (root.inGame && root.hist) root.hist.liveMode = false

    function cancelAll() {
        hist.closeConflict()
        hist.cancelRestore()
        hist.cancelDelete()
        hist.cancelUploadFile()
        hist.cancelDeleteSlot()
        hist.dismissUploadFailure()
        root.newSlotOpen = false
        root.renameOpen = false
        root.snapshotOpen = false
        root.detailsOpen = false
        root.pathPromptOpen = false
        root.filter = "all"
    }
    // "Upload save file...": native file dialog (SaveFileDialog.qml, QtQuick.Dialogs) when the module is installed,
    // otherwise a path field.
    function openUpload() {
        if (root.pathPromptOpen) {
            hist.requestUploadFile(uploadPath.text)
            return
        }
        fileDialogLoader.active = true
        if (fileDialogLoader.status === Loader.Ready && fileDialogLoader.item !== null) {
            fileDialogLoader.item.open()
        } else {
            root.pathPromptOpen = true
        }
    }
    function createSlot() {
        root.newSlotError = hist.createSlot(newSlotField.text)
        if (root.newSlotError === "") {
            newSlotField.text = ""
            root.newSlotOpen = false
        }
    }
    function renameSlot() {
        root.renameError = hist.renameSlot(renameField.text)
        if (root.renameError === "") {
            renameField.text = ""
            root.renameOpen = false
        }
    }
    function join(parts) { return parts.filter(p => p !== undefined && p !== "").join(" · ") }

    readonly property var rows: {
        const out = []
        const cur = hist.current
        if (cur.revision !== undefined) {
            out.push({ kind: "current", version: -1, versionText: cur.revisionText, label: "", isSnapshot: false,
                       meta: join([cur.when, cur.device, cur.reasonText]) })
        }
        for (const v of hist.history) {
            if (root.filter === "snapshots" && !v.isSnapshot) continue
            out.push({ kind: v.isSnapshot ? "snap" : "plain", version: v.version, versionText: v.versionText, label: v.label,
                       isSnapshot: v.isSnapshot, meta: join([v.when, v.device, v.reasonText]) })
        }
        return out
    }

    Keys.onEscapePressed: (event) => {
        if (hist.conflictOpen) hist.closeConflict()
        else if (hist.restoreRequest.version !== undefined) hist.cancelRestore()
        else if (hist.deleteRequest.version !== undefined) hist.cancelDelete()
        else if (root.uploadConfirming) hist.cancelUploadFile()
        else if (root.slotDeleteConfirming) hist.cancelDeleteSlot()
        else if (root.renameOpen) root.renameOpen = false
        else if (root.newSlotOpen) root.newSlotOpen = false
        else if (root.snapshotOpen) root.snapshotOpen = false
        else root.back()
        event.accepted = true
    }

    // One side of the conflict: who saved it, when, how big.
    component ConflictSide: Rectangle {
        id: side
        property string title: ""
        property var info: ({})
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: sideCol.implicitHeight + 20
        radius: Theme.radiusNone
        color: Theme.ground
        ColumnLayout {
            id: sideCol
            anchors.fill: parent
            anchors.margins: 10
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 4
            Eyebrow { text: side.title }
            UzLabel {
                objectName: side.objectName + "Device"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                text: side.info.device || qsTr("unknown device")
                font.pixelSize: Theme.roles.input.size
                font.weight: Font.Medium
            }
            UzLabel {
                objectName: side.objectName + "When"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                text: side.info.when || ""
                color: Theme.muted
                font.pixelSize: Theme.roles.small.size
            }
            UzMono {
                objectName: side.objectName + "Size"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                text: (side.info.revisionText ? side.info.revisionText + " · " : "") + (side.info.sizeText || "")
                color: Theme.ink
                font.pixelSize: Theme.roles.small.size
            }
        }
    }

    Loader {
        id: fileDialogLoader
        active: false
        source: "SaveFileDialog.qml"
        onLoaded: item.history = root.hist
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        // Back row
        Item {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: 28
            Item {
                id: backButton
                objectName: "savesBack"
                visible: root.inGame || root.backTitle !== ""   // in the Library the detail tabs lead back
                anchors.left: parent.left
                anchors.right: updated.visible ? updated.left : refresh.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                height: 28
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: backText.text
                Text {
                    id: backText
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    text: "← " + (root.backTitle !== "" ? root.backTitle : (root.game.title || qsTr("Library")))
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.roles.input.size
                    elide: Text.ElideRight
                    color: backHover.hovered || backButton.activeFocus ? Theme.ink : Theme.muted
                }
                HoverHandler { id: backHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.back() }
                Keys.onPressed: (event) => { if (event.key === Qt.Key_Space || event.key === Qt.Key_Return) { root.back(); event.accepted = true } }
            }
            UzMono {
                id: updated
                objectName: "savesUpdated"
                visible: root.hist.lastUpdated !== ""
                // Leaves the back link at least 90 px: wide fonts elide this text instead of squeezing the link away
                width: Math.min(implicitWidth, Math.max(0, parent.width - refresh.width - 16 - 90))
                elide: Text.ElideRight
                anchors.right: refresh.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("updated %1").arg(root.hist.lastUpdated)
                color: Theme.dim
                font.pixelSize: Theme.roles.meta11.size
            }
            Rectangle {
                id: refresh
                objectName: "historyRefresh"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 28
                height: 28
                radius: Theme.radiusNone
                color: refreshHover.hovered && enabled ? Theme.hover : "transparent"
                border.width: 1
                border.color: activeFocus ? Theme.accent : Theme.rule
                enabled: !root.hist.loading
                opacity: enabled ? 1 : 0.6
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Refresh from server")
                Text {
                    id: refreshGlyph
                    anchors.centerIn: parent
                    text: "↻"
                    font.pixelSize: 15
                    color: Theme.muted
                    RotationAnimation on rotation { running: root.hist.loading && !Theme.reduceMotion; from: 0; to: 360; duration: 900; loops: Animation.Infinite }
                }
                HoverHandler { id: refreshHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { enabled: refresh.enabled; onTapped: root.hist.refresh() }
                Keys.onPressed: (event) => { if (refresh.enabled && (event.key === Qt.Key_Space || event.key === Qt.Key_Return)) { root.hist.refresh(); event.accepted = true } }
                ToolTip.visible: refreshHover.hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Refresh from server")
            }
        }

        // Admins: "Show saves of: Mine / All users" (package 8, SavesAdminSwitch); "All users" opens a sheet over the window.
        SavesAdminSwitch {
            Layout.fillWidth: true
            visible: root.player.admin.available && !root.inGame && !root.player.localHub.onThisPcOnly  // no "All users" on this PC (8j-1)
            player: root.player
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            contentWidth: width
            contentHeight: body.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            ColumnLayout {
                id: body
                width: flick.width
                spacing: 14

                UzLabel {
                    objectName: "savesTitle"
                    visible: root.inGame
                    text: qsTr("Saves")
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                // The Library's saves tab (3c-3-r6): "Play from Main" is the primary under the title, "Start a run" sits on top.
                UzButton {
                    objectName: "savesStartRun"
                    visible: !root.inGame && root.player.runs.available && root.game.onHub !== false
                    size: "sm"
                    text: qsTr("Start a run")
                    onClicked: root.startRunRequested()
                }

                // Slot tabs, or the switcher when there are more than three slots
                Item {
                    id: tabs
                    objectName: "slotTabs"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: 30
                    readonly property bool changeable: !root.hist.gameRunning && !root.hist.busy
                    // Invisible measurement of the natural tab widths (independent of the collapsed state)
                    Item {
                        id: tabsFitProbe
                        visible: false
                        property real sum: 0
                        // Tabs shrink (elide) down to 48 px each before the row falls back to the switcher.
                        property real minSum: 0
                        property int count: 0
                        // "+ New slot" shrinks to a plain "+" when the full label does not fit beside the tabs.
                        readonly property bool linkCompact: sum + 8 + linkProbe.implicitWidth + 8 > tabs.width
                        Text {
                            id: linkProbe
                            visible: false
                            text: qsTr("+ New slot")
                            font.pixelSize: Theme.roles.input.size
                            font.weight: Font.Medium
                        }
                        Repeater {
                            model: root.hist.slotOptions
                            onCountChanged: Qt.callLater(tabsFitProbe.recalc)
                            delegate: Text {
                                required property var modelData
                                required property int index
                                textFormat: Text.StyledText
                                text: modelData.label + (modelData.count > 0 ? " <font color='" + Theme.dim + "'>" + modelData.count + "</font>" : "")
                                font.pixelSize: Theme.roles.input.size
                                font.weight: modelData.value === root.hist.slot ? Font.DemiBold : Font.Normal
                                onImplicitWidthChanged: tabsFitProbe.recalc()
                                Component.onCompleted: tabsFitProbe.recalc()
                            }
                        }
                        function recalc() {
                            let w = 0, m = 0, n = 0
                            for (let i = 0; i < children.length; ++i) {
                                const c = children[i]
                                if (c !== linkProbe && c.implicitWidth !== undefined && c.text !== undefined) { w += Math.min(c.implicitWidth, 90); m += Math.min(c.implicitWidth, 48); ++n }
                            }
                            sum = w + Math.max(0, n - 1) * 12
                            minSum = m + Math.max(0, n - 1) * 12
                            count = n
                        }
                    }
                    Row {
                        id: tabRow
                        visible: !root.tabsCollapsed
                        anchors.left: parent.left
                        anchors.right: newSlotLink.left
                        anchors.rightMargin: 8
                        height: parent.height
                        spacing: 12
                        clip: true
                        Repeater {
                            model: root.tabsCollapsed ? [] : root.hist.slotOptions
                            delegate: Item {
                                id: tab
                                required property var modelData
                                readonly property bool active: modelData.value === root.hist.slot
                                objectName: "slotTab_" + modelData.value
                                readonly property string text: tabText.text
                                height: tabRow.height
                                width: Math.min(tabText.implicitWidth, 90, Math.max(48, (tabRow.width - Math.max(0, tabsFitProbe.count - 1) * tabRow.spacing) / Math.max(1, tabsFitProbe.count)))
                                opacity: tabs.changeable || active ? 1 : 0.5
                                activeFocusOnTab: true
                                Accessible.role: Accessible.PageTab
                                Accessible.name: tabText.text
                                Text {
                                    id: tabText
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width
                                    elide: Text.ElideRight
                                    textFormat: Text.StyledText
                                    text: tab.modelData.label + (tab.modelData.count > 0 ? " <font color='" + Theme.dim + "'>" + tab.modelData.count + "</font>" : "")
                                    font.pixelSize: Theme.roles.input.size
                                    font.weight: tab.active ? Font.DemiBold : Font.Normal
                                    color: tab.active ? Theme.ink : Theme.muted
                                }
                                Rectangle {
                                    visible: tab.active
                                    anchors.bottom: parent.bottom
                                    width: parent.width
                                    height: 2
                                    color: Theme.accent
                                }
                                HoverHandler { id: tabHover; cursorShape: Qt.PointingHandCursor }
                                // The tab elides long slot names: the full name is the tooltip (R-P12).
                                // A run's slot shows as "Run: <name>"; the raw slot name is the secondary text.
                                readonly property bool renamed: tab.modelData.value !== "default" && tab.modelData.label !== tab.modelData.value
                                ToolTip.visible: tabHover.hovered && (tabText.truncated || tab.renamed)
                                ToolTip.text: tab.renamed ? tab.modelData.label + " (" + tab.modelData.value + ")" : tab.modelData.label
                                ToolTip.delay: 400
                                TapHandler { enabled: tabs.changeable && !tab.active; onTapped: root.hist.selectSlot(tab.modelData.value) }
                                Keys.onPressed: (event) => {
                                    if (tabs.changeable && !tab.active && (event.key === Qt.Key_Space || event.key === Qt.Key_Return)) {
                                        root.hist.selectSlot(tab.modelData.value)
                                        event.accepted = true
                                    }
                                }
                            }
                        }
                    }
                    // Switcher "Main · 8 ▾" (more than three slots); "+ New slot" is its last item
                    Rectangle {
                        id: switcher
                        objectName: "slotSwitcher"
                        visible: root.tabsCollapsed
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        height: 30
                        width: Math.min(switcherText.implicitWidth + 36, parent.width)
                        radius: Theme.radiusNone
                        color: Theme.surface
                        border.width: activeFocus || switchMenu.visible ? 1.5 : 1
                        border.color: activeFocus || switchMenu.visible ? Theme.accent : Theme.rule
                        opacity: tabs.changeable ? 1 : 0.6
                        activeFocusOnTab: true
                        Accessible.role: Accessible.ComboBox
                        Accessible.name: qsTr("Save slot: %1").arg(switcherText.text)
                        Text {
                            id: switcherText
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(implicitWidth, switcher.width - 36)
                            elide: Text.ElideRight
                            text: {
                                let count = 0
                                for (const o of root.hist.slotOptions) if (o.value === root.hist.slot) count = o.count
                                return root.hist.slotLabel + (count > 0 ? " · " + count : "")
                            }
                            font.pixelSize: Theme.roles.input.size
                            font.weight: Font.DemiBold
                            color: Theme.ink
                        }
                        Text { anchors.right: parent.right; anchors.rightMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: switchMenu.visible ? "▴" : "▾"; font.pixelSize: Theme.roles.meta11.size; color: Theme.dim }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler { enabled: tabs.changeable; onTapped: { switcher.forceActiveFocus(); switchMenu.visible ? switchMenu.close() : switchMenu.open() } }
                        Keys.onPressed: (event) => { if (tabs.changeable && (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Down)) { switchMenu.open(); event.accepted = true } }
                        Popup {
                            id: switchMenu
                            objectName: "slotSwitchMenu"
                            parent: switcher
                            y: switcher.height + 4
                            width: Math.max(220, switcher.width)
                            padding: 4
                            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                            background: Rectangle { radius: Theme.radiusNone; color: Theme.surface; border.width: 1; border.color: Theme.rule }
                            // A plain Column (no Layout): the model can change while the popup resizes without touching stale layout items.
                            contentItem: Column {
                                spacing: 0
                                Repeater {
                                    model: root.hist.slotOptions
                                    delegate: ItemDelegate {
                                        id: slotItem
                                        required property var modelData
                                        objectName: "slotMenu_" + modelData.value
                                        width: switchMenu.availableWidth
                                        implicitHeight: 30
                                        padding: 0
                                        leftPadding: 8
                                        rightPadding: 8
                                        onClicked: { const slotValue = slotItem.modelData.value; switchMenu.close(); root.hist.selectSlot(slotValue) }
                                        contentItem: Row {
                                            spacing: 4
                                            Text { width: 16; anchors.verticalCenter: parent.verticalCenter; text: slotItem.modelData.value === root.hist.slot ? "✓" : ""; font.pixelSize: Theme.roles.input.size; color: Theme.accentText }
                                            Text {
                                                anchors.verticalCenter: parent.verticalCenter
                                                width: slotItem.width - 16 - 4 - 16
                                                elide: Text.ElideRight
                                                text: slotItem.modelData.label + (slotItem.modelData.count > 0 ? " · " + slotItem.modelData.count : "")
                                                textFormat: Text.PlainText
                                                font.pixelSize: Theme.roles.input.size
                                                color: Theme.ink
                                            }
                                        }
                                        background: Rectangle { radius: Theme.radiusNone; color: slotItem.highlighted || slotItem.hovered ? Theme.hover : "transparent" }
                                    }
                                }
                                Item { width: switchMenu.availableWidth; height: 9; Rectangle { anchors.centerIn: parent; width: parent.width; height: 1; color: Theme.rule } }
                                ItemDelegate {
                                    id: newItem
                                    objectName: "slotMenuNew"
                                    width: switchMenu.availableWidth
                                    implicitHeight: 30
                                    padding: 0
                                    leftPadding: 24
                                    text: qsTr("+ New slot")
                                    font.pixelSize: Theme.roles.input.size
                                    font.weight: Font.Medium
                                    onClicked: { switchMenu.close(); root.newSlotError = ""; root.newSlotOpen = true; Qt.callLater(() => newSlotField.forceActiveFocus()) }
                                    // The Basic ItemDelegate text follows the palette (dark text on the dark popup looked disabled, R-P16).
                                    contentItem: Text {
                                        text: newItem.text
                                        textFormat: Text.PlainText
                                        font: newItem.font
                                        color: Theme.ink
                                        verticalAlignment: Text.AlignVCenter
                                        elide: Text.ElideRight
                                    }
                                    background: Rectangle { radius: Theme.radiusNone; color: newItem.highlighted || newItem.hovered ? Theme.hover : "transparent" }
                                }
                            }
                        }
                    }
                    UzButton {
                        id: newSlotLink
                        objectName: "newSlotButton"
                        visible: !root.tabsCollapsed
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        kind: "link"
                        implicitHeight: 28
                        font.pixelSize: Theme.roles.input.size
                        font.weight: Font.Medium
                        text: tabsFitProbe.linkCompact ? "+" : qsTr("+ New slot")
                        Accessible.name: qsTr("New slot")
                        enabled: !root.hist.gameRunning
                        onClicked: {
                            root.newSlotError = ""
                            root.newSlotOpen = !root.newSlotOpen
                            if (root.newSlotOpen) newSlotField.forceActiveFocus()
                        }
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule; z: -1 }
                }

                // New slot (inline)
                ColumnLayout {
                    objectName: "newSlotRow"
                    visible: root.newSlotOpen
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 8
                    UzField {
                        id: newSlotField
                        objectName: "newSlotField"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        implicitHeight: 34
                        placeholderText: qsTr("slot-name")
                        maximumLength: 64
                        onTextChanged: root.newSlotError = ""
                        onAccepted: root.createSlot()
                    }
                    UzLabel {
                        objectName: "newSlotError"
                        visible: root.newSlotError !== ""
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.newSlotError
                        color: Theme.danger
                        font.pixelSize: Theme.roles.small.size
                    }
                    UzLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("A new slot starts without a save. “%1” is not changed.").arg(root.hist.slotLabel)
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                    }
                    Flow {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 8
                        layoutDirection: Qt.RightToLeft   // right-aligned, wraps under the first button when the column is narrow
                        UzButton { objectName: "newSlotCreate"; kind: "primary"; implicitHeight: 32; font.pixelSize: Theme.roles.input.size; text: qsTr("Create slot"); onClicked: root.createSlot() }
                        UzButton { objectName: "newSlotCancel"; implicitHeight: 32; font.pixelSize: Theme.roles.input.size; text: qsTr("Cancel"); onClicked: root.newSlotOpen = false }
                    }
                }

                // Rename / delete the selected slot (saves_v5; never the main slot)
                Flow {
                    objectName: "slotManageRow"
                    visible: root.hist.slotDeleteAvailable && !root.newSlotOpen && !root.renameOpen && !root.slotDeleteConfirming
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 8
                    UzButton {
                        objectName: "slotRenameButton"
                        visible: root.hist.slotManageAvailable
                        kind: "link"
                        implicitHeight: 28
                        font.pixelSize: Theme.roles.input.size
                        font.weight: Font.Medium
                        text: qsTr("Rename slot")
                        enabled: root.hist.slotManageBlockReason === ""
                        onClicked: { root.renameError = ""; renameField.text = root.hist.slot; root.renameOpen = true; Qt.callLater(() => { renameField.forceActiveFocus(); renameField.selectAll() }) }
                    }
                    UzButton {
                        objectName: "slotDeleteButton"
                        kind: "link"
                        implicitHeight: 28
                        font.pixelSize: Theme.roles.input.size
                        font.weight: Font.Medium
                        text: qsTr("Delete slot…")
                        enabled: root.hist.slotManageBlockReason === ""
                        onClicked: root.hist.requestDeleteSlot()
                    }
                }
                UzLabel {
                    objectName: "slotManageHint"
                    visible: root.hist.slotDeleteAvailable && root.hist.slotManageBlockReason !== "" && !root.newSlotOpen && !root.renameOpen && !root.slotDeleteConfirming
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.hist.slotManageBlockReason
                    color: Theme.dim
                    font.pixelSize: Theme.roles.small.size
                }

                // Rename slot (inline)
                ColumnLayout {
                    objectName: "renameSlotRow"
                    visible: root.renameOpen
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 8
                    UzField {
                        id: renameField
                        objectName: "renameSlotField"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        implicitHeight: 34
                        placeholderText: qsTr("new-slot-name")
                        maximumLength: 64
                        onTextChanged: root.renameError = ""
                        onAccepted: root.renameSlot()
                    }
                    UzLabel {
                        objectName: "renameSlotError"
                        visible: root.renameError !== ""
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.renameError
                        color: Theme.danger
                        font.pixelSize: Theme.roles.small.size
                    }
                    UzLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("The save and its history keep their content. Your other devices pick up the new name.")
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                    }
                    Flow {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 8
                        layoutDirection: Qt.RightToLeft   // right-aligned, wraps under the first button when the column is narrow
                        UzButton { objectName: "renameSlotConfirm"; kind: "primary"; implicitHeight: 32; font.pixelSize: Theme.roles.input.size; text: qsTr("Rename slot"); onClicked: root.renameSlot() }
                        UzButton { objectName: "renameSlotCancel"; implicitHeight: 32; font.pixelSize: Theme.roles.input.size; text: qsTr("Cancel"); onClicked: root.renameOpen = false }
                    }
                }

                // Delete slot confirmation: names the latest checkpoint
                Rectangle {
                    objectName: "slotDeleteBox"
                    visible: root.slotDeleteConfirming
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: slotDeleteCol.implicitHeight + 28
                    radius: Theme.radiusNone
                    color: Theme.dangerTint
                    border.width: 1
                    border.color: Theme.danger
                    ColumnLayout {
                        id: slotDeleteCol
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 10
                        UzLabel {
                            objectName: "slotDeleteTitle"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Delete slot “%1”?").arg(root.hist.slotDeleteRequest.slotLabel || "")
                            color: Theme.danger
                            font.pixelSize: Theme.roles.body.size
                            font.weight: Font.DemiBold
                        }
                        UzLabel {
                            objectName: "slotDeleteLatest"
                            visible: root.hist.slotDeleteRequest.onHub === true
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Latest save: %1").arg(root.join([root.hist.slotDeleteRequest.revisionText, root.hist.slotDeleteRequest.when, root.hist.slotDeleteRequest.device]))
                            color: Theme.ink
                            font.pixelSize: Theme.roles.input.size
                        }
                        UzLabel {
                            objectName: "slotDeleteNote"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: root.hist.slotDeleteRequest.onHub === true
                                  ? qsTr("The slot is deleted for good on the server with its whole history (%1 versions), and the copy on this device is removed. This cannot be undone. If the save changed since this page was shown, nothing is deleted.").arg(root.hist.slotDeleteRequest.versionCount || 0)
                                  : (root.hist.slotDeleteRequest.isMain === true
                                     ? qsTr("The main slot has no save on the server. Its local save is removed. This cannot be undone.")
                                     : qsTr("This slot exists only on this device. Its local save is removed. This cannot be undone."))
                            color: Theme.ink2
                            font.pixelSize: Theme.roles.small.size
                        }
                        Flow {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            layoutDirection: Qt.RightToLeft
                            Rectangle {
                                objectName: "slotDeleteConfirm"
                                height: 32
                                width: Math.min(slotDeleteOkText.implicitWidth + 24, parent.width)   // never wider than the row, the text elides
                                radius: Theme.radiusNone
                                color: Theme.danger
                                opacity: slotDeleteOkTap.pressed ? 0.8 : 1
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: slotDeleteOkText.text
                                Text { id: slotDeleteOkText; anchors.centerIn: parent; width: Math.min(implicitWidth, parent.width - 24); elide: Text.ElideRight; text: qsTr("Delete slot"); font.pixelSize: Theme.roles.input.size; font.weight: Font.DemiBold; color: Theme.inkOnAccent }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { id: slotDeleteOkTap; onTapped: root.hist.confirmDeleteSlot() }
                                Keys.onPressed: (event) => { if (event.key === Qt.Key_Space || event.key === Qt.Key_Return) { root.hist.confirmDeleteSlot(); event.accepted = true } }
                            }
                            UzButton { objectName: "slotDeleteCancel"; implicitHeight: 32; font.pixelSize: Theme.roles.input.size; text: qsTr("Cancel"); onClicked: root.hist.cancelDeleteSlot() }
                        }
                    }
                }

                // Banners: Hub offline, game running, save conflict, pending sync
                Rectangle {
                    objectName: "savesOfflineBanner"
                    visible: !root.hist.online
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: offlineCol.implicitHeight + 20
                    radius: Theme.radiusNone
                    color: Theme.selected
                    ColumnLayout {
                        id: offlineCol
                        anchors.fill: parent
                        anchors.margins: 10
                        anchors.leftMargin: 12
                        spacing: 6
                        UzLabel {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: root.hist.lastSynced !== ""
                                  ? qsTr("Server offline · read-only. Showing the history from the last sync, %1. Restore, snapshots and uploads need the server.").arg(root.hist.lastSynced)
                                  : qsTr("Server offline · read-only. Restore, snapshots and uploads need the server.")
                            color: Theme.accentText
                            font.pixelSize: Theme.roles.small.size
                        }
                        UzButton { objectName: "savesTryAgain"; kind: "link"; implicitHeight: 24; font.pixelSize: Theme.roles.small.size; text: qsTr("Try again"); onClicked: root.hist.refresh() }
                    }
                }
                Rectangle {
                    objectName: "savesRunningBanner"
                    visible: root.hist.online && root.hist.gameRunning
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: runningText.implicitHeight + 20
                    radius: Theme.radiusNone
                    color: Theme.selected
                    UzLabel {
                        id: runningText
                        anchors.fill: parent
                        anchors.margins: 10
                        anchors.leftMargin: 12
                        wrapMode: Text.WordWrap
                        text: root.live ? qsTr("%1 keeps running. Snapshots and delete work live. Restore and upload restart it from that save. Rename and delete of slots are possible after the game is closed.").arg(root.game.title || "")
                                        : root.inGame ? qsTr("%1 keeps running. Snapshots still work. Restore, upload and slot changes (rename, delete) are possible after the game is closed.").arg(root.game.title || "")
                                                      : qsTr("%1 is running on this device. Restore and upload are possible after the game is closed. Snapshots still work.").arg(root.game.title || "")
                        color: Theme.accentText
                        font.pixelSize: Theme.roles.small.size
                    }
                }
                Rectangle {
                    objectName: "savesConflictBanner"
                    visible: root.hist.online && root.conflict && !root.hist.conflictOpen
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: conflictCol.implicitHeight + 24
                    radius: Theme.radiusNone
                    color: Theme.selected
                    border.width: 1
                    border.color: Theme.rule
                    ColumnLayout {
                        id: conflictCol
                        anchors.fill: parent
                        anchors.margins: 12
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        spacing: 8
                        Eyebrow { Layout.fillWidth: true; text: qsTr("▲ Save conflict · %1").arg(root.hist.slotLabel); color: Theme.accentText }
                        UzLabel {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Restore, upload and new slot are paused for “%1” until the conflict is resolved. Other slots work normally.").arg(root.hist.slotLabel)
                            color: Theme.ink2
                            font.pixelSize: Theme.roles.small.size
                        }
                        UzButton {
                            objectName: "resolveConflictButton"
                            implicitHeight: 32
                            font.pixelSize: Theme.roles.input.size
                            text: qsTr("Resolve conflict")
                            enabled: !root.hist.confirmationOpen && !root.hist.busy
                            onClicked: root.hist.openConflict()
                        }
                        UzLabel {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Compare both versions here and choose one. The game does not start.")
                            color: Theme.dim
                            font.pixelSize: Theme.roles.small.size
                        }
                    }
                }
                // Inline conflict resolution: this device vs the Hub
                Rectangle {
                    objectName: "conflictBox"
                    visible: root.hist.conflictOpen
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: resolveCol.implicitHeight + 28
                    radius: Theme.radiusNone
                    color: Theme.selected
                    border.width: 1
                    border.color: Theme.rule
                    ColumnLayout {
                        id: resolveCol
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 10
                        Eyebrow { Layout.fillWidth: true; text: qsTr("▲ Resolve conflict · %1").arg(root.hist.slotLabel); color: Theme.accentText }
                        RowLayout {
                            objectName: "conflictLoading"
                            visible: root.hist.conflictLoading
                            Layout.fillWidth: true
                            spacing: 8
                            UzSpinner { size: 14 }
                            UzLabel {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                elide: Text.ElideRight
                                text: qsTr("Loading both versions…")
                                color: Theme.ink2
                                font.pixelSize: Theme.roles.small.size
                            }
                        }
                        UzLabel {
                            visible: !root.hist.conflictLoading
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("This device and the server both changed “%1”. Choose which save becomes the current one. Nothing is deleted.").arg(root.hist.slotLabel)
                            color: Theme.ink2
                            font.pixelSize: Theme.roles.small.size
                        }
                        ConflictSide {
                            objectName: "conflictLocal"
                            visible: !root.hist.conflictLoading
                            title: qsTr("This device")
                            info: root.hist.conflictInfo.local || ({})
                        }
                        ConflictSide {
                            objectName: "conflictHub"
                            visible: !root.hist.conflictLoading
                            title: qsTr("Server")
                            info: root.hist.conflictInfo.hub || ({})
                        }
                        UzLabel {
                            visible: !root.hist.conflictLoading
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: root.inGame
                                  ? qsTr("“Use the server save” saves this device's copy as a local backup first and restarts the game from the server save. “Keep this device's save” uploads it as the new current version; the game keeps running.")
                                  : qsTr("“Use the server save” saves this device's copy as a local backup first. “Keep this device's save” uploads it as the new current version. The game does not start.")
                            color: Theme.ink2
                            font.pixelSize: Theme.roles.small.size
                        }
                        Flow {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            layoutDirection: Qt.RightToLeft
                            UzButton {
                                objectName: "conflictKeepLocal"
                                kind: "primary"
                                implicitHeight: 32
                                font.pixelSize: Theme.roles.input.size
                                text: qsTr("Keep this device's save")
                                enabled: !root.hist.conflictLoading && !root.hist.conflictBusy && (root.hist.conflictInfo.local || ({})).exists === true
                                onClicked: root.hist.resolveConflict("use_local")
                            }
                            UzButton {
                                objectName: "conflictUseHub"
                                implicitHeight: 32
                                font.pixelSize: Theme.roles.input.size
                                text: qsTr("Use the server save")
                                enabled: !root.hist.conflictLoading && !root.hist.conflictBusy
                                onClicked: root.hist.resolveConflict("use_hub")
                            }
                            UzButton {
                                objectName: "conflictCancel"
                                implicitHeight: 32
                                font.pixelSize: Theme.roles.input.size
                                text: qsTr("Cancel")
                                enabled: !root.hist.conflictBusy
                                onClicked: root.hist.closeConflict()
                            }
                        }
                    }
                }
                UzLabel {
                    objectName: "restoreBlockReason"
                    visible: root.hist.online && !root.hist.gameRunning && !root.conflict && root.hist.restoreBlockReason !== ""
                             && (root.hist.history.length > 0 || root.hist.canUploadFile)
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Restore and upload are off: %1").arg(root.hist.restoreBlockReason)
                    color: Theme.dim
                    font.pixelSize: Theme.roles.small.size
                }

                // CURRENT bar
                Rectangle {
                    objectName: "currentBar"
                    visible: root.hasCurrent
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: currentCol.implicitHeight + 20
                    radius: Theme.radiusNone
                    color: Theme.surface
                    border.width: 1
                    border.color: Theme.rule
                    ColumnLayout {
                        id: currentCol
                        anchors.fill: parent
                        anchors.topMargin: 10
                        anchors.bottomMargin: 10
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 6
                        RowLayout {
                            id: currentHeader
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.maximumWidth: currentCol.width
                            spacing: 8
                            Eyebrow { text: qsTr("Current") }
                            UzMono {
                                objectName: "currentVersion"
                                // Takes the free space and elides, so a wide font or a long pill never pushes the row past the column
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                elide: Text.ElideRight
                                text: root.hist.current.revisionText || ""
                                color: Theme.ink
                                font.pixelSize: Theme.roles.input.size
                                font.weight: Font.Medium
                            }
                            UzTag {
                                objectName: "currentPill"
                                // The pill elides its text when squeezed; it keeps at most 60% of the row
                                Layout.maximumWidth: Math.max(48, currentHeader.width * 0.6)
                                visible: text !== ""
                                small: true
                                tone: !root.hist.online ? "neutral" : (root.syncKind === "pending" ? "warn" : "ok")
                                text: !root.hist.online ? qsTr("Offline")
                                      : root.syncKind === "pending" ? qsTr("⟳ Sync pending")
                                      : root.syncKind === "synced" ? qsTr("✓ Synced") : ""
                            }
                        }
                        UzLabel {
                            objectName: "currentMeta"
                            Layout.fillWidth: true
                            Layout.maximumWidth: currentCol.width
                            wrapMode: Text.WordWrap
                            text: root.join([root.hist.current.when, root.hist.current.device, root.hist.current.reasonText])
                            color: Theme.muted
                            font.pixelSize: Theme.roles.small.size
                        }
                        Item {
                            id: detailsToggle
                            objectName: "fileDetailsToggle"
                            Layout.preferredHeight: 22
                            Layout.preferredWidth: detailsToggleText.implicitWidth
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: detailsToggleText.text
                            Text {
                                id: detailsToggleText
                                anchors.verticalCenter: parent.verticalCenter
                                text: root.detailsOpen ? qsTr("File details ▾") : qsTr("File details ▸")
                                font.pixelSize: Theme.roles.small.size
                                color: Theme.muted
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.detailsOpen = !root.detailsOpen }
                            Keys.onPressed: (event) => { if (event.key === Qt.Key_Space || event.key === Qt.Key_Return) { root.detailsOpen = !root.detailsOpen; event.accepted = true } }
                        }
                        GridLayout {
                            objectName: "fileDetails"
                            visible: root.detailsOpen
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredWidth: 1
                            Layout.maximumWidth: parent.width
                            // Label above value when the widest label would leave the value too little room (wide fonts)
                            columns: detailLabelProbe.implicitWidth > parent.width * 0.4 ? 1 : 2
                            columnSpacing: 12
                            rowSpacing: 6
                            UzLabel { Layout.minimumWidth: 0; text: qsTr("SHA-256"); color: Theme.muted; font.pixelSize: Theme.roles.small.size; Layout.alignment: Qt.AlignTop }
                            UzMono { objectName: "detailSha"; Layout.fillWidth: true; Layout.minimumWidth: 0; text: root.hist.current.shaShort || ""; color: Theme.ink; font.pixelSize: Theme.roles.small.size; wrapMode: Text.WrapAnywhere }
                            UzLabel { Layout.minimumWidth: 0; text: qsTr("Size"); color: Theme.muted; font.pixelSize: Theme.roles.small.size; Layout.alignment: Qt.AlignTop }
                            UzMono { objectName: "detailSize"; Layout.fillWidth: true; Layout.minimumWidth: 0; text: root.hist.current.sizeText || ""; color: Theme.ink; font.pixelSize: Theme.roles.small.size }
                            UzLabel { Layout.minimumWidth: 0; id: detailLabelProbe; text: qsTr("Local path"); color: Theme.muted; font.pixelSize: Theme.roles.small.size; Layout.alignment: Qt.AlignTop }
                            UzMono { objectName: "detailPath"; Layout.fillWidth: true; Layout.minimumWidth: 0; text: root.hist.current.localPath || ""; color: Theme.ink; font.pixelSize: Theme.roles.small.size; wrapMode: Text.WrapAnywhere }
                            Item { visible: parent.columns === 2; Layout.preferredWidth: 1; Layout.preferredHeight: 1 }
                            Flow {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 8
                                UzButton { objectName: "copyHashButton"; implicitHeight: 28; font.pixelSize: Theme.roles.small.size; text: qsTr("Copy hash"); onClicked: root.hist.copyText(root.hist.current.sha256 || "") }
                                UzButton { objectName: "openFolderButton"; implicitHeight: 28; font.pixelSize: Theme.roles.small.size; text: qsTr("Open folder"); onClicked: root.hist.openSaveFolder() }
                            }
                        }
                    }
                }

                // Message of the last action (snapshot created, restored, deleted, errors)
                Rectangle {
                    objectName: "historyMessage"
                    visible: root.hist.message !== "" && !root.uploadFailed
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: msgRow.implicitHeight + 16
                    radius: Theme.radiusNone
                    color: root.hist.messageIsError ? Theme.dangerTint : Theme.ground
                    RowLayout {
                        id: msgRow
                        anchors.fill: parent
                        anchors.margins: 8
                        UzLabel {
                            objectName: "historyMessageText"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            wrapMode: Text.WordWrap
                            text: (root.hist.messageIsError ? "" : "✓ ") + root.hist.message
                            font.pixelSize: Theme.roles.small.size
                            color: root.hist.messageIsError ? Theme.danger : Theme.ok
                        }
                        UzButton { kind: "link"; text: qsTr("Dismiss"); onClicked: root.hist.dismissMessage() }
                    }
                }

                // Actions
                Flow {
                    objectName: "saveActions"
                    visible: root.historyAvailable && !root.snapshotOpen && !root.uploadConfirming && !root.hist.uploading && !root.uploadFailed
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    id: saveActionsFlow
                    spacing: 8
                    // Wide fonts: the "◆ " mark is dropped when the full label does not fit the column (never elide the label).
                    TextMetrics { id: snapshotFull; font.pixelSize: Theme.roles.input.size; font.weight: Font.Medium; text: qsTr("◆ Create snapshot") }
                    UzButton {
                        objectName: "snapshotButton"
                        implicitHeight: 32
                        font.pixelSize: Theme.roles.input.size
                        text: snapshotFull.advanceWidth + 32 + 4 <= saveActionsFlow.width ? snapshotFull.text : qsTr("Create snapshot")
                        enabled: root.hist.canSnapshot && root.hist.online
                        onClicked: { root.snapshotOpen = true; Qt.callLater(() => snapshotLabel.forceActiveFocus()) }
                    }
                    UzButton {
                        objectName: "uploadSaveButton"
                        visible: root.hist.canUploadFile || !root.hist.online
                        kind: "link"
                        implicitHeight: 32
                        font.pixelSize: Theme.roles.input.size
                        font.weight: Font.Medium
                        text: root.pathPromptOpen ? qsTr("Choose") : qsTr("Upload save file…")
                        enabled: root.hist.restoreBlockReason === "" && !root.hist.busy && root.hist.online && root.hist.canUploadFile && root.hist.uploadBlockReason === ""
                        onClicked: root.openUpload()
                    }
                }
                UzLabel {
                    objectName: "uploadBlockHint"
                    visible: root.hist.online && root.hist.uploadBlockReason !== ""
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: root.hist.uploadBlockReason
                    color: Theme.dim
                    font.pixelSize: Theme.roles.small.size
                }
                UzField {
                    id: uploadPath
                    objectName: "uploadPathField"
                    visible: root.pathPromptOpen && !root.uploadConfirming
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    implicitHeight: 34
                    placeholderText: qsTr("Path to the save file")
                    onAccepted: root.hist.requestUploadFile(text)
                }

                // Create snapshot (inline)
                Rectangle {
                    objectName: "snapshotForm"
                    visible: root.snapshotOpen
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: snapCol.implicitHeight + 28
                    radius: Theme.radiusNone
                    color: Theme.selected
                    border.width: 1
                    border.color: Theme.rule
                    ColumnLayout {
                        id: snapCol
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 10
                        UzLabel {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: root.hist.current.revisionText ? qsTr("◆ Create snapshot of %1").arg(root.hist.current.revisionText) : qsTr("◆ Create snapshot")
                            color: Theme.accentText
                            font.pixelSize: Theme.roles.input.size
                            font.weight: Font.DemiBold
                        }
                        UzField {
                            id: snapshotLabel
                            objectName: "snapshotLabel"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredWidth: 1
                            Layout.maximumWidth: parent.width
                            implicitHeight: 34
                            placeholderText: qsTr("Label (optional)")
                            maximumLength: 64
                            onAccepted: snapshotCreate.clicked()
                        }
                        UzLabel {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Label is optional. Snapshots are kept until you delete them.")
                            color: Theme.ink2
                            font.pixelSize: Theme.roles.small.size
                        }
                        Flow {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            layoutDirection: Qt.RightToLeft   // right-aligned, wraps under the first button when narrow
                            UzButton {
                                id: snapshotCreate
                                objectName: "snapshotCreate"
                                kind: "primary"
                                implicitHeight: 30
                                font.pixelSize: Theme.roles.input.size
                                text: qsTr("Create snapshot")
                                enabled: root.hist.canSnapshot
                                onClicked: {
                                    root.hist.createSnapshot(snapshotLabel.text)
                                    snapshotLabel.text = ""
                                    root.snapshotOpen = false
                                }
                            }
                            UzButton { objectName: "snapshotCancel"; implicitHeight: 30; font.pixelSize: Theme.roles.input.size; text: qsTr("Cancel"); onClicked: { snapshotLabel.text = ""; root.snapshotOpen = false } }
                        }
                    }
                }

                // Upload confirmation (3c-4)
                Rectangle {
                    objectName: "uploadConfirmBox"
                    visible: root.uploadConfirming
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: uploadCol.implicitHeight + 28
                    radius: Theme.radiusNone
                    color: Theme.selected
                    border.width: 1
                    border.color: Theme.rule
                    ColumnLayout {
                        id: uploadCol
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 12
                        UzLabel {
                            objectName: "uploadConfirmTitle"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Upload as the new current version?")
                            color: Theme.accentText
                            font.pixelSize: Theme.roles.body.size
                            font.weight: Font.DemiBold
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            implicitHeight: dataGrid.implicitHeight + 20
                            radius: Theme.radiusNone
                            color: Theme.ground
                            GridLayout {
                                id: dataGrid
                                anchors.fill: parent
                                anchors.margins: 10
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                columns: 2
                                columnSpacing: 14
                                rowSpacing: 6
                                UzLabel { text: qsTr("File"); color: Theme.muted; font.pixelSize: Theme.roles.input.size }
                                UzMono { objectName: "uploadFileName"; Layout.fillWidth: true; text: root.hist.uploadRequest.fileName || ""; color: Theme.ink; font.pixelSize: Theme.roles.input.size; elide: Text.ElideRight }
                                UzLabel { text: qsTr("Size"); color: Theme.muted; font.pixelSize: Theme.roles.input.size }
                                UzMono { objectName: "uploadSize"; Layout.fillWidth: true; text: root.hist.uploadRequest.sizeText || ""; color: Theme.ink; font.pixelSize: Theme.roles.input.size; elide: Text.ElideRight }
                                UzLabel { text: qsTr("Into slot"); color: Theme.muted; font.pixelSize: Theme.roles.input.size }
                                UzLabel {
                                    objectName: "uploadSlot"
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: root.hist.uploadRequest.currentText ? qsTr("%1 · becomes %2").arg(root.hist.slotLabel).arg(root.hist.uploadRequest.nextText || "")
                                                                               : qsTr("%1 · first save").arg(root.hist.slotLabel)
                                    font.pixelSize: Theme.roles.input.size
                                }
                            }
                        }
                        UzLabel {
                            objectName: "uploadConfirmNote"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: (root.hist.uploadRequest.currentText
                                   ? qsTr("The current version is kept in the history as “Before upload”. A local backup is made first.")
                                   : qsTr("A local backup is made first."))
                                  + (root.live ? "\n" + qsTr("The game restarts from this save.") : "")
                            color: Theme.ink2
                            font.pixelSize: Theme.roles.small.size
                        }
                        Flow {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            layoutDirection: Qt.RightToLeft
                            UzButton { objectName: "uploadConfirm"; kind: "primary"; implicitHeight: 32; font.pixelSize: Theme.roles.input.size; text: qsTr("Upload as new current"); onClicked: root.hist.confirmUploadFile() }
                            UzButton { objectName: "uploadCancel"; implicitHeight: 32; font.pixelSize: Theme.roles.input.size; text: qsTr("Cancel"); onClicked: root.hist.cancelUploadFile() }
                        }
                    }
                }

                // Upload in progress
                Rectangle {
                    objectName: "uploadProgressBox"
                    visible: root.hist.uploading
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: progCol.implicitHeight + 28
                    radius: Theme.radiusNone
                    color: Theme.selected
                    border.width: 1
                    border.color: Theme.rule
                    ColumnLayout {
                        id: progCol
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 8
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            UzSpinner { size: 14 }
                            UzLabel {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                text: qsTr("Uploading to “%1”").arg(root.hist.slotLabel)
                                color: Theme.accentText
                                font.pixelSize: Theme.roles.body.size
                                font.weight: Font.DemiBold
                            }
                        }
                        UzLabel {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Play is disabled until the upload finishes.")
                            color: Theme.ink2
                            font.pixelSize: Theme.roles.small.size
                        }
                    }
                }

                // Upload failed
                Rectangle {
                    objectName: "uploadFailedBox"
                    visible: root.uploadFailed
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: failCol.implicitHeight + 28
                    radius: Theme.radiusNone
                    color: Theme.dangerTint
                    border.width: 1
                    border.color: Theme.danger
                    ColumnLayout {
                        id: failCol
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 8
                        UzLabel { Layout.fillWidth: true; text: qsTr("✕ Upload failed"); color: Theme.danger; font.pixelSize: Theme.roles.body.size; font.weight: Font.DemiBold }
                        UzLabel {
                            objectName: "uploadFailedText"
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: root.hist.message
                            color: Theme.ink2
                            font.pixelSize: Theme.roles.small.size
                        }
                        Flow {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 8
                            layoutDirection: Qt.RightToLeft   // right-aligned, wraps under the first button when the column is narrow
                            UzButton { objectName: "uploadRetry"; kind: "primary"; implicitHeight: 30; font.pixelSize: Theme.roles.input.size; text: qsTr("Try again"); enabled: root.hist.online; onClicked: root.hist.retryUpload() }
                            UzButton { objectName: "uploadFailedCancel"; implicitHeight: 30; font.pixelSize: Theme.roles.input.size; text: qsTr("Cancel"); onClicked: { root.hist.dismissUploadFailure(); root.hist.dismissMessage() } }
                        }
                    }
                }

                UzLabel {
                    objectName: "historyUnsupported"
                    visible: !root.historyAvailable
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("This server keeps no version history. Update the server to restore versions or create snapshots.")
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                }

                // HISTORY
                RowLayout {
                    id: historyHeader
                    objectName: "historyHeader"
                    visible: root.historyAvailable
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.topMargin: 2
                    spacing: 8
                    Eyebrow { id: historyEyebrow; text: qsTr("History") }
                    Item { Layout.fillWidth: true }
                    UzSegmented {
                        objectName: "historyFilter"
                        // Deterministic width: never wider than what the eyebrow leaves, so wide fonts shrink and elide the labels
                        Layout.preferredWidth: Math.max(0, Math.min(implicitWidth, 200, body.width - historyEyebrow.implicitWidth - historyHeader.spacing))
                        Layout.maximumWidth: 200
                        Layout.minimumWidth: 0
                        segmentHeight: 22
                        current: root.filter
                        options: [
                            { value: "all", label: qsTr("All"), name: "historyFilterAll" },
                            { value: "snapshots", label: qsTr("Snapshots"), name: "historyFilterSnapshots" }
                        ]
                        onPicked: (v) => root.filter = v
                    }
                }

                UzLabel {
                    objectName: "historyLoading"
                    visible: root.historyAvailable && root.hist.loading && !root.hasCurrent && root.hist.history.length === 0
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Loading history from %1…").arg(root.player.hubAddress)
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                }
                ColumnLayout {
                    objectName: "historyEmpty"
                    visible: root.historyAvailable && !root.hist.loading && !root.hasCurrent && root.hist.history.length === 0
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 4
                    UzLabel { text: qsTr("No saves yet"); font.pixelSize: Theme.roles.body.size; font.weight: Font.DemiBold }
                    UzLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("The first save appears in slot “%1” after you play.").arg(root.hist.slotLabel)
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                    }
                }
                UzLabel {
                    objectName: "snapshotsEmpty"
                    visible: root.historyAvailable && root.filter === "snapshots" && root.rows.length <= (root.hasCurrent ? 1 : 0) && (root.hasCurrent || root.hist.history.length > 0)
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("No snapshots yet. A snapshot keeps a version until you delete it.")
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                }

                // Timeline
                ColumnLayout {
                    objectName: "timeline"
                    visible: root.historyAvailable
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 0
                    Repeater {
                        model: root.rows
                        delegate: SaveRow {
                            id: row
                            required property var modelData
                            required property int index
                            objectName: modelData.kind === "current" ? "historyRowCurrent" : "historyRow"
                            kind: modelData.kind
                            versionText: modelData.versionText
                            label: modelData.label
                            meta: modelData.meta
                            last: index === root.rows.length - 1
                            showRestore: modelData.kind !== "current"
                            showDelete: modelData.kind !== "current" && (modelData.isSnapshot === true || root.hist.canDeleteAnyVersion)
                            restoreEnabled: root.hist.restoreBlockReason === "" && !root.hist.busy && root.hist.online
                            deleteEnabled: root.hist.canDeleteSnapshot
                            muted: !root.hist.online
                            confirm: modelData.version === root.hist.restoreRequest.version ? "restore"
                                     : modelData.version === root.hist.deleteRequest.version ? "delete" : ""
                            confirmTitle: confirm === "delete"
                                          ? (modelData.label !== "" ? (modelData.isSnapshot ? qsTr("Delete snapshot %1 “%2”?") : qsTr("Delete version %1 “%2”?")).arg(modelData.versionText).arg(modelData.label)
                                                                    : (modelData.isSnapshot ? qsTr("Delete snapshot %1?") : qsTr("Delete version %1?")).arg(modelData.versionText))
                                          : qsTr("Restore %1 as the current version of “%2”?").arg(modelData.versionText).arg(root.hist.slotLabel)
                            confirmBody: confirm === "delete"
                                         ? (modelData.isSnapshot ? qsTr("Only this snapshot is removed.") : qsTr("Only this version is removed.")) + qsTr(" The current version and all other versions stay.")
                                         : qsTr("The current version %1 stays in the history as “Before restore”.").arg(root.hist.current.revisionText || "")
                           + (root.live ? "\n" + qsTr("The game restarts from this save. A local backup is made first.") : "")
                            confirmOk: confirm === "delete" ? (modelData.isSnapshot ? qsTr("Delete snapshot") : qsTr("Delete version")) : qsTr("Restore %1").arg(modelData.versionText)
                            onRestoreClicked: root.hist.requestRestore(modelData.version)
                            onDeleteClicked: root.hist.requestDelete(modelData.version)
                            onConfirmed: confirm === "delete" ? root.hist.confirmDelete() : root.hist.confirmRestore()
                            onCancelled: confirm === "delete" ? root.hist.cancelDelete() : root.hist.cancelRestore()
                        }
                    }
                }
            }
        }
    }
}
