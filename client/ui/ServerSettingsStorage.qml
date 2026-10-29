import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server › Storage (accounts-and-server.md 3q-app-6). GET /admin/storage (API 1.38) carries the split of the server's data: the page shows
// the disk as a storage bar (Games, Saves and snapshots, Save states, Screenshots, Cores and firmware as ink tints, never hues) with a
// hairline legend, the free space, the backups and the backup disk (a warning when it is nearly full), a small table "Screenshots by player
// and game" and the Clean-up suggestions (POST /admin/storage/cleanup {id}): save versions older than 90 days, upload leftovers older than
// 7 days, unused cores. Each "Clean up…" asks inline first and reads the page again afterwards; clean-up never offers saves, current
// saves, manual snapshots and open conflicts. The save history limits (network settings save_keep_recent / daily / weekly) apply at once.
// A server without the endpoint (404) falls back to the disk-only view from GET /admin/status: used / free and the backup disk.
// objectNames: settingsStorage, storageBar, storageBar_other, storageOther, storageKind_<games|saves|states|screenshots|cores>, storageUsed (disk-only view), storageFree,
// storageKind_backups, storageBackupDisk, storageWarning, storageNoData, storageShotsLabel, storageShotsTable (UzTable prefix),
// storageCleanupLabel, storageCleanup_<id>, storageCleanupButton_<id>, storageCleanupConfirm_<id>, storageCleanupConfirmText_<id>,
// storageCleanupCancel_<id>, storageCleanupConfirmButton_<id>, storageCleanupResult, storageNothingToClean (<id> = old_snapshots |
// upload_leftovers | unused_cores), storageRecentRow, storageDailyRow, storageWeeklyRow.
ColumnLayout {
    id: root
    objectName: "settingsStorage"
    required property var pane
    readonly property var st: root.pane.admin.status
    readonly property var disk: root.st.disk ?? null
    readonly property var backupDisk: root.st.backup_disk ?? null
    // GET /admin/storage: null until it answered, and on a server without it (404).
    property var usage: null
    property bool usageLoaded: false
    property string confirmId: ""
    property string resultText: ""
    readonly property real usedFraction: root.disk && root.disk.total_bytes > 0 ? (root.disk.total_bytes - root.disk.free_bytes) / root.disk.total_bytes : 0
    readonly property real backupFraction: root.backupDisk && root.backupDisk.total_bytes > 0 && root.backupDisk.free_bytes !== undefined
                                           ? (root.backupDisk.total_bytes - root.backupDisk.free_bytes) / root.backupDisk.total_bytes : -1
    readonly property var usageDisk: root.usage && root.usage.disk ? root.usage.disk : root.disk
    readonly property var usageBackupDisk: root.usage && root.usage.backup_disk ? root.usage.backup_disk : root.backupDisk
    spacing: Theme.space4

    function kindBytes(ids: var): real {
        let n = 0
        for (const k of (root.usage ? root.usage.kinds ?? [] : [])) if (ids.indexOf(k.id) >= 0) n += k.bytes
        return n
    }
    function hasKind(ids: var): bool {
        for (const k of (root.usage ? root.usage.kinds ?? [] : [])) if (ids.indexOf(k.id) >= 0) return true
        return false
    }
    // The five bars of the design in order; "Saves and snapshots" and "Cores and firmware" add two server kinds each.
    readonly property var kinds: !root.usage ? [] : [
        { id: "games", label: qsTr("Games"), bytes: root.kindBytes(["games"]) },
        { id: "saves", label: qsTr("Saves and snapshots"), bytes: root.kindBytes(["saves", "snapshots"]) },
        { id: "states", label: qsTr("Save states"), bytes: root.kindBytes(["states"]) },
        { id: "screenshots", label: qsTr("Screenshots"), bytes: root.kindBytes(["screenshots"]) },
        { id: "cores", label: qsTr("Cores and firmware"), bytes: root.kindBytes(["cores", "firmware"]) }
    ].map(k => ({ id: k.id, label: k.label, bytes: k.bytes, text: root.pane.bytesText(k.bytes) }))
    readonly property real kindsTotal: root.kinds.reduce((a, k) => a + k.bytes, 0)
    // Without the endpoint the bar shows the disk as used / free (ink 72 %).
    readonly property var barKinds: root.usage ? root.kinds
        : (root.disk ? [{ id: "used", name: "storageUsed", label: qsTr("Used"), bytes: root.disk.total_bytes - root.disk.free_bytes, text: root.pane.bytesText(root.disk.total_bytes - root.disk.free_bytes) }] : [])
    // Used space on the disk that is not Uzume's own data (the kinds above): the bar shows real disk usage, the kinds are Uzume's share.
    readonly property real otherBytes: root.usage && root.usageDisk ? Math.max(0, root.usageDisk.total_bytes - root.usageDisk.free_bytes - root.kindsTotal) : 0
    readonly property real barTotal: root.usageDisk ? root.usageDisk.total_bytes : root.kindsTotal
    readonly property var extraRows: {
        const rows = []
        if (root.usage && root.hasKind(["backups"])) rows.push({ id: "backups", label: qsTr("Backups"), text: root.pane.bytesText(root.kindBytes(["backups"])) })
        if (root.usageBackupDisk && root.backupFractionOf(root.usageBackupDisk) >= 0) {
            const f = root.backupFractionOf(root.usageBackupDisk)
            rows.push({ id: "backupDisk", name: "storageBackupDisk", label: qsTr("Backup disk"), text: qsTr("%1 % · %2 free").arg(Math.round(f * 100)).arg(root.pane.bytesText(root.usageBackupDisk.free_bytes)) })
        }
        return rows
    }
    readonly property var shots: root.usage ? root.usage.screenshots ?? [] : []
    readonly property var shotRows: root.shots.slice(0, 8).map(r => ({ player: r.user_name, game: r.game_name, size: root.pane.bytesText(r.bytes), count: String(r.count) }))
    readonly property var cleanupInfo: ({
        old_snapshots: {
            label: qsTr("Save versions older than 90 days"),
            text: qsTr("Manual snapshots, current saves and open conflicts are never removed."),
            confirm: qsTr("Remove old save versions and free %1? Manual snapshots, current saves and open conflicts are kept."),
            button: qsTr("Remove versions")
        },
        upload_leftovers: {
            label: qsTr("Upload leftovers older than 7 days"),
            text: qsTr("Unfinished uploads that were never completed."),
            confirm: qsTr("Remove unfinished uploads and free %1? Saves are not touched."),
            button: qsTr("Remove leftovers")
        },
        unused_cores: {
            label: qsTr("Unused cores"),
            text: qsTr("Core versions no system uses any more."),
            confirm: qsTr("Remove unused cores and free %1? Saves are not touched."),
            button: qsTr("Remove cores")
        }
    })
    readonly property var suggestions: (root.usage ? root.usage.cleanup ?? [] : []).filter(c => c.bytes > 0 || c.count > 0 || c.id === root.confirmId)

    function backupFractionOf(d: var): real {
        return d && d.total_bytes > 0 && d.free_bytes !== undefined ? (d.total_bytes - d.free_bytes) / d.total_bytes : -1
    }
    function loadUsage() {
        root.pane.call("GET", "/admin/storage", undefined, function (ok, data, code, message, status) {
            root.usageLoaded = true
            root.usage = ok && data ? data : null
        })
    }

    Component.onCompleted: {
        root.pane.helpItems = [
            { title: qsTr("Save history"), text: qsTr("How many older versions of each save the server keeps. 0 keeps all of them. Manual snapshots and open conflicts are never removed.") },
            { title: qsTr("Clean-up"), text: qsTr("Clean-up never offers saves.") }
        ]
        root.pane.admin.refreshStatus()
        root.loadUsage()
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.maximumWidth: 640
        spacing: Theme.space2
        UzStatusIcon {
            objectName: "storageWarning"
            visible: root.backupFractionOf(root.usageBackupDisk) >= 0.9
            kind: "warning"
            maxWidth: 640
            text: root.backupFractionOf(root.usageBackupDisk) >= 0 ? qsTr("The backup disk is %1 % full. The next backup may not fit.").arg(Math.round(root.backupFractionOf(root.usageBackupDisk) * 100)) : ""
        }
        UzSectionLabel {
            Layout.fillWidth: true
            text: qsTr("Disk")
            meta: root.usageDisk ? qsTr("%1 of %2").arg(root.pane.bytesText(root.usageDisk.total_bytes - root.usageDisk.free_bytes)).arg(root.pane.bytesText(root.usageDisk.total_bytes)) : ""
        }
        UzStorageBar {
            Layout.fillWidth: true
            visible: root.usageDisk !== null || root.usage !== null
            kinds: root.barKinds
            totalBytes: root.barTotal
            otherBytes: root.otherBytes
            otherText: root.pane.bytesText(root.otherBytes)
            freeText: root.usageDisk ? root.pane.bytesText(root.usageDisk.free_bytes) : ""
            extraRows: root.extraRows
        }
        UzMono {
            objectName: "storageNoData"
            visible: root.usageDisk === null && root.usage === null
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("This server doesn't report its disk.")
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.maximumWidth: 640
        visible: root.shotRows.length > 0
        spacing: Theme.space1
        UzSectionLabel { objectName: "storageShotsLabel"; Layout.fillWidth: true; text: qsTr("Screenshots by player and game") }
        UzTable {
            objectPrefix: "storageShotsTable"
            selectable: false
            Layout.fillWidth: true
            columns: [
                { key: "player", title: qsTr("Player"), fill: 1 },
                { key: "game", title: qsTr("Game"), fill: 1.4 },
                { key: "count", title: qsTr("Shots"), width: 70, align: "right", mono: true },
                { key: "size", title: qsTr("Size"), width: 90, align: "right", mono: true }
            ]
            rows: root.shotRows
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space15
        visible: root.usage !== null
        UzSectionLabel { objectName: "storageCleanupLabel"; Layout.fillWidth: true; text: qsTr("Clean-up") }
        Repeater {
            model: root.suggestions
            delegate: ColumnLayout {
                id: sug
                required property var modelData
                readonly property var info: root.cleanupInfo[modelData.id] ?? ({ label: modelData.id, text: "", confirm: "", button: "" })
                readonly property string busyKey: "storageCleanup_" + modelData.id
                Layout.fillWidth: true
                spacing: Theme.space15
                UzSettingsRow {
                    objectName: "storageCleanup_" + sug.modelData.id
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    resetMode: "none"
                    label: sug.info.label
                    meta: root.pane.bytesText(sug.modelData.bytes)
                    description: sug.info.text
                    ctrl: "buttons"
                    buttons: [{ name: "storageCleanupButton_" + sug.modelData.id, text: qsTr("Clean up…"), enabled: root.confirmId !== sug.modelData.id && !root.pane.isBusy(sug.busyKey) }]
                    onButtonClicked: { root.resultText = ""; root.confirmId = sug.modelData.id }
                }
                RowLayout {
                    objectName: "storageCleanupConfirm_" + sug.modelData.id
                    Layout.fillWidth: true
                    spacing: Theme.space15
                    visible: root.confirmId === sug.modelData.id
                    UzLabel {
                        objectName: "storageCleanupConfirmText_" + sug.modelData.id
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.ink
                        text: sug.info.confirm.arg(root.pane.bytesText(sug.modelData.bytes))
                    }
                    UzButton {
                        objectName: "storageCleanupCancel_" + sug.modelData.id
                        implicitHeight: 32
                        text: qsTr("Cancel")
                        onClicked: root.confirmId = ""
                    }
                    UzButton {
                        objectName: "storageCleanupConfirmButton_" + sug.modelData.id
                        implicitHeight: 32
                        kind: "primary"
                        busy: root.pane.isBusy(sug.busyKey)
                        text: sug.info.button
                        onClicked: {
                            const id = sug.modelData.id
                            root.confirmId = ""
                            root.pane.setBusy(sug.busyKey, true)
                            root.pane.call("POST", "/admin/storage/cleanup", { id: id }, function (ok, data, code, message) {
                                root.pane.setBusy(sug.busyKey, false)
                                if (ok) {
                                    const n = data && data.removed !== undefined ? data.removed : 0
                                    root.resultText = qsTr("Freed %1 (%2 removed).").arg(root.pane.bytesText(data && data.freed_bytes ? data.freed_bytes : 0)).arg(n)
                                } else {
                                    root.resultText = root.pane.failText(qsTr("Clean-up"), message)
                                }
                                root.loadUsage()
                                root.pane.admin.refreshStatus()
                            })
                        }
                    }
                }
            }
        }
        UzMono {
            objectName: "storageCleanupResult"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.resultText !== ""
            text: root.resultText
        }
        UzMono {
            objectName: "storageNothingToClean"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.suggestions.length === 0 && root.resultText === ""
            text: qsTr("Nothing to clean up.")
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Save history") }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Repeater {
                model: [
                    { key: "save_keep_recent", id: "recent", row: "storageRecentRow", label: qsTr("Keep newest versions"), text: qsTr("Per save slot."), title: qsTr("Keep how many newest versions?") },
                    { key: "save_keep_daily", id: "daily", row: "storageDailyRow", label: qsTr("Keep daily versions"), text: qsTr("Days of which the newest version is kept."), title: qsTr("Keep how many daily versions?") },
                    { key: "save_keep_weekly", id: "weekly", row: "storageWeeklyRow", label: qsTr("Keep weekly versions"), text: qsTr("Weeks of which the newest version is kept."), title: qsTr("Keep how many weekly versions?") }
                ]
                delegate: UzSettingsRow {
                    required property var modelData
                    readonly property var s: root.pane.netSetting(modelData.key)
                    objectName: modelData.row
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    resetMode: s.source === "web" ? "reset" : "none"
                    changed: s.source === "web"
                    idKey: modelData.id
                    label: modelData.label
                    description: (s.value === "0" ? qsTr("All") : s.value) + " · " + modelData.text
                    ctrl: "buttons"
                    buttons: [{ name: "storageChange_" + modelData.id, text: qsTr("Change…") }]
                    onButtonClicked: root.pane.openEdit({
                        title: modelData.title, text: qsTr("0 keeps all of them. Manual snapshots and open conflicts are never removed."),
                        label: modelData.label, value: s.value, mono: true, action: qsTr("Change"),
                        save: function (v) {
                            if (!/^[0-9]+$/.test(v)) { root.pane.editFinished(false, qsTr("Enter a whole number.")); return }
                            root.pane.saveNet(modelData.key, { value: v }, qsTr("Saved %1.").arg(modelData.label.toLowerCase()))
                        }
                    })
                    onResetRequested: root.pane.resetNet(modelData.key, qsTr("%1 is back to the server's own value.").arg(modelData.label))
                }
            }
        }
    }
}
