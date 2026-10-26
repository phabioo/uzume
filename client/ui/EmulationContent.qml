import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Emulation section of Settings (was the Emulation screen): tabs for Defaults, the systems and the per-game overrides, then the
// options the core reports. Settings are local to this device; only explicitly changed values are stored. Options of a system
// are exactly those the core reports (no invented options). The page header, the section column and the scrolling belong to
// SettingsScreen; this item is only the content below the header.
ColumnLayout {
    id: root
    objectName: "emulationContent"
    required property PlayerController player
    readonly property var emu: root.player.emulation
    spacing: Theme.space2

    function toneColor(tone: string): color {
        return tone === "neutral" ? Theme.muted : Theme.toneColor(tone)
    }
    // Position of a category heading among the headings of its group (the "02 —" of the section label; 01 is the page).
    function categoryNumber(options: var, index: int): int {
        let n = 1
        for (let i = 1; i <= index; ++i) {
            if (options[i].category !== options[i - 1].category) {
                ++n
            }
        }
        return n + 1
    }

    // One tab of the row: ink 500 with the 2 px Yamabuki underline when active, muted otherwise.
    component CatTab: Item {
        id: ct
        property string text: ""
        property bool active: false
        property real maxWidth: 100000
        signal clicked()
        width: Math.min(ctLabel.implicitWidth, maxWidth)
        height: 40
        activeFocusOnTab: true
        Accessible.role: Accessible.PageTab
        Accessible.name: ct.text
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                ct.clicked()
                event.accepted = true
            }
        }
        Text {
            id: ctLabel
            width: Math.min(implicitWidth, ct.maxWidth)
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: ct.text
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: 14
            font.weight: ct.active ? Font.Medium : Font.Normal
            color: ct.active || ctHover.hovered ? Theme.ink : Theme.muted
        }
        Rectangle { visible: ct.active; anchors.bottom: parent.bottom; width: parent.width; height: 2; color: Theme.accent }
        UzFocusRing { visible: ct.activeFocus }
        HoverHandler { id: ctHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: ct.clicked() }
    }

    // Quiet note: ground plane with the 2 px Yamabuki start marker (no amber box).
    component Note: Rectangle {
        id: note
        property string text: ""
        property alias label: noteText
        Layout.fillWidth: true
        implicitHeight: noteText.implicitHeight + 2 * Theme.space15
        color: Theme.selected
        Rectangle { width: 2; height: parent.height; color: Theme.accent }
        UzLabel {
            id: noteText
            anchors.fill: parent
            anchors.leftMargin: Theme.space2
            anchors.rightMargin: Theme.space2
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
            text: note.text
            color: Theme.ink2
            font.pixelSize: Theme.roles.input.size
        }
    }

    Connections {
        target: root.player.controllers
        enabled: root.visible
        function onPadCommand(command: string) {
            if (command === "search") emuSearch.forceActiveFocus()
        }
    }

    // Tabs (board 3e): Defaults first, then the systems the model lists, then the per-game overrides. A tab shows how many of its
    // values differ from the default.
    readonly property string defaultsLabel: qsTr("Defaults")
    readonly property string gamesLabel: qsTr("Game overrides")
    readonly property var systemList: root.emu.systems
    // System tabs use the short code (NDS, 3DS, GBA …) so that every tab fits at 1280 px; the full name is on the line below.
    function tabName(sys) { return sys && sys.id ? String(sys.id).toUpperCase() : (sys && sys.name ? sys.name : "") }
    readonly property var tabLabels: [root.defaultsLabel].concat(root.systemList.map(function (x) { return root.tabName(x) }), [root.gamesLabel])
    readonly property string activeTab: root.emu.defaultsSelected ? root.defaultsLabel
                                        : root.emu.gameSelected ? root.gamesLabel
                                        : (root.emu.system.id !== undefined ? root.tabName(root.emu.system) : "")
    readonly property var tabCounts: {
        const c = {}
        c[root.defaultsLabel] = root.emu.defaultsChangedCount
        for (let i = 0; i < root.systemList.length; ++i) c[root.tabName(root.systemList[i])] = root.systemList[i].changedCount
        c[root.gamesLabel] = root.emu.gameOverrideCount
        return c
    }
    function pickTab(label: string) {
        if (label === root.defaultsLabel) {
            root.emu.level = "global"
        } else if (label === root.gamesLabel) {
            root.emu.level = "game"
        } else {
            for (let i = 0; i < root.systemList.length; ++i) {
                if (root.tabName(root.systemList[i]) === label) {
                    root.emu.level = "system"
                    root.emu.selectSystem(root.systemList[i].id)
                    return
                }
            }
        }
    }

    UzTabs {
        id: emuTabs
        objectName: "emulationTabs"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        size: "lg"
        items: root.tabLabels
        counts: root.tabCounts
        active: root.activeTab
        onPicked: label => root.pickTab(label)
    }

    // The selected system: its core, whether it is ready to play and the firmware state (what the system column showed).
    Repeater {
        model: root.systemList
        delegate: RowLayout {
            id: card
            required property var modelData
            objectName: "systemStatus_" + modelData.id
            visible: !root.emu.defaultsSelected && !root.emu.gameSelected && modelData.id === root.emu.selectedSystem
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.space3
            UzLabel {
                objectName: "systemCore_" + card.modelData.id
                Layout.minimumWidth: 0
                text: card.modelData.coreVersion !== ""
                      ? qsTr("%1 · %2").arg(card.modelData.coreName).arg(card.modelData.coreVersion)
                      : card.modelData.coreName
                color: Theme.ink2
                font.pixelSize: Theme.roles.small.size
                elide: Text.ElideRight
            }
            RowLayout {
                spacing: Theme.space1
                StatusDot { tone: card.modelData.readyTone }
                UzLabel {
                    objectName: "systemReady_" + card.modelData.id
                    text: card.modelData.readyText
                    color: root.toneColor(card.modelData.readyTone)
                    font.pixelSize: Theme.roles.small.size
                    elide: Text.ElideRight
                }
            }
            UzLabel {
                objectName: "systemFirmware_" + card.modelData.id
                visible: card.modelData.firmwareText !== ""
                Layout.minimumWidth: 0
                text: card.modelData.firmwareText
                color: root.toneColor(card.modelData.firmwareTone)
                font.pixelSize: Theme.roles.small.size
                elide: Text.ElideRight
            }
            Item { Layout.fillWidth: true }
        }
    }

    // Quiet indicator of the background core update (no dialog): progress row, else the last result.
    UzLabel {
        objectName: "coreUpdateRow"
        Layout.fillWidth: true
        visible: text !== ""
        text: root.player.coreUpdate.active ? root.player.coreUpdate.text : root.player.coreUpdate.lastText
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
        elide: Text.ElideRight
    }

    // Subtitle with "Reset all to default" at the right, then the search. Below 700 the search field takes its own line.
    ColumnLayout {
        id: headRow
        objectName: "emulationHeadRow"
        readonly property bool stacked: root.width < 700
        Layout.fillWidth: true
        spacing: Theme.space2
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space2
            UzLabel {
                objectName: "emulationSubtitle"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: root.emu.defaultsSelected
                      ? qsTr("Systems use these unless you change them there.")
                      : root.emu.gameSelected
                        ? qsTr("Only what you change here applies to this game; everything else follows the system.")
                        : qsTr("Only options this core reports.")
                color: Theme.muted
                font.pixelSize: Theme.roles.small.size
                lineHeight: Theme.roles.small.line
                lineHeightMode: Text.FixedHeight
                wrapMode: Text.WordWrap
            }
            UzButton {
                objectName: "resetAllChanged"
                visible: root.emu.changedCount > 0
                kind: "ghost"
                text: qsTr("Reset all to default")
                onClicked: root.emu.resetAllChanged()
            }
        }
        UzField {
            id: emuSearch
            objectName: "emulationSearch"
            Layout.fillWidth: headRow.stacked
            Layout.preferredWidth: headRow.stacked ? -1 : 260
            Layout.minimumWidth: 120
            font.family: Theme.sans
            placeholderText: qsTr("Search settings…")
            text: root.emu.searchText
            onTextChanged: root.emu.searchText = text
        }
    }

    // Per-game section: pick a game from the library.
    ColumnLayout {
        objectName: "gamePicker"
        visible: root.emu.gameSelected
        Layout.fillWidth: true
        spacing: Theme.space1
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space15
            UzLabel { text: qsTr("Game"); color: Theme.muted; font.pixelSize: Theme.roles.fieldLabel.size; font.weight: Theme.roles.fieldLabel.weight }
            UzModelSelect {
                id: gameSelect
                objectName: "gameSelect"
                Layout.fillWidth: true
                Layout.maximumWidth: 460
                enabled: root.emu.gameCount > 0
                displayText: root.emu.selectedGame === "" ? qsTr("Pick a game…") : root.emu.game.label
                model: root.emu.gamePicker
                current: root.emu.selectedGame
                onPicked: value => root.emu.selectGame(value)
            }
            Text {
                objectName: "gameChangedCount"
                visible: root.emu.selectedGame !== "" && root.emu.game.changedCount > 0
                text: qsTr("%1 changed").arg(root.emu.game.changedCount)
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.meta.size
                font.letterSpacing: Theme.roles.meta.spacing
                font.capitalization: Font.AllUppercase
                color: Theme.dim
            }
        }
        UzLabel {
            objectName: "gamePickerHint"
            visible: root.emu.selectedGame === ""
            Layout.fillWidth: true
            text: root.emu.gameCount > 0 ? qsTr("Choose a game to give it its own core or options. Games without changes follow their system.")
                                            : qsTr("No games in the library yet.")
            color: Theme.muted
            font.pixelSize: Theme.roles.small.size
            wrapMode: Text.WordWrap
        }
    }

    // Category tabs (the core's own categories). They never wrap or widen the column: what does not fit collapses
    // into a "+N more ▾" tab with a menu (decisions.md ag).
    Item {
        id: chipBar
        objectName: "categoryChips"
        visible: root.emu.categories.length > 0
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredWidth: 1
        Layout.preferredHeight: 40
        readonly property var cats: root.emu.categories
        readonly property real tabGap: 28
        FontMetrics { id: chipMetrics; font.family: Theme.sans; font.pixelSize: 14; font.weight: Font.Medium }
        function chipWidth(t: string): real { return Math.ceil(chipMetrics.advanceWidth(t)) + 1 }
        function moreText(n: int): string { return qsTr("+%1 more ▾").arg(n) }
        // Number of category tabs that fit next to "All" (and the "+N more" tab when some are left out).
        readonly property int shown: {
            const n = chipBar.cats.length
            let used = chipBar.chipWidth(qsTr("All"))
            let k = 0
            for (; k < n; ++k) {
                const need = used + chipBar.tabGap + chipBar.chipWidth(chipBar.cats[k])
                const rest = n - k - 1
                const reserve = rest > 0 ? chipBar.tabGap + chipBar.chipWidth(chipBar.moreText(rest)) : 0
                if (need + reserve > chipBar.width) {
                    break
                }
                used = need
            }
            return k
        }
        readonly property bool collapsed: chipBar.shown < chipBar.cats.length
        readonly property bool hiddenActive: root.emu.categoryFilter !== "" && chipBar.cats.indexOf(root.emu.categoryFilter) >= chipBar.shown

        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
        Row {
            spacing: chipBar.tabGap
            CatTab {
                id: allChip
                objectName: "categoryAll"
                maxWidth: chipBar.width
                text: qsTr("All")
                active: root.emu.categoryFilter === ""
                onClicked: root.emu.categoryFilter = ""
            }
            Repeater {
                model: chipBar.cats
                delegate: CatTab {
                    required property string modelData
                    required property int index
                    objectName: "category_" + modelData
                    visible: index < chipBar.shown
                    text: modelData
                    active: root.emu.categoryFilter === modelData
                    onClicked: root.emu.categoryFilter = modelData
                }
            }
            CatTab {
                id: moreChip
                objectName: "categoryMore"
                maxWidth: Math.max(60, chipBar.width - allChip.width - chipBar.tabGap)
                visible: chipBar.collapsed
                text: chipBar.moreText(chipBar.cats.length - chipBar.shown)
                active: chipBar.hiddenActive
                onClicked: overflowMenu.opened ? overflowMenu.close() : overflowMenu.open()
            }
        }
        Popup {
            id: overflowMenu
            parent: moreChip
            y: moreChip.height + Theme.space05
            width: Theme.menu + 20
            padding: 0
            contentItem: Column {
                spacing: 0
                Repeater {
                    model: chipBar.cats
                    delegate: Rectangle {
                        id: ovItem
                        required property string modelData
                        required property int index
                        objectName: "categoryOverflow_" + modelData
                        visible: index >= chipBar.shown
                        width: parent ? parent.width : 0
                        height: visible ? Theme.row32 : 0
                        color: ovHover.hovered ? Theme.hover : "transparent"
                        Rectangle { visible: root.emu.categoryFilter === ovItem.modelData; width: 2; height: parent.height; color: Theme.accent }
                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.space15
                            anchors.rightMargin: Theme.space15
                            text: ovItem.modelData
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.input.size
                            font.weight: root.emu.categoryFilter === ovItem.modelData ? Font.Medium : Font.Normal
                            color: Theme.ink
                        }
                        HoverHandler { id: ovHover }
                        TapHandler {
                            onTapped: {
                                root.emu.categoryFilter = ovItem.modelData
                                overflowMenu.close()
                            }
                        }
                    }
                }
            }
            background: Rectangle {
                color: Theme.surface
                border.width: 1
                border.color: Theme.rule
            }
        }
    }

    Note {
        objectName: "restartHint"
        visible: root.emu.restartHint
        text: qsTr("Some changes apply after the next game start.")
    }

    RowLayout {
        visible: root.emu.coreProbing
        objectName: "coreProbingNote"
        Layout.fillWidth: true
        spacing: Theme.space15
        UzSpinner { size: 14; color: Theme.accent }
        UzLabel {
            Layout.fillWidth: true
            text: qsTr("Reading the core's options ...")
            color: Theme.muted
            font.pixelSize: Theme.roles.small.size
            wrapMode: Text.WordWrap
        }
    }

    UzLabel {
        visible: root.emu.gameRunning
        objectName: "gameRunningNote"
        Layout.fillWidth: true
        text: qsTr("A game is running. Changes apply the next time a game starts.")
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
        wrapMode: Text.WordWrap
    }

    Repeater {
        model: root.emu.groups
        delegate: ColumnLayout {
            id: group
            required property var modelData
            objectName: "optionGroup_" + modelData.id
            Layout.fillWidth: true
            spacing: 0

            UzLabel {
                objectName: "coreNoteHint"
                visible: group.modelData.note !== ""
                Layout.fillWidth: true
                Layout.bottomMargin: Theme.space15
                text: group.modelData.note
                color: Theme.accentText     // one hint style: accent link colour, like the per-row "Reset" (P32)
                font.pixelSize: Theme.roles.small.size
                wrapMode: Text.WordWrap
            }
            // The list starts on a `rule` line (a heading brings its own).
            Rectangle {
                visible: group.modelData.options.length > 0 && group.modelData.options[0].category === ""
                Layout.fillWidth: true
                height: 1
                color: Theme.rule
            }

            Repeater {
                model: group.modelData.options
                delegate: ColumnLayout {
                    id: opt
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 0
                    // Category heading when the category changes.
                    ColumnLayout {
                        visible: opt.modelData.category !== "" && (opt.index === 0 || group.modelData.options[opt.index - 1].category !== opt.modelData.category)
                        Layout.fillWidth: true
                        Layout.topMargin: opt.index === 0 ? 0 : Theme.space3
                        spacing: Theme.space15
                        UzSectionLabel {
                            objectName: "categoryTitle_" + opt.modelData.category
                            Layout.fillWidth: true
                            number: root.categoryNumber(group.modelData.options, opt.index)
                            text: opt.modelData.category
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule }
                    }
                    UzSettingsRow {
                        objectName: "optionRow_" + opt.modelData.key
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        idKey: opt.modelData.key
                        controlName: "optionSelect_" + opt.modelData.key
                        toggleName: "optionToggle_" + opt.modelData.key
                        label: opt.modelData.label
                        description: opt.modelData.description
                        badge: opt.modelData.restart ? qsTr("applies on next start") : ""
                        disabledReason: opt.modelData.disabledReason || ""
                        values: opt.modelData.values
                        current: opt.modelData.value
                        changed: opt.modelData.isSet
                        onValuePicked: value => root.emu.setOption(opt.modelData.key, value)
                        onResetRequested: root.emu.resetOption(opt.modelData.key)
                    }
                }
            }
        }
    }

    UzLabel {
        visible: root.emu.defaultsSelected
        objectName: "globalCoreHint"
        Layout.fillWidth: true
        text: qsTr("Options of the core are set per system, because their keys belong to the core.")
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
        wrapMode: Text.WordWrap
    }
    UzLabel {
        visible: !root.emu.defaultsSelected && root.emu.lockedCount > 0
        objectName: "lockedHint"
        Layout.fillWidth: true
        text: (root.emu.lockedCount === 1 ? qsTr("1 option of the core is controlled by Uzume (render mode, screen layout, on-screen display, firmware) and is not shown.")
                                    : qsTr("%1 options of the core are controlled by Uzume (render mode, screen layout, on-screen display, firmware) and are not shown.").arg(root.emu.lockedCount))
        color: Theme.dim
        font.pixelSize: Theme.roles.small.size
        wrapMode: Text.WordWrap
    }
    UzLabel {
        objectName: "noMatchHint"
        visible: (root.emu.searchText !== "" || root.emu.categoryFilter !== "") && root.emu.groups.length > 0 && !root.hasVisibleOptions
        Layout.fillWidth: true
        text: qsTr("No setting matches.")
        color: Theme.muted
        font.pixelSize: Theme.roles.input.size
    }

    readonly property bool hasVisibleOptions: {
        const g = root.emu.groups
        for (let i = 0; i < g.length; ++i) {
            if (g[i].options.length > 0) {
                return true
            }
        }
        return false
    }
}
