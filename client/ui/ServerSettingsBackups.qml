import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server › Backups (accounts-and-server.md 3q-app-5) from /admin/backups (list, start), /backups/status (polled every second while a
// backup or restore runs), /backups/cancel, /backups/settings, /backups/restore-result and /backups/{name} (download, restore, delete).
// Back up now (primary) with a progress line and Cancel, the rows Schedule (Off / Daily / Weekly), Time, Day, Keep the last, Include game
// files and Folder (each applies at once), the result of the last backup and restore, and the table Recent backups with a details
// section: Download..., Restore... (explains the safety backup) and Delete.... Restoring and deleting ask Confirm it's you through the
// server.
// Left out, the API carries nothing for it: "Scheduled" as a kind (a file only says whether it is a safety backup), the size game files
// add to a backup.
// objectNames: settingsBackups, backupNow, backupProgress, backupCancel, backupScheduleRow, backupSchedule (backupModeOff / backupModeDaily
// / backupModeWeekly), backupTimeRow, backupDayRow, backupKeepRow, backupRomsRow, backupFolderRow, backupRestoreResult,
// backupDismiss, backupsTable, backupDetails, backupDownload, backupRestore, backupDelete, restoreDialog, restoreConfirm, deleteBackupDialog,
// deleteBackupConfirm, backupNote.
ColumnLayout {
    id: root
    objectName: "settingsBackups"
    required property var pane
    readonly property var st: root.pane.backupStatus
    readonly property var cfg: root.st.settings ?? ({ mode: "off", weekday: 0, time: "03:00", keep: 7, include_roms: false, folder: "" })
    readonly property var running: root.st.running ?? null
    readonly property bool folderSet: root.st.folder_set === true
    property var backups: []
    property bool listLoaded: false
    property int backupIndex: -1
    property string note: ""
    property string noteKind: "done"
    property int downloadId: -1
    property bool downloadBusy: false
    property string restoreError: ""
    property bool restoreBusy: false
    readonly property var rows: root.backups.map(b => ({ name: b.name, when: root.pane.dateText(b.created), size: root.pane.bytesText(b.size),
        kind: b.safety ? qsTr("Safety · before restore") : qsTr("Backup"),
        note: b.error ? qsTr("Damaged") : b.restored ? qsTr("Last restored") : b.includes_roms ? qsTr("With games") : "" }))
    readonly property var sel: root.backupIndex >= 0 && root.backupIndex < root.backups.length ? root.backups[root.backupIndex] : null
    readonly property string weekdayName: Qt.locale().dayName(root.cfg.weekday, Locale.LongFormat)
    spacing: Theme.space4

    Component.onCompleted: {
        root.pane.helpItems = [
            { title: qsTr("What a backup holds"), text: qsTr("The database, saves, plugins and settings. Game files only when you include them. A backup file holds secrets, so keep it as private as the server.") },
            { title: qsTr("Safety backups"), text: qsTr("The server makes one before every restore and before going back to an older version. They are never removed automatically.") }
        ]
        root.pane.loadBackupStatus()
        root.loadList()
    }
    function say(text: string, kind: string) {
        root.note = text
        root.noteKind = kind
    }
    function loadList() {
        root.pane.call("GET", "/admin/backups", undefined, function (ok, data, code, message) {
            if (ok && data && data.backups !== undefined) {
                root.backups = data.backups
                root.listLoaded = true
                if (root.backupIndex >= root.backups.length) root.backupIndex = -1
            } else {
                root.say(root.pane.failText(qsTr("Could not read the backups"), message), "error")
            }
        })
    }
    function putSettings(body: var, okText: string) {
        root.pane.call("PUT", "/admin/backups/settings", body, function (ok, data, code, message) {
            if (ok && data && data.mode !== undefined) {
                const s = Object.assign({}, root.pane.backupStatus)
                s.settings = data
                s.folder_set = data.folder !== ""
                root.pane.backupStatus = s
                root.say(okText, "done")
                root.pane.editFinished(true, "")
                root.pane.loadBackupStatus()
            } else {
                root.say(root.pane.failText(qsTr("Could not save"), message), "error")
                root.pane.editFinished(false, message)
            }
        })
    }
    function start() {
        root.pane.setBusy("backup", true)
        root.pane.call("POST", "/admin/backups", undefined, function (ok, data, code, message) {
            root.pane.setBusy("backup", false)
            if (ok) {
                root.say(qsTr("The backup started. Apps keep playing."), "done")
                root.pane.loadBackupStatus()
            } else {
                root.say(root.pane.failText(qsTr("Could not start the backup"), message), "error")
            }
        })
    }
    function cancel() {
        root.pane.setBusy("cancel", true)
        root.pane.call("POST", "/admin/backups/cancel", undefined, function (ok, data, code, message) {
            root.pane.setBusy("cancel", false)
            if (ok) {
                root.say(qsTr("Canceling. The partial file is removed."), "done")
                root.pane.loadBackupStatus()
            } else {
                root.say(root.pane.failText(qsTr("Could not cancel"), message), "error")
            }
        })
    }
    // Polls while something runs; when it ends the result and the list are read again.
    property bool wasRunning: false
    onRunningChanged: {
        if (root.running !== null) {
            root.wasRunning = true
        } else if (root.wasRunning) {
            root.wasRunning = false
            const last = root.st.last
            if (root.st.restore && root.st.restore.ok === false) {
                root.say(qsTr("The restore failed: %1").arg(root.st.restore.error ?? ""), "error")
            } else if (last && last.ok === false) {
                root.say(last.no_space ? qsTr("The backup folder is full. The server never deletes backups to make room.") : qsTr("The backup failed: %1").arg(last.error ?? ""), "error")
            } else if (last) {
                root.say(qsTr("Backup done: %1 (%2).").arg(last.name ?? "").arg(root.pane.bytesText(last.size ?? 0)), "done")
            }
            root.loadList()
        }
    }
    Timer {
        objectName: "backupPoll"
        interval: 1000
        repeat: true
        running: root.running !== null
        onTriggered: root.pane.loadBackupStatus()
    }
    function dismissRestore() {
        root.pane.call("DELETE", "/admin/backups/restore-result", undefined, function (ok, data, code, message) {
            if (ok) {
                const s = Object.assign({}, root.pane.backupStatus)
                delete s.restore
                root.pane.backupStatus = s
            } else {
                root.say(root.pane.failText(qsTr("Could not dismiss"), message), "error")
            }
        })
    }
    function restore() {
        const b = root.sel
        if (b === null) return
        root.restoreError = ""
        root.restoreBusy = true
        root.pane.call("POST", "/admin/backups/" + encodeURIComponent(b.name) + "/restore", undefined, function (ok, data, code, message) {
            root.restoreBusy = false
            if (ok) {
                restoreDialog.close()
                root.say(qsTr("Restoring %1. The server makes a safety backup, then restarts.").arg(b.name), "done")
                root.pane.loadBackupStatus()
            } else {
                root.restoreError = message !== "" ? message : qsTr("The server did not answer.")
            }
        })
    }
    function remove() {
        const b = root.sel
        if (b === null) return
        root.pane.call("DELETE", "/admin/backups/" + encodeURIComponent(b.name), undefined, function (ok, data, code, message) {
            if (ok) {
                root.backupIndex = -1
                root.say(qsTr("Deleted %1.").arg(b.name), "done")
                root.loadList()
            } else {
                root.say(root.pane.failText(qsTr("Could not delete %1").arg(b.name), message), "error")
            }
        })
    }
    function saveBackup(pathOrUrl: string) {
        const b = root.sel
        if (b === null) return
        root.say("", "done")
        root.downloadBusy = true
        root.downloadId = root.pane.admin.download("/admin/backups/" + encodeURIComponent(b.name) + "/download", pathOrUrl)
    }
    Connections {
        target: root.pane.admin
        function onDownloadFinished(id, ok, filePath, message) {
            if (id !== root.downloadId) return
            root.downloadBusy = false
            root.say(ok ? qsTr("Saved the backup to %1. It holds secrets: keep it private.").arg(filePath)
                        : qsTr("Could not save the backup: %1").arg(message !== "" ? message : qsTr("the server did not answer.")), ok ? "done" : "error")
        }
    }
    Loader {
        id: saveDialog
        active: false
        source: "AdminSaveDialog.qml"
        function open() {
            if (item) {
                item.prepare(root.sel ? root.sel.name : "backup.tar")
                item.open()
            } else {
                active = true
            }
        }
        onLoaded: {
            item.player = root.pane.player
            item.title = qsTr("Save backup")
            item.suffix = "tar"
            item.chosen.connect(root.saveBackup)
            item.prepare(root.sel ? root.sel.name : "backup.tar")
            item.open()
        }
        onStatusChanged: if (status === Loader.Error) root.say(qsTr("The file dialog is not available in this installation."), "error")
    }
    function scheduleText(): string {
        if (root.cfg.mode === "daily") return qsTr("Daily at %1.").arg(root.cfg.time)
        if (root.cfg.mode === "weekly") return qsTr("Every %1 at %2.").arg(root.weekdayName).arg(root.cfg.time)
        return qsTr("Backups only run when you press Back up now.")
    }
    function editTime() {
        root.pane.openEdit({
            title: qsTr("Change the backup time"), text: qsTr("Time of day in the server's time zone, for example 03:00."), label: qsTr("Time"),
            value: root.cfg.time, mono: true, placeholder: "03:00", action: qsTr("Change"),
            save: function (v) { root.putSettings({ time: v }, qsTr("Backups run at %1.").arg(v)) }
        })
    }
    function editKeep() {
        root.pane.openEdit({
            title: qsTr("Keep how many backups?"), text: qsTr("Older regular backups are deleted after a new one succeeded. Safety backups are never deleted."),
            label: qsTr("Backups"), value: String(root.cfg.keep), mono: true, action: qsTr("Change"),
            save: function (v) {
                const n = parseInt(v)
                if (isNaN(n) || n < 1 || n > 365) { root.pane.editFinished(false, qsTr("Enter a number from 1 to 365.")); return }
                root.putSettings({ keep: n }, qsTr("The server keeps the last %1.").arg(Theme.plural(n, qsTr("backup"), qsTr("backups"))))
            }
        })
    }
    function editFolder() {
        root.pane.openEdit({
            title: qsTr("Change the backup folder"), text: qsTr("A folder on the server's computer, ideally on another disk. Empty turns backups off."),
            label: qsTr("Folder"), value: root.cfg.folder, mono: true, placeholder: "/mnt/backup/uzume", action: qsTr("Change"),
            save: function (v) { root.putSettings({ folder: v }, v !== "" ? qsTr("Backups go to %1.").arg(v) : qsTr("Backups are off: no folder is set.")) }
        })
    }

    // ------------------------------------------------------------ top
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space2
        RowLayout {
            spacing: Theme.space1
            UzButton {
                objectName: "backupNow"
                kind: "primary"
                enabled: root.folderSet && root.running === null
                busy: root.pane.isBusy("backup")
                text: qsTr("Back up now")
                onClicked: root.start()
            }
            UzButton {
                objectName: "backupCancel"
                visible: root.running !== null && (root.running.kind === "backup" || root.running.step === 1)
                busy: root.pane.isBusy("cancel")
                text: qsTr("Cancel")
                onClicked: root.cancel()
            }
        }
        ColumnLayout {
            objectName: "backupProgress"
            visible: root.running !== null
            Layout.fillWidth: true
            Layout.maximumWidth: 640
            spacing: Theme.space05
            UzMono {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: !root.running ? "" : (root.running.kind === "restore"
                      ? qsTr("Restoring · step %1 of 3").arg(root.running.step ?? 1)
                      : qsTr("Backing up · %1").arg(root.running.phase))
                      + (root.running.total_bytes > 0 ? " · " + qsTr("%1 of %2").arg(root.pane.bytesText(root.running.done_bytes)).arg(root.pane.bytesText(root.running.total_bytes)) : "")
                      + (root.running.eta_seconds > 0 ? " · " + qsTr("about %1 min left").arg(Math.max(1, Math.round(root.running.eta_seconds / 60))) : "")
            }
            Rectangle {
                Layout.fillWidth: true
                height: 4
                color: Theme.rule2
                Rectangle {
                    height: parent.height
                    width: root.running && root.running.total_bytes > 0 ? parent.width * Math.min(1, root.running.done_bytes / root.running.total_bytes) : 0
                    color: Theme.ink2
                }
            }
        }
        UzStatusIcon {
            objectName: "backupNote"
            visible: root.note !== ""
            kind: root.noteKind
            small: true
            text: root.note
            maxWidth: 640
        }
        UzStatusIcon {
            visible: !root.folderSet
            kind: "warning"
            small: true
            maxWidth: 640
            text: qsTr("No backup folder is set. Choose one to turn backups on.")
        }
        UzStatusIcon {
            visible: (root.st.write_error ?? "") !== ""
            kind: "error"
            small: true
            maxWidth: 640
            text: qsTr("The backup folder can't be written: %1").arg(root.st.write_error ?? "")
        }
        UzStatusIcon {
            visible: root.running === null && root.st.last !== undefined && root.st.last.ok === false
            kind: "error"
            small: true
            maxWidth: 640
            text: root.st.last ? (root.st.last.no_space ? qsTr("The last backup didn't fit in the folder. The server never deletes backups to make room.")
                                                        : qsTr("The last backup failed: %1").arg(root.st.last.error ?? "")) : ""
        }
        UzMono {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: text !== ""
            text: {
                const parts = []
                if (root.st.last_ok) parts.push(qsTr("Last backup %1 · %2").arg(root.pane.dateText(root.st.last_ok.finished)).arg(root.pane.bytesText(root.st.last_ok.size ?? 0)))
                if (root.st.next && root.cfg.mode !== "off") parts.push(qsTr("Next %1").arg(root.pane.dateText(root.st.next)))
                if (root.st.disk_total > 0) parts.push(qsTr("%1 free in the folder").arg(root.pane.bytesText(root.st.disk_free)))
                return parts.join(" · ")
            }
        }
        RowLayout {
            objectName: "backupRestoreResult"
            visible: root.st.restore !== undefined
            Layout.fillWidth: true
            spacing: Theme.space2
            UzStatusIcon {
                Layout.fillWidth: true
                kind: root.st.restore && root.st.restore.ok ? "done" : "error"
                maxWidth: 520
                text: !root.st.restore ? ""
                    : root.st.restore.ok ? qsTr("Restored %1 on %2.").arg(root.st.restore.backup).arg(root.pane.dateText(root.st.restore.at))
                    : qsTr("The restore of %1 failed: %2").arg(root.st.restore.backup).arg(root.st.restore.error ?? "")
            }
            UzButton { objectName: "backupDismiss"; size: "sm"; kind: "ghost"; text: qsTr("Dismiss"); onClicked: root.dismissRestore() }
        }
    }

    // ------------------------------------------------------------ settings
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        UzSettingsRow {
            objectName: "backupScheduleRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "schedule"
            label: qsTr("Schedule")
            description: root.scheduleText()
            ctrl: "segment"
            controlName: "backupSchedule"
            values: [
                { value: "off", label: qsTr("Off"), name: "backupModeOff" },
                { value: "daily", label: qsTr("Daily"), name: "backupModeDaily" },
                { value: "weekly", label: qsTr("Weekly"), name: "backupModeWeekly" }
            ]
            current: root.cfg.mode
            onValuePicked: value => root.putSettings({ mode: value }, value === "off" ? qsTr("Scheduled backups are off.") : qsTr("Backups run %1.").arg(value === "daily" ? qsTr("daily") : qsTr("weekly")))
        }
        UzSettingsRow {
            objectName: "backupTimeRow"
            visible: root.cfg.mode !== "off"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "time"
            label: qsTr("Time")
            description: root.cfg.time
            ctrl: "buttons"
            buttons: [{ name: "backupTimeChange", text: qsTr("Change…") }]
            onButtonClicked: root.editTime()
        }
        UzSettingsRow {
            objectName: "backupDayRow"
            visible: root.cfg.mode === "weekly"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "day"
            label: qsTr("Day")
            ctrl: "select"
            controlName: "backupDay"
            values: [0, 1, 2, 3, 4, 5, 6].map(d => ({ value: String(d), label: Qt.locale().dayName(d, Locale.LongFormat) }))
            current: String(root.cfg.weekday)
            onValuePicked: value => root.putSettings({ weekday: parseInt(value) }, qsTr("Backups run every %1.").arg(Qt.locale().dayName(parseInt(value), Locale.LongFormat)))
        }
        UzSettingsRow {
            objectName: "backupKeepRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "keep"
            label: qsTr("Keep the last")
            description: Theme.plural(root.cfg.keep, qsTr("backup"), qsTr("backups"))
            ctrl: "buttons"
            buttons: [{ name: "backupKeepChange", text: qsTr("Change…") }]
            onButtonClicked: root.editKeep()
        }
        UzSettingsRow {
            objectName: "backupRomsRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "roms"
            label: qsTr("Include game files")
            description: qsTr("Makes each backup much bigger. Off: you add the games again after a restore.")
            ctrl: "toggle"
            controlName: "backupRoms"
            checked: root.cfg.include_roms === true
            onToggled: value => root.putSettings({ include_roms: value }, value ? qsTr("Backups include the game files.") : qsTr("Backups leave out the game files."))
        }
        UzSettingsRow {
            objectName: "backupFolderRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "folder"
            label: qsTr("Folder")
            descriptionName: "backupFolder"
            description: root.cfg.folder !== "" ? root.cfg.folder : qsTr("Not set")
            ctrl: "buttons"
            buttons: [{ name: "backupFolderChange", text: qsTr("Change…") }]
            onButtonClicked: root.editFolder()
        }
    }

    // ------------------------------------------------------------ recent backups
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Recent backups · %1").arg(root.backups.length) }
        UzTable {
            objectPrefix: "backupsTable"
            Layout.fillWidth: true
            emptyText: root.listLoaded ? qsTr("No backups yet.") : qsTr("Reading the backups…")
            currentIndex: root.backupIndex
            onPicked: index => root.backupIndex = index
            columns: [
                { key: "when", title: qsTr("When"), fill: 1.3 },
                { key: "size", title: qsTr("Size"), width: 90, align: "right", mono: true },
                { key: "kind", title: qsTr("Kind"), fill: 1.2 },
                { key: "note", title: "", fill: 0.9 }
            ]
            rows: root.rows
            ColumnLayout {
                objectName: "backupDetails"
                visible: root.sel !== null
                width: parent ? parent.width : 0
                spacing: Theme.space1
                UzSectionLabel { Layout.fillWidth: true; Layout.topMargin: Theme.space2; text: root.sel ? root.sel.name : "" }
                UzMono {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: text !== ""
                    text: root.sel ? [root.sel.contents ?? "", root.sel.hub_version ? qsTr("Server %1").arg(Theme.shownVersion(root.sel.hub_version)) : ""].filter(t => t !== "").join(" · ") : ""
                }
                UzStatusIcon {
                    visible: root.sel !== null && (root.sel.error ?? "") !== ""
                    kind: "error"
                    small: true
                    maxWidth: 640
                    text: root.sel ? qsTr("This file is damaged or unreadable: %1").arg(root.sel.error ?? "") : ""
                }
                Text {
                    visible: root.st.can_restore === false
                    text: qsTr("This server can't restart itself, so it can't restore from the app.")
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.muted
                }
                RowLayout {
                    spacing: Theme.space1
                    UzButton { objectName: "backupDownload"; size: "sm"; busy: root.downloadBusy; text: qsTr("Download…"); onClicked: saveDialog.open() }
                    UzButton {
                        objectName: "backupRestore"
                        size: "sm"
                        enabled: root.st.can_restore === true && root.running === null && root.sel !== null && (root.sel.error ?? "") === ""
                        text: qsTr("Restore…")
                        onClicked: { root.restoreError = ""; restoreDialog.open() }
                    }
                    UzButton { objectName: "backupDelete"; size: "sm"; kind: "danger"; enabled: root.running === null; text: qsTr("Delete…"); onClicked: deleteDialog.open() }
                }
            }
        }
    }

    // ------------------------------------------------------------ dialogs
    Binding { target: restoreDialog; property: "visible"; value: false; when: root.pane.admin.confirmNeeded }
    UzDialog {
        id: restoreDialog
        objectName: "restoreDialog"
        parent: root.pane.overlayParent
        size: "lg"
        label: qsTr("Backups")
        title: root.sel ? qsTr("Restore %1?").arg(root.pane.dateText(root.sel.created)) : ""
        text: qsTr("The server first makes a safety backup of itself as it is now, so you can undo this. It then checks every file of this backup and restarts. Running Sessions end and apps reconnect. Everything changed since this backup was made is replaced.")
        error: root.restoreError
        busy: root.restoreBusy
        actions: [UzButton { objectName: "restoreCancel"; text: qsTr("Cancel"); onClicked: restoreDialog.close() }]
        dangerActions: [
            UzButton {
                objectName: "restoreConfirm"
                kind: "danger"
                text: qsTr("Restore")
                onClicked: root.restore()
            }
        ]
    }
    UzDialog {
        id: deleteDialog
        objectName: "deleteBackupDialog"
        parent: root.pane.overlayParent
        label: qsTr("Backups")
        title: root.sel ? qsTr("Delete %1?").arg(root.sel.name) : ""
        text: qsTr("The file is removed from the backup folder. This can't be undone.")
        actions: [UzButton { text: qsTr("Cancel"); onClicked: deleteDialog.close() }]
        dangerActions: [
            UzButton {
                objectName: "deleteBackupConfirm"
                kind: "danger"
                text: qsTr("Delete")
                onClicked: { deleteDialog.close(); root.remove() }
            }
        ]
    }
}
