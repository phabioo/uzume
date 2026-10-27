import QtQuick
import QtQuick.Layouts
import Uzume.App

// "Variants" section of the game page Overview (design 10a-1, 10a-3c/d, 10c-3d): the variants of this game as rows VARIANT (name,
// "Created 14 Oct") · PRESET · SLOT · STATUS (Ready / Needs rebuild / Building n % / Failed) · actions (Play or Rebuild, `⋯` menu: Play,
// Rename…, Rebuild, Copy seed and settings, Open spoiler log…, Delete…). A running build shows "Building · about 3 min left" in the row
// and Details, which opens the build dialog again. Every variant plays in its own slot; Main is not touched. The section is hidden
// while the game cannot be randomized and has no variants; with variants but no plugin it shows the "plugin off" line instead of the
// table (10a-3c); with none yet, the empty state with Randomize… (10a-3d).
// Keys on a row: Enter Play, F2 Rename…, Del Delete…, Menu opens `⋯`, Ctrl+Shift+C copies seed and settings.
// objectNames: variantsSection, variantsLabel, variantsTable, variantRow_<n>, variantName_<n>, variantStatus_<n>, variantPlay_<n>,
// variantRebuild_<n>, variantDetails_<n>, variantMenuButton_<n>, variantsMenu, variantsNote, variantsEmpty, variantsPluginOff,
// variantsOpenPlugins, variantsActionNote, variantsCopiedNote, menuItem_variantPlay / variantRename / variantRebuild / variantCopy /
// variantSpoiler / variantDelete.
ColumnLayout {
    id: root
    objectName: "variantsSection"
    required property PlayerController player
    property string gameId: ""
    property bool playEnabled: true          // the game is not starting or running (variant Play follows Play of the page)
    readonly property VariantsController vc: root.player.variants
    readonly property var rows: root.vc.gameId === root.gameId ? root.vc.variants : []
    readonly property string randomize: {
        const _rev = root.vc.registryRevision
        return root.gameId === "" ? "none" : root.vc.randomizeState(root.gameId)
    }
    readonly property bool pluginAvailable: {
        const _rev = root.vc.registryRevision
        return root.vc.pluginAvailable()
    }
    readonly property var presetLabels: {
        const _rev = root.vc.registryRevision
        const m = ({})
        for (const p of (root.gameId === "" ? [] : root.vc.presets(root.gameId))) m[p.id] = p.custom ? qsTr("Custom") : p.label
        return m
    }
    readonly property var slotOptions: root.player.saveHistory.slotOptions
    property string actionNote: ""

    signal randomizeRequested()
    signal askDelete(var variant, int saveVersions)
    signal askRename(var variant)
    signal askSpoiler(var variant)
    signal showProblem(var variant)
    signal showBuild(var variant)

    visible: root.rows.length > 0 || root.randomize === "ok"
    spacing: Theme.space1
    Layout.fillWidth: true
    Layout.minimumWidth: 0

    function createdText(iso: string): string {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const ago = Date.now() - d.getTime()
        if (ago < 120000) return qsTr("Created just now")
        if (d.toDateString() === new Date().toDateString()) return qsTr("Created today")
        return qsTr("Created %1").arg(Qt.formatDate(d, "d MMM"))
    }
    function slotText(v: var): string {
        let count = -1
        for (const o of root.slotOptions) if (o.value === v.slot) count = o.count
        return v.name + (count > 0 ? " · v" + count : (count === 0 || count === -1 ? " · " + qsTr("new") : ""))
    }
    function slotVersions(v: var): int {
        for (const o of root.slotOptions) if (o.value === v.slot) return Math.max(0, o.count)
        return 0
    }
    function presetText(v: var): string {
        return v.presetId !== "" && root.presetLabels[v.presetId] !== undefined ? root.presetLabels[v.presetId] : "–"
    }
    function statusText(v: var): string {
        if (v.state === "ready") return qsTr("Ready")
        if (v.state === "needs_rebuild") return qsTr("Needs rebuild")
        if (v.state === "failed") return qsTr("Failed")
        return v.queued ? qsTr("Queued") : qsTr("Building %1 %").arg(v.percent)
    }
    function statusTone(v: var): string {
        return v.state === "ready" ? "ok" : v.state === "needs_rebuild" ? "amber" : v.state === "failed" ? "error" : "neutral"
    }
    function primary(v: var) {
        if (v.state === "ready") root.vc.play(v.id)
        else if (v.state === "needs_rebuild") root.vc.rebuild(v.id)
        else if (v.state === "failed") root.showProblem(v)
        else root.showBuild(v)
    }
    function copySeed(v: var) {
        const r = root.vc.ownSeed(v.id)
        if (!r.ok) { root.actionNote = qsTr("The seed of this variant isn’t available."); return }
        root.player.saveHistory.copyText(qsTr("Settings: %1").arg(r.settings) + "\n" + qsTr("Seed: %1").arg(r.seed))
        copiedNote.show()
    }
    Connections {
        target: root.vc
        function onActionFailed(action: string, id: string, error: string) {
            root.actionNote = error === "name_taken" ? qsTr("You already have a variant with that name.")
                              : action === "rename" ? qsTr("The variant couldn’t be renamed.") : qsTr("The variant couldn’t be deleted.")
        }
    }

    UzSectionLabel {
        objectName: "variantsLabel"
        Layout.fillWidth: true
        number: 3
        text: qsTr("Variants")
        meta: root.rows.length > 0 ? qsTr("%1 · OWN SLOTS").arg(Theme.plural(root.rows.length, qsTr("variant"), qsTr("variants")).toUpperCase()) : ""
    }
    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule }

    // 10a-3c: the plugin is off; variants and saves stay.
    ColumnLayout {
        objectName: "variantsPluginOff"
        visible: root.rows.length > 0 && !root.pluginAvailable
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space1
        UzLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.itemTitle.size
            font.weight: Font.Medium
            text: qsTr("Randomizer is off on this PC")
        }
        UzLabel {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
            text: qsTr("Your %1 and their saves stay. They play again when Randomizer is on.").arg(Theme.plural(root.rows.length, qsTr("variant"), qsTr("variants")))
        }
        UzButton {
            objectName: "variantsOpenPlugins"
            Layout.fillWidth: false
            size: "sm"
            text: qsTr("Open plugin settings")
            onClicked: root.player.showSettingsSection("plugins")
        }
    }

    // 10a-3d: none yet.
    UzStateBlock {
        objectName: "variantsEmpty"
        visible: root.rows.length === 0 && root.randomize === "ok"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        title: qsTr("No variants yet")
        text: qsTr("A variant is this game with a randomized world. It plays in its own slot; Main is not touched.")
        action: qsTr("Randomize…")
        onActionClicked: root.randomizeRequested()
    }

    // The table.
    ColumnLayout {
        id: table
        objectName: "variantsTable"
        visible: root.rows.length > 0 && root.pluginAvailable
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 0
        readonly property bool wide: width >= 700
        // Head: absolute columns that match the rows (right edge 8, actions 148, status 112, slot 176, preset 110, gap 12; a layout lost
        // the positions of the wide-only labels when they appeared after the first pass).
        Item {
            id: head
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            implicitHeight: headName.implicitHeight
            readonly property real statusX: width - Theme.space1 - 148 - Theme.space15 - 112
            readonly property real slotX: statusX - Theme.space15 - 176
            readonly property real presetX: slotX - Theme.space15 - 110
            component HeadLabel: UzMono {
                color: Theme.dim
                elide: Text.ElideRight
                font.pixelSize: Theme.roles.sectionLabel.size
                font.weight: Theme.roles.sectionLabel.weight
                font.letterSpacing: Theme.roles.sectionLabel.spacing
                font.capitalization: Font.AllUppercase
            }
            HeadLabel { id: headName; x: Theme.space15; width: Math.max(0, (table.wide ? head.presetX : head.statusX) - Theme.space15 - Theme.space15); text: qsTr("Variant") }
            HeadLabel { visible: table.wide; x: head.presetX; width: 110; text: qsTr("Preset") }
            HeadLabel { visible: table.wide; x: head.slotX; width: 176; text: qsTr("Slot") }
            HeadLabel { x: head.statusX; width: 112; text: qsTr("Status") }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; Layout.topMargin: Theme.space05; color: Theme.rule }
        Repeater {
            model: root.rows
            delegate: FocusScope {
                id: row
                required property var modelData
                required property int index
                objectName: "variantRow_" + index
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                implicitHeight: 52
                activeFocusOnTab: true
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.name + ", " + root.statusText(modelData)
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { if (root.playEnabled || row.modelData.state !== "ready") root.primary(row.modelData); event.accepted = true }
                    else if (event.key === Qt.Key_F2) { root.askRename(row.modelData); event.accepted = true }
                    else if (event.key === Qt.Key_Delete) { root.askDelete(row.modelData, root.slotVersions(row.modelData)); event.accepted = true }
                    else if (event.key === Qt.Key_Menu) { root.openMenu(row.index, menuButton); event.accepted = true }
                    else if ((event.modifiers & Qt.ControlModifier) && (event.modifiers & Qt.ShiftModifier) && event.key === Qt.Key_C) { root.copySeed(row.modelData); event.accepted = true }
                }
                Rectangle { anchors.fill: parent; color: rowHover.hovered ? Theme.hover : "transparent" }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                HoverHandler { id: rowHover }
                TapHandler { onTapped: row.forceActiveFocus() }
                UzFocusRing { inset: true; visible: row.activeFocus }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space15
                    anchors.rightMargin: Theme.space1
                    spacing: Theme.space15
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        spacing: 2
                        UzLabel {
                            objectName: "variantName_" + row.index
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pixelSize: Theme.roles.input.size
                            font.weight: Font.Medium
                            text: row.modelData.name
                        }
                        UzMono {
                            objectName: "variantMeta_" + row.index
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pixelSize: Theme.roles.meta11.size
                            color: Theme.dim
                            text: row.modelData.state === "building"
                                  ? (row.modelData.etaText !== "" ? qsTr("Building · %1").arg(row.modelData.etaText) : qsTr("Building"))
                                  : row.modelData.synced === false ? qsTr("Not synced") : root.createdText(row.modelData.createdAt)
                        }
                    }
                    UzLabel {
                        visible: table.wide
                        Layout.preferredWidth: 110
                        Layout.maximumWidth: 110
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.ink2
                        text: root.presetText(row.modelData)
                    }
                    UzMono {
                        objectName: "variantSlot_" + row.index
                        visible: table.wide
                        Layout.preferredWidth: 176
                        Layout.maximumWidth: 176
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        font.pixelSize: Theme.roles.meta11.size
                        color: Theme.ink2
                        text: root.slotText(row.modelData)
                    }
                    Item {
                        Layout.preferredWidth: 112
                        Layout.maximumWidth: 112
                        Layout.minimumWidth: 0
                        implicitHeight: statusTag.implicitHeight
                        UzTag {
                            id: statusTag
                            objectName: "variantStatus_" + row.index
                            width: Math.min(implicitWidth, parent.width)
                            tone: root.statusTone(row.modelData)
                            small: true
                            text: root.statusText(row.modelData)
                        }
                    }
                    RowLayout {
                        Layout.preferredWidth: 148
                        Layout.maximumWidth: 148
                        Layout.minimumWidth: 0
                        spacing: Theme.space05
                        Item { Layout.fillWidth: true }
                        UzButton {
                            objectName: "variantPlay_" + row.index
                            visible: row.modelData.state === "ready"
                            size: "sm"
                            text: qsTr("Play")
                            enabled: root.playEnabled
                            Accessible.name: qsTr("Play %1").arg(row.modelData.name)
                            onClicked: root.primary(row.modelData)
                        }
                        UzButton {
                            objectName: "variantRebuild_" + row.index
                            visible: row.modelData.state === "needs_rebuild"
                            size: "sm"
                            text: qsTr("Rebuild")
                            Accessible.name: qsTr("Rebuild %1").arg(row.modelData.name)
                            onClicked: root.primary(row.modelData)
                        }
                        UzButton {
                            objectName: "variantDetails_" + row.index
                            visible: row.modelData.state === "failed" || row.modelData.state === "building"
                            size: "sm"
                            text: qsTr("Details")
                            Accessible.name: qsTr("Details of %1").arg(row.modelData.name)
                            onClicked: root.primary(row.modelData)
                        }
                        UzIconButton {
                            id: menuButton
                            objectName: "variantMenuButton_" + row.index
                            glyph: "more"
                            outlined: true
                            on: variantsMenu.opened && root.menuRow === row.index
                            implicitHeight: Theme.row32
                            implicitWidth: Theme.row32
                            Accessible.name: qsTr("Variant menu for %1").arg(row.modelData.name)
                            onClicked: root.openMenu(row.index, menuButton)
                        }
                    }
                }
            }
        }
        UzLabel {
            objectName: "variantsNote"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.topMargin: Theme.space1
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
            text: qsTr("Each variant plays in its own slot. Main is not touched.")
        }
    }
    UzLabel {
        objectName: "variantsActionNote"
        visible: root.actionNote !== ""
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.roles.small.size
        color: Theme.danger
        text: root.actionNote
    }
    UzLabel {
        id: copiedNote
        objectName: "variantsCopiedNote"
        visible: false
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.roles.small.size
        color: Theme.ink2
        text: qsTr("Copied. Someone who pastes both builds the same world.")
        function show() { visible = true; copiedTimer.restart() }
        Timer { id: copiedTimer; interval: 8000; onTriggered: copiedNote.visible = false }
    }

    // The `⋯` menu of a row.
    property int menuRow: -1
    readonly property var menuVariant: root.menuRow >= 0 && root.menuRow < root.rows.length ? root.rows[root.menuRow] : ({})
    function openMenu(index: int, anchor: Item) {
        root.menuRow = index
        variantsMenu.openFor(anchor, "", true)
    }
    UzMenu {
        id: variantsMenu
        objectName: "variantsMenu"
        readonly property var v: root.menuVariant
        items: {
            const v = variantsMenu.v
            if (v.id === undefined) return []
            const out = [{ label: qsTr("Play"), name: "variantPlay", shortcut: "Enter", disabled: v.state === "building" || v.state === "failed" || !root.playEnabled },
                         { label: qsTr("Rename…"), name: "variantRename", shortcut: "F2" },
                         { label: qsTr("Rebuild"), name: "variantRebuild", disabled: v.state === "building" || v.isRun === true }]
            if (!v.isRun) out.push({ label: qsTr("Copy seed and settings"), name: "variantCopy", shortcut: "Ctrl+Shift+C" })
            if (v.hasSpoilerLog === true) out.push({ label: qsTr("Open spoiler log…"), name: "variantSpoiler" })
            out.push({ separator: true })
            out.push({ label: qsTr("Delete…"), name: "variantDelete", shortcut: "Del", danger: true, disabled: v.state === "building" })
            return out
        }
        onChosen: (item) => {
            const v = variantsMenu.v
            if (item.name === "variantPlay") root.vc.play(v.id)
            else if (item.name === "variantRename") root.askRename(v)
            else if (item.name === "variantRebuild") root.vc.rebuild(v.id)
            else if (item.name === "variantCopy") root.copySeed(v)
            else if (item.name === "variantSpoiler") root.askSpoiler(v)
            else if (item.name === "variantDelete") root.askDelete(v, root.slotVersions(v))
        }
    }
}
