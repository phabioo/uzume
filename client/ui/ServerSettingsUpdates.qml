import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server › Updates (accounts-and-server.md 3q-app-4) from GET /admin/settings (updates), POST /admin/updates/check | install | resume,
// PATCH /admin/settings (channel, automatic install) and GET /admin/updates/versions (read when the page opens, fetches the signed index,
// so it can take a moment). The done line, Update now (primary; "Some apps are too old" asks again with confirm_breaking), Check now,
// Channel, Install updates, the paused card with Resume automatic updates, and the table Earlier versions with a details section and
// Go back... (safety backup first; when the data must be restored the dialog says what is lost and asks for confirm_data_loss).
// Left out, the API carries nothing for it: "fixes N problems" (the notes are a link), the time automatic updates run.
// objectNames: settingsUpdates, updatesDone, updatesNow, updatesCheck, updatesChannelRow, updatesChannel (updatesChannelStable /
// updatesChannelBeta), updatesAutoRow, updatesAuto (updatesAutoOn / updatesAutoOff), updatesPaused, updatesResume, updatesManual,
// versionsTable, versionsDetails, versionsGoBack, versionsNote, breakingDialog, breakingConfirm, rollbackDialog, rollbackConfirm.
ColumnLayout {
    id: root
    objectName: "settingsUpdates"
    required property var pane
    readonly property var u: root.pane.updates
    readonly property var av: root.u.available ?? null
    readonly property var paused: root.u.paused ?? null
    property var versions: null               // AdminUpdateVersions
    property string versionsError: ""
    property int versionIndex: -1
    property string rollbackError: ""
    property bool rollbackBusy: false
    readonly property var versionRows: (root.versions ? root.versions.versions : []).map(v => ({
        version: Theme.shownVersion(v.version), channel: v.channel === "beta" ? qsTr("Beta") : qsTr("Stable"),
        used: root.usedText(v), data: v.data === "refuse" ? qsTr("Can't go back") : v.data === "restore" ? qsTr("Restores data") : qsTr("Keeps data") }))
    readonly property var vsel: root.versions && root.versionIndex >= 0 && root.versionIndex < root.versions.versions.length ? root.versions.versions[root.versionIndex] : null
    readonly property bool canInstall: root.av !== null && root.u.packaged === true && !root.u.request_pending && !(root.u.staged_version)
    spacing: Theme.space4

    Component.onCompleted: {
        root.pane.helpItems = [
            { title: qsTr("Channel"), text: qsTr("Stable gets tested releases. Beta gets new versions first.") },
            { title: qsTr("Going back"), text: qsTr("Going back first writes a safety backup, then installs the older version and pauses automatic updates until you resume them.") }
        ]
        root.loadVersions()
    }
    function usedText(v: var): string {
        if (v.first_used && v.last_used) return qsTr("%1 to %2").arg(root.pane.dayText(v.first_used)).arg(root.pane.dayText(v.last_used))
        return v.published_at ? qsTr("Released %1").arg(root.pane.dayText(v.published_at)) : ""
    }
    function loadVersions() {
        root.pane.call("GET", "/admin/updates/versions", undefined, function (ok, data, code, message) {
            if (ok && data && data.versions !== undefined) {
                root.versions = data
                root.versionsError = ""
            } else {
                root.versionsError = message !== "" ? message : qsTr("The server did not answer.")
            }
        })
    }
    function reloadSettings(done: var) {
        root.pane.call("GET", "/admin/settings", undefined, function (ok, data) {
            if (ok && data && data.hub_name !== undefined) root.pane.settings = data
            if (done) done()
        })
    }
    function check() {
        root.pane.setBusy("check", true)
        root.pane.call("POST", "/admin/updates/check", undefined, function (ok, data, code, message) {
            if (!ok) {
                root.pane.setBusy("check", false)
                root.pane.say(root.pane.failText(qsTr("Could not check for updates"), message), "error")
                return
            }
            checkTimer.tries = 0
            checkTimer.restart()
        })
    }
    Timer {
        id: checkTimer
        property int tries: 0
        interval: 1500
        onTriggered: {
            ++tries
            root.reloadSettings(function () {
                if (checkTimer.tries < 2) {
                    checkTimer.restart()
                } else {
                    root.pane.setBusy("check", false)
                    root.pane.say(root.av ? qsTr("Server %1 is ready.").arg(Theme.shownVersion(root.av.version)) : qsTr("The server is up to date."), "done")
                }
            })
        }
    }
    function install(confirmBreaking: bool) {
        root.pane.setBusy("install", true)
        root.pane.call("POST", "/admin/updates/install", { confirm_breaking: confirmBreaking }, function (ok, data, code, message) {
            root.pane.setBusy("install", false)
            if (ok) {
                root.pane.say(qsTr("Installing %1. The server restarts in a moment and apps reconnect.").arg(data && data.version ? Theme.shownVersion(data.version) : ""), "done")
                root.reloadSettings()
            } else if (code === "conflict" && !confirmBreaking) {
                breakingDialog.open()
            } else {
                root.pane.say(root.pane.failText(qsTr("Could not install the update"), message), "error")
            }
        })
    }
    function setChannel(value: string) {
        root.pane.patchSettings({ update_channel: value }, value === "beta" ? qsTr("The server follows the beta channel.") : qsTr("The server follows the stable channel."))
    }
    function setAuto(value: bool) {
        root.pane.patchSettings({ update_auto: value }, value ? qsTr("Updates install automatically.") : qsTr("The server asks before it installs an update."))
    }
    function resume() {
        root.pane.setBusy("resume", true)
        root.pane.call("POST", "/admin/updates/resume", undefined, function (ok, data, code, message) {
            root.pane.setBusy("resume", false)
            if (ok) {
                root.pane.say(qsTr("Automatic updates are on again."), "done")
                root.reloadSettings()
                root.loadVersions()
            } else {
                root.pane.say(root.pane.failText(qsTr("Could not resume updates"), message), "error")
            }
        })
    }
    function goBack() {
        const v = root.vsel
        if (v === null) return
        root.rollbackError = ""
        root.rollbackBusy = true
        root.pane.call("POST", "/admin/updates/rollback", { version: v.version, confirm_data_loss: v.data === "restore" }, function (ok, data, code, message) {
            root.rollbackBusy = false
            if (ok) {
                rollbackDialog.close()
                root.pane.say(qsTr("Going back to %1. A safety backup is written first; the server restarts in a moment.").arg(Theme.shownVersion(v.version)), "done")
                root.reloadSettings()
                root.loadVersions()
            } else {
                root.rollbackError = message !== "" ? message : qsTr("The server did not answer.")
            }
        })
    }

    // ------------------------------------------------------------ top
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space2
        UzStatusIcon {
            objectName: "updatesDone"
            kind: root.u.last_result && root.u.last_result.ok === false ? "warning" : "done"
            text: root.av ? qsTr("Server %1 is ready.").arg(Theme.shownVersion(root.av.version))
                          : root.u.disabled_reason ? root.u.disabled_reason
                          : qsTr("Server %1 is up to date.").arg(Theme.shownVersion(root.u.current_version))
            maxWidth: 640
        }
        UzStatusIcon {
            visible: root.u.last_error !== undefined && root.u.last_error !== ""
            kind: "warning"
            small: true
            text: qsTr("The last check failed: %1").arg(root.u.last_error ?? "")
            maxWidth: 640
        }
        UzStatusIcon {
            visible: root.u.last_result !== undefined && root.u.last_result.ok === false
            kind: "error"
            small: true
            text: root.u.last_result ? qsTr("The update to %1 failed: %2").arg(Theme.shownVersion(root.u.last_result.version)).arg(root.u.last_result.message) : ""
            maxWidth: 640
        }
        UzMono {
            Layout.fillWidth: true
            visible: root.av !== null
            wrapMode: Text.Wrap
            text: root.av ? qsTr("%1 · %2 · released %3").arg(root.av.channel).arg(root.pane.bytesText(root.av.size)).arg(root.pane.dayText(root.av.published_at)) : ""
        }
        UzStatusIcon {
            visible: root.av !== null && root.av.breaking === true
            kind: "warning"
            small: true
            text: root.av ? qsTr("%1 seen in the last 30 days are too old for this version.").arg(Theme.plural(root.av.breaking_count, qsTr("app"), qsTr("apps"))) : ""
            maxWidth: 640
        }
        UzMono {
            objectName: "updatesManual"
            Layout.fillWidth: true
            visible: root.u.packaged !== true && (root.u.manual_command ?? "") !== ""
            wrapMode: Text.Wrap
            text: qsTr("This server updates by hand: %1").arg(root.u.manual_command ?? "")
        }
        RowLayout {
            spacing: Theme.space1
            UzButton {
                objectName: "updatesNow"
                kind: "primary"
                visible: root.av !== null
                enabled: root.canInstall
                busy: root.pane.isBusy("install")
                text: qsTr("Update now")
                onClicked: root.install(false)
            }
            UzButton {
                objectName: "updatesCheck"
                busy: root.pane.isBusy("check")
                text: qsTr("Check now")
                onClicked: root.check()
            }
        }
        Text {
            visible: root.u.last_check !== undefined
            text: qsTr("Last checked %1").arg(root.pane.dateText(root.u.last_check ?? ""))
            textFormat: Text.PlainText
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
    }

    ColumnLayout {
        objectName: "updatesPaused"
        visible: root.paused !== null
        Layout.fillWidth: true
        spacing: Theme.space1
        UzStatusIcon {
            kind: "warning"
            text: root.paused ? qsTr("Automatic updates are paused. The server went back from %1 to %2 %3.").arg(root.paused.from).arg(root.paused.to).arg(root.pane.dayText(root.paused.at)) : ""
            maxWidth: 640
        }
        UzMono {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: root.paused !== null && (root.paused.lost_since ?? "") !== ""
            text: root.paused ? qsTr("The database was replaced. Changes since %1 are only in the safety backup %2.").arg(root.pane.dateText(root.paused.lost_since ?? "")).arg(root.paused.backup ?? "") : ""
        }
        UzButton {
            objectName: "updatesResume"
            busy: root.pane.isBusy("resume")
            text: qsTr("Resume automatic updates")
            onClicked: root.resume()
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        UzSettingsRow {
            objectName: "updatesChannelRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "channel"
            label: qsTr("Channel")
            description: root.u.channel === "off" ? qsTr("Updates are off on this server.") : qsTr("Stable gets tested releases. Beta gets new versions first.")
            ctrl: "segment"
            controlName: "updatesChannel"
            values: [{ value: "stable", label: qsTr("Stable"), name: "updatesChannelStable" }, { value: "beta", label: qsTr("Beta"), name: "updatesChannelBeta" }]
            current: root.u.channel === "beta" ? "beta" : "stable"
            onValuePicked: value => root.setChannel(value)
        }
        UzSettingsRow {
            objectName: "updatesAutoRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "auto"
            label: qsTr("Install updates")
            description: qsTr("Automatically installs when nobody plays. Ask me: you press Update now.")
            ctrl: "segment"
            controlName: "updatesAuto"
            values: [{ value: "auto", label: qsTr("Automatically"), name: "updatesAutoOn" }, { value: "ask", label: qsTr("Ask me"), name: "updatesAutoOff" }]
            current: root.u.auto === true ? "auto" : "ask"
            onValuePicked: value => root.setAuto(value === "auto")
        }
    }

    // ------------------------------------------------------------ earlier versions
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        UzSectionLabel {
            Layout.fillWidth: true
            text: qsTr("Earlier versions")
            meta: root.versions ? Theme.plural(root.versions.versions.length, qsTr("version"), qsTr("versions")) : ""
        }
        UzStatusIcon {
            objectName: "versionsNote"
            visible: root.versions !== null && root.versions.can_rollback !== true
            kind: "warning"
            small: true
            maxWidth: 640
            text: root.versions ? (root.versions.in_container ? qsTr("This server runs in a container. Go back by pulling the older image tag.")
                                                             : (root.versions.unavailable ?? qsTr("This server can't go back by itself."))) : ""
        }
        UzStateBlock {
            Layout.fillWidth: true
            visible: root.versions === null && root.versionsError === ""
            kind: "loading"
            title: qsTr("Reading the earlier versions")
        }
        UzStatusIcon {
            objectName: "versionsError"
            visible: root.versions === null && root.versionsError !== ""
            kind: "error"
            small: true
            maxWidth: 640
            text: qsTr("Could not read the earlier versions: %1").arg(root.versionsError)
        }
        UzTable {
            objectPrefix: "versionsTable"
            Layout.fillWidth: true
            visible: root.versions !== null
            emptyText: qsTr("No earlier version is available.")
            currentIndex: root.versionIndex
            onPicked: index => root.versionIndex = index
            columns: [
                { key: "version", title: qsTr("Version"), width: 110, mono: true },
                { key: "channel", title: qsTr("Channel"), width: 90 },
                { key: "used", title: qsTr("Used"), fill: 1.5 },
                { key: "data", title: qsTr("Data"), fill: 1 }
            ]
            rows: root.versionRows
            ColumnLayout {
                objectName: "versionsDetails"
                visible: root.vsel !== null
                width: parent ? parent.width : 0
                spacing: Theme.space1
                UzSectionLabel { Layout.fillWidth: true; Layout.topMargin: Theme.space2; text: root.vsel ? Theme.shownVersion(root.vsel.version) : "" }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.ink2
                    text: !root.vsel ? ""
                        : root.vsel.data === "refuse" ? qsTr("Going back isn't safe: %1").arg(root.vsel.reason ?? "")
                        : root.vsel.data === "restore" ? qsTr("The database is replaced by the backup made before the update. Changes since %1 are only in the safety backup.").arg(root.pane.dateText(root.vsel.lost_since ?? ""))
                        : qsTr("Your data stays as it is.")
                }
                UzMono {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    visible: root.vsel !== null && (root.vsel.manual_command ?? "") !== ""
                    text: root.vsel ? qsTr("By hand: %1").arg(root.vsel.manual_command ?? "") : ""
                }
                UzButton {
                    objectName: "versionsGoBack"
                    size: "sm"
                    enabled: root.vsel !== null && root.vsel.data !== "refuse" && root.versions.can_rollback === true
                    text: qsTr("Go back…")
                    onClicked: { root.rollbackError = ""; rollbackDialog.open() }
                }
            }
        }
    }

    // ------------------------------------------------------------ dialogs
    UzDialog {
        id: breakingDialog
        objectName: "breakingDialog"
        parent: root.pane.overlayParent
        label: qsTr("Updates")
        title: qsTr("Some apps are too old")
        text: root.av ? qsTr("%1 seen in the last 30 days can't connect to %2 until they are updated. Install it anyway?").arg(Theme.plural(root.av.breaking_count, qsTr("app"), qsTr("apps"))).arg(Theme.shownVersion(root.av.version)) : ""
        actions: [UzButton { text: qsTr("Cancel"); onClicked: breakingDialog.close() }]
        dangerActions: [
            UzButton {
                objectName: "breakingConfirm"
                kind: "danger"
                text: qsTr("Install")
                onClicked: { breakingDialog.close(); root.install(true) }
            }
        ]
    }
    Binding { target: rollbackDialog; property: "visible"; value: false; when: root.pane.admin.confirmNeeded }
    UzDialog {
        id: rollbackDialog
        objectName: "rollbackDialog"
        parent: root.pane.overlayParent
        size: "lg"
        label: qsTr("Updates")
        title: root.vsel ? qsTr("Go back to %1?").arg(Theme.shownVersion(root.vsel.version)) : ""
        text: !root.versions || !root.vsel ? ""
            : qsTr("The server writes a safety backup first (into %1), installs %2, pauses automatic updates and restarts. %3 connected now are disconnected for a moment.")
                .arg(root.versions.backup_folder).arg(Theme.shownVersion(root.vsel.version)).arg(Theme.plural(root.versions.connected_players, qsTr("app"), qsTr("apps")))
        error: root.rollbackError
        busy: root.rollbackBusy
        UzStatusIcon {
            visible: root.vsel !== null && root.vsel.data === "restore"
            kind: "warning"
            small: true
            maxWidth: 480
            text: root.vsel ? qsTr("The database is replaced by the backup made before the update. Changes since %1 are only in the safety backup.").arg(root.pane.dateText(root.vsel.lost_since ?? "")) : ""
        }
        actions: [UzButton { objectName: "rollbackCancel"; text: qsTr("Cancel"); onClicked: rollbackDialog.close() }]
        dangerActions: [
            UzButton {
                objectName: "rollbackConfirm"
                kind: "danger"
                text: qsTr("Go back")
                onClicked: root.goBack()
            }
        ]
    }
}
