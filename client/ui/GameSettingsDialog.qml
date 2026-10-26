import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Settings for this game (board 3c-3): a lg Dialog over the window with the overrides of one game, tabs per category and a
// SettingsRow per option that says "Uses your default (<value>)" or "Changed for this game · default <value>" with a per-row Reset.
// The values come from EmulationController.gameSettings(); changes go through setGameOption / resetGameOption, so the Emulation
// page and the game page tag follow. The dialog opens from the game page ("Settings for this game...") and from the "⋯" menu of the
// running game. Picture options apply to a running game at once, every other row at the next start (badge "applies on next start").
//   open(gameId)
// objectNames: gameSettingsDialog, gameSettingsTabs, gameSettingsNote, gameSettingsRestart, gameSettingsFooter,
// gameSettingsResetAll, gameSettingsDone, gameOption_<key> (SettingsRow), gameOptionSelect_<key>, optionReset_<key>.
Item {
    id: root
    required property PlayerController player
    readonly property EmulationController emu: player.emulation
    property string gameId: ""
    property var info: ({})
    property string activeTab: ""
    readonly property var tabs: info.tabs ?? []
    readonly property var current: {
        for (let i = 0; i < tabs.length; ++i) if (tabs[i].title === activeTab) return tabs[i]
        return tabs.length > 0 ? tabs[0] : ({ rows: [], note: "" })
    }
    readonly property alias dialog: dlg
    signal closed()
    readonly property bool opened: dlg.shown

    function reload() {
        if (root.gameId === "") return
        root.info = root.emu.gameSettings(root.gameId)
    }
    function open(id: string) {
        root.gameId = id
        root.emu.ensureGameCore(id)
        root.reload()
        root.activeTab = root.tabs.length > 0 ? root.tabs[0].title : ""
        dlg.open()
    }
    function close() { dlg.close() }
    readonly property var tabCounts: {
        const c = {}
        for (let i = 0; i < tabs.length; ++i) c[tabs[i].title] = tabs[i].changed
        return c
    }
    // The game runs now: the in-game screen layout and speed-up speed are live values (header menus) that are not stored as
    // game overrides, so the rows show them with their source instead of the stored default.
    readonly property bool runningHere: root.gameId !== "" && root.gameId === root.player.runningGameId && root.player.gameSession.active
    readonly property string layoutKey: "uzume.screen_layout"
    readonly property string speedKey: "uzume.speedup_ratio"
    function liveValue(key: string): var {
        if (!root.runningHere) return undefined
        if (key === root.layoutKey) return root.player.sessions.screenLayout
        if (key === root.speedKey) return String(Math.round(root.player.gameSession.fastForwardRatio * 10) / 10)
        return undefined
    }
    function isLive(r): bool {
        const v = root.liveValue(r.key)
        return v !== undefined && v !== r.value
    }
    function shownValue(r): string { return root.isLive(r) ? root.liveValue(r.key) : r.value }
    function applyLive(key: string, value: string) {
        if (!root.runningHere) return
        if (key === root.layoutKey) root.player.sessions.screenLayout = value
        else if (key === root.speedKey) root.player.gameSession.fastForwardRatio = Number(value)
    }
    // Tallest tab, estimated from its rows, so the dialog keeps one height on every tab (capped to the window).
    readonly property real rowsHeight: {
        let best = 0
        for (let i = 0; i < tabs.length; ++i) {
            const rows = tabs[i].rows ?? []
            let h = 0, last = ""
            for (let j = 0; j < rows.length; ++j) {
                h += 76
                if (tabs[i].id === "core" && (rows[j].category ?? "") !== last) { h += 32; last = rows[j].category ?? "" }
            }
            best = Math.max(best, h)
        }
        return best
    }
    function rowMeta(r): string {
        if (root.isLive(r)) return qsTr("Set in the game now · default %1").arg(r.defaultLabel)
        return r.isSet ? qsTr("Changed for this game · default %1").arg(r.defaultLabel)
                       : qsTr("Uses your default (%1)").arg(r.defaultLabel)
    }

    Connections {
        target: root.emu
        enabled: dlg.shown
        function onGameSettingsChanged(id) { if (id === "" || id === root.gameId) root.reload() }
    }

    UzDialog {
        id: dlg
        objectName: "gameSettingsDialog"
        parent: Window.contentItem
        size: "lg"
        onClosed: root.closed()
        label: root.info.systemLabel === undefined ? ""
               : qsTr("%1 · %2 · %3").arg(root.info.systemLabel).arg(root.info.coreName)
                   .arg(root.info.changedCount > 0 ? qsTr("%n change(s) from your defaults", "", root.info.changedCount) : qsTr("no changes from your defaults"))
        title: root.info.title === undefined ? qsTr("Settings for this game") : qsTr("Settings for %1").arg(root.info.title)

        UzTabs {
            objectName: "gameSettingsTabs"
            Layout.fillWidth: true
            visible: root.tabs.length > 1
            items: root.tabs.map(t => t.title)
            counts: root.tabCounts
            active: root.current.title ?? ""
            onPicked: label => root.activeTab = label
        }
        UzLabel {
            objectName: "gameSettingsNote"
            visible: (root.current.note ?? "") !== ""
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            wrapMode: Text.Wrap
            color: Theme.muted
            font.pixelSize: Theme.roles.small.size
            text: root.current.note ?? ""
        }
        Flickable {
            id: flick
            objectName: "gameSettingsScroll"
            Layout.fillWidth: true
            // One height for every tab (the tallest, capped to the window): the dialog does not jump when the tab changes.
            Layout.preferredHeight: {
                const wh = Window.height > 0 ? Window.height : 800
                return Math.max(160, Math.min(root.rowsHeight, wh * 0.6, wh - 420))
            }
            ScrollBar.vertical: ScrollBar { policy: flick.contentHeight > flick.height + 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
            contentWidth: width
            readonly property real barSpace: 14   // the scrollbar never covers the controls
            contentHeight: rowsCol.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ColumnLayout {
                id: rowsCol
                width: flick.width - flick.barSpace
                spacing: 0
                Repeater {
                    model: root.current.rows ?? []
                    delegate: ColumnLayout {
                        id: rowItem
                        required property var modelData
                        required property int index
                        readonly property var prev: index > 0 ? (root.current.rows[index - 1]) : null
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 0
                        UzSectionLabel {
                            visible: rowItem.modelData.category !== "" && rowItem.modelData.category !== undefined
                                     && (rowItem.prev === null || rowItem.prev.category !== rowItem.modelData.category)
                                     && root.current.id === "core"
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.space15
                            text: rowItem.modelData.category ?? ""
                        }
                        UzSettingsRow {
                            objectName: "gameOption_" + rowItem.modelData.key
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            idKey: rowItem.modelData.key
                            controlName: "gameOptionSelect_" + rowItem.modelData.key
                            toggleName: "gameOptionToggle_" + rowItem.modelData.key
                            label: rowItem.modelData.label
                            meta: root.rowMeta(rowItem.modelData)
                            description: rowItem.modelData.description
                            badge: rowItem.modelData.restart ? qsTr("applies on next start") : ""
                            values: rowItem.modelData.values
                            current: root.shownValue(rowItem.modelData)
                            changed: rowItem.modelData.isSet || root.isLive(rowItem.modelData)
                            onValuePicked: value => {
                                root.emu.setGameOption(root.gameId, rowItem.modelData.key, value)
                                root.applyLive(rowItem.modelData.key, value)   // the running game follows the dialog
                            }
                            onResetRequested: {
                                root.emu.resetGameOption(root.gameId, rowItem.modelData.key)
                                root.applyLive(rowItem.modelData.key, rowItem.modelData.defaultValue)
                            }
                        }
                    }
                }
            }
        }
        UzLabel {
            objectName: "gameSettingsRestart"
            visible: root.info.restartHint === true
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            wrapMode: Text.Wrap
            color: Theme.accentText
            font.pixelSize: Theme.roles.small.size
            text: qsTr("Changes marked \"applies on next start\" take effect the next time this game starts.")
        }
        UzLabel {
            objectName: "gameSettingsFooter"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            wrapMode: Text.Wrap
            color: Theme.dim
            font.pixelSize: Theme.roles.small.size
            text: qsTr("Reset goes back to your default for %1 in Emulation.").arg(root.info.systemLabel ?? "")
        }
        actions: [
            UzButton {
                objectName: "gameSettingsResetAll"
                text: qsTr("Reset all")
                enabled: (root.info.changedCount ?? 0) > 0
                onClicked: root.emu.resetGameOptions(root.gameId)
            },
            UzButton { objectName: "gameSettingsDone"; kind: "primary"; text: qsTr("Done"); onClicked: dlg.close() }
        ]
    }
}
