import QtQuick
import QtQuick.Layouts
import Uzume.App

// "Continue from" block of the game page overview: the save slots as 44 px rows (slot name, version, mono meta, sync pill, Play and
// for a run's slot "Open run"), then "All saves and versions" (the Saves tab; "Upload save file…" while there is no save yet).
// States: normal, sync pending, server offline, save conflict, no saves yet, server without save sync.
Rectangle {
    id: root
    required property PlayerController player
    readonly property SaveHistoryController hist: player.saveHistory
    readonly property var game: player.selectedGame
    readonly property string syncKind: game.syncKind || "none"
    readonly property bool conflict: syncKind === "conflict"
    readonly property bool offline: hist.slotsAvailable ? !hist.online : hist.hasSaveData
    readonly property bool unsupported: !hist.hasSaveData
    readonly property bool noSaves: hist.slotsAvailable && hist.online && !hist.loading && hist.current.revision === undefined
                                    && hist.history.length === 0 && (syncKind === "none" || hist.available)
    // 3c-5: the game of this summary is paused in the background; the checkpoint was written when it was paused.
    readonly property bool running: game.running === true
    readonly property var bg: player.backgroundGame
    property real nowMs: Date.now()
    function ago(ms) {
        const s = Math.max(0, Math.round((nowMs - ms) / 1000))
        return s < 60 ? qsTr("%1 s").arg(s) : (s < 3600 ? qsTr("%1 min").arg(Math.floor(s / 60)) : qsTr("%1 h").arg(Math.floor(s / 3600)))
    }
    Timer { running: root.running; interval: 10000; repeat: true; onTriggered: root.nowMs = Date.now() }
    onRunningChanged: nowMs = Date.now()
    signal manage()
    signal uploadRequested()
    // Starts the game from a slot: the slot becomes the selected one, then the game starts like the Play button.
    function playSlot(slot: string) {
        if (slot !== root.hist.slot) root.hist.selectSlot(slot)
        root.player.playSelected()
    }
    function openRun(slot: string) {
        const id = root.player.runs.runIdForSlot(slot)
        if (id === "") return
        root.player.showRuns()
        root.player.runs.openRun(id)
    }

    objectName: "saveSummary"
    Layout.fillWidth: true
    Layout.minimumWidth: 0
    Layout.preferredWidth: 1
    implicitHeight: col.implicitHeight
    color: "transparent"

    ColumnLayout {
        id: col
        width: root.width
        spacing: Theme.space1

        UzSectionLabel {
            objectName: "saveSummaryLabel"
            Layout.fillWidth: true
            number: 1
            text: root.conflict ? qsTr("▲ Save conflict") : qsTr("Continue from")
        }

        // Server without save sync (or user unknown): the old "Save" row text
        UzLabel {
            objectName: "saveUnsupported"
            visible: root.unsupported
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.game.saveText || ""
            color: Theme.muted
            font.pixelSize: Theme.roles.input.size
        }

        // Conflict
        ColumnLayout {
            objectName: "saveConflictBlock"
            visible: root.conflict && !root.unsupported
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.space1
            UzLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("The server and this device have different saves for “%1”. Nothing is overwritten until you decide.").arg(root.hist.slotLabel)
                color: Theme.ink2
                font.pixelSize: Theme.roles.input.size
            }
            UzButton {
                objectName: "resolveConflictButton"
                Layout.fillWidth: false
                implicitHeight: Theme.row32
                font.pixelSize: Theme.roles.input.size
                text: qsTr("Resolve conflict")
                enabled: root.game.canPlay === true && !root.hist.confirmationOpen
                onClicked: root.player.playSelected()
            }
            UzLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("The conflict dialog opens when the game starts. Restore, upload and new slot are paused for “%1” until it is resolved.").arg(root.hist.slotLabel)
                color: Theme.dim
                font.pixelSize: Theme.roles.small.size
            }
        }

        // No saves yet
        ColumnLayout {
            objectName: "saveEmptyBlock"
            visible: root.noSaves && !root.conflict && !root.unsupported
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.space05
            UzLabel { text: qsTr("No saves yet"); font.pixelSize: Theme.roles.body.size; font.weight: Font.DemiBold }
            UzLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("The first save appears in slot “%1” after you play.").arg(root.hist.slotLabel)
                color: Theme.muted
                font.pixelSize: Theme.roles.small.size
            }
        }

        // Loading
        UzLabel {
            objectName: "saveLoading"
            visible: !root.unsupported && !root.noSaves && !root.conflict && root.hist.current.revision === undefined
            Layout.fillWidth: true
            text: root.hist.loading ? qsTr("Loading…") : (root.game.saveText || "")
            color: Theme.muted
            font.pixelSize: Theme.roles.small.size
        }

        // The slots: 44 px rows. The selected slot carries the version, the sync state and where the save is; the others their
        // number of versions.
        ColumnLayout {
            objectName: "saveSlotRows"
            visible: !root.unsupported && !root.conflict && root.hist.current.revision !== undefined
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 0
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule }
            Repeater {
                model: root.hist.slotOptions
                delegate: Item {
                    id: slotRow
                    required property var modelData
                    readonly property bool selected: modelData.value === root.hist.slot
                    readonly property bool isRun: root.player.runs.runIdForSlot(modelData.value) !== ""
                    objectName: selected ? "saveCurrentRow" : "saveSlotRow"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: Theme.row44
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                    RowLayout {
                        anchors.fill: parent
                        spacing: Theme.space1
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 0
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: Theme.space1
                                UzLabel {
                                    objectName: slotRow.selected ? "saveSlotName" : ""
                                    text: slotRow.modelData.label
                                    font.pixelSize: Theme.roles.body.size
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: 200
                                }
                                UzMono {
                                    objectName: slotRow.selected ? "saveVersion" : ""
                                    visible: slotRow.selected
                                    text: root.hist.current.revisionText || ""
                                    color: Theme.muted
                                    font.pixelSize: Theme.roles.input.size
                                }
                                Item { Layout.fillWidth: true }
                            }
                            UzMono {
                                objectName: slotRow.selected ? "saveMeta" : ""
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                color: Theme.dim
                                font.pixelSize: Theme.roles.meta11.size
                                font.capitalization: Font.AllUppercase
                                text: slotRow.selected ? (
                                      root.running && root.bg.pausedMs ? qsTr("checkpoint %1 ago · this device").arg(root.ago(root.bg.pausedMs))
                                      : root.offline ? qsTr("last synced %1").arg(root.hist.lastSynced)
                                      : root.syncKind === "pending" ? qsTr("local changes not uploaded yet · this device")
                                      : (root.hist.current.device ? qsTr("%1 · %2").arg(root.hist.current.when).arg(root.hist.current.device)
                                                                  : (root.hist.current.when || "")
                                                                    + (root.syncKind === "synced" ? qsTr(" · on server") : "")))
                                      : (slotRow.modelData.count > 0 ? Theme.plural(slotRow.modelData.count, qsTr("version"), qsTr("versions")) : "")
                            }
                        }
                        UzTag {
                            objectName: slotRow.selected ? "saveSyncPill" : ""
                            visible: slotRow.selected && text !== ""
                            small: true
                            tone: root.running || root.offline ? "neutral" : (root.syncKind === "pending" ? "warn" : "ok")
                            text: root.running ? qsTr("Syncs on quit")
                                  : root.offline ? qsTr("Offline")
                                  : root.syncKind === "pending" ? qsTr("⟳ Sync pending")
                                  : root.syncKind === "synced" ? qsTr("✓ Synced") : ""
                        }
                        UzButton {
                            objectName: slotRow.selected ? "saveOpenRun" : ""
                            visible: slotRow.isRun
                            kind: "link"
                            Layout.fillWidth: false
                            text: qsTr("Open run")
                            onClicked: root.openRun(slotRow.modelData.value)
                        }
                        UzButton {
                            objectName: slotRow.selected ? "savePlaySlot" : ""
                            kind: "link"
                            Layout.fillWidth: false
                            text: qsTr("Play")
                            enabled: root.game.canPlay === true && !root.hist.confirmationOpen
                            onClicked: root.playSlot(slotRow.modelData.value)
                        }
                    }
                }
            }
        }

        UzLabel {
            objectName: "saveOfflineNote"
            visible: root.offline && !root.unsupported
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("You can play. The save syncs when the server is back.")
            color: Theme.muted
            font.pixelSize: Theme.roles.small.size
        }

        // Footer: the link to the Saves tab and "2 slots · 8 versions in Main"
        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            visible: !root.unsupported
            spacing: Theme.space2
            UzButton {
                id: link
                objectName: "manageSavesLink"
                Layout.minimumWidth: 0
                kind: "link"
                implicitHeight: Theme.row28
                font.pixelSize: Theme.roles.input.size
                font.weight: Font.Medium
                text: root.noSaves && root.hist.canUploadFile && root.hist.uploadBlockReason === "" ? qsTr("Upload save file…") : qsTr("All saves and versions")
                onClicked: root.noSaves && root.hist.canUploadFile && root.hist.uploadBlockReason === "" ? root.uploadRequested() : root.manage()
            }
            UzLabel {
                objectName: "saveCounts"
                visible: root.hist.current.revision !== undefined
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                text: Theme.plural(root.hist.slotCount, qsTr("slot"), qsTr("slots")) + " · "
                      + (root.hist.versionCount === 1 ? qsTr("1 version in %1") : qsTr("%1 versions in %2").arg(root.hist.versionCount)).arg(root.hist.slotLabel)
                color: Theme.dim
                font.pixelSize: Theme.roles.small.size
            }
        }
    }
}
