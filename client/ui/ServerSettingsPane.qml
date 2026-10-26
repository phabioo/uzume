import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server settings (accounts-and-server.md 3q-app-1 to 3q-app-7), the "Settings for: Server" side of the Settings screen.
//
// The pane has two faces: `navDelegate` is the item of the second column (General · Network & sharing · Sign-in · Updates · Backups ·
// Storage · Diagnostics, Mono value at the right, Yamabuki marker on the open page; SettingsScreen puts it into a Repeater over
// `pages`), and the pane itself is the page area (title, result line, "Restart the server to apply" line, the page, help).
// One page is created while it is shown. The pane owns what the pages share: the data of GET /admin/settings, GET
// /admin/accounts/settings and GET /admin/backups/status (read again when the pane opens), the request plumbing (`call`,
// `saveNet`, `patchSettings`), the result line (`say`), the generic edit dialog (`openEdit`), the restart line and the formatters.
// Pages: ServerSettingsGeneral / Network / SignIn / Updates / Backups / Storage / Diagnostics (each `pane: root`).
// Dangerous steps rely on the server's 403 confirmation_required: AdminController parks the request, the global dialog asks for
// the password and the same request runs again.
// objectNames: serverSettingsPane, serverSettingsNav_<id>, serverSettingsNote, serverSettingsRestart (line), serverSettingsRestartButton,
// serverSettingsHelp, settingsEditDialog, settingsEditField, settingsEditField2, settingsEditSave, settingsEditCancel.
ColumnLayout {
    id: root
    objectName: "serverSettingsPane"
    required property PlayerController player
    readonly property AdminController admin: root.player.admin
    property string page: "general"
    readonly property var pages: [
        { id: "general", name: qsTr("General") },
        { id: "network", name: qsTr("Network & sharing") },
        { id: "signin", name: qsTr("Sign-in") },
        { id: "updates", name: qsTr("Updates") },
        { id: "backups", name: qsTr("Backups") },
        { id: "storage", name: qsTr("Storage") },
        { id: "diagnostics", name: qsTr("Diagnostics") }
    ]
    readonly property string pageName: {
        for (const p of root.pages) if (p.id === root.page) return p.name
        return ""
    }

    // ------------------------------------------------------------ shared data
    property string phase: "loading"          // loading | ready | error
    property string loadError: ""
    property var settings: ({})               // AdminSettings
    property var accounts: ({})               // AdminAccountSettings
    property var backupStatus: ({})           // AdminBackupStatus
    readonly property bool ready: root.phase === "ready" && root.settings.hub_name !== undefined
    readonly property var network: root.settings.network ?? ({})
    readonly property var updates: root.settings.updates ?? ({})
    readonly property var security: root.settings.security ?? ({})
    readonly property var pendingRestart: root.network.pending_restart ?? []

    property string note: ""
    property string noteKind: "done"
    property var calls: ({})
    property var busy: ({})
    property bool restarting: false
    property string helpTitle: ""
    property var helpItems: []                // set by the shown page: [{ title, text }]

    readonly property Item overlayParent: Window.window ? Window.window.contentItem : root   // dialogs fill the window
    spacing: Theme.space4
    Layout.fillWidth: true
    Layout.minimumWidth: 0

    // ------------------------------------------------------------ helpers
    function netSetting(key: string): var {
        for (const s of (root.network.settings ?? [])) if (s.key === key) return s
        return ({ key: key, label: key, value: "", running_value: "", default_value: "", source: "env", needs_restart: false, restart_pending: false })
    }
    function netValue(key: string): string { return root.netSetting(key).value ?? "" }
    // A copy of `obj` with one key set (the one place that builds changed copies of the settings objects).
    function copyWith(obj: var, key: string, value: var): var {
        const out = {}
        for (const k in obj) out[k] = obj[k]
        out[key] = value
        return out
    }
    function isOn(v: string): bool { return v === "true" || v === "1" || v === "on" }
    function say(text: string, kind: string) {
        root.note = text
        root.noteKind = kind
    }
    function failText(what: string, message: string): string {
        return message !== "" ? qsTr("%1: %2").arg(what).arg(message) : qsTr("%1: the server did not answer.").arg(what)
    }
    function setBusy(key: string, on: bool) {
        const b = Object.assign({}, root.busy)
        if (on) b[key] = true
        else delete b[key]
        root.busy = b
    }
    function isBusy(key: string): bool { return root.busy[key] === true }
    function remember(id: int, done: var) {
        const c = Object.assign({}, root.calls)
        c[id] = done
        root.calls = c
    }
    // done(ok, data, code, message)
    function call(method: string, path: string, body: var, done: var) {
        root.remember(root.admin.request(method, path, body), done)
    }
    Connections {
        target: root.admin
        function onReplied(id, ok, status, data, code, message) {
            const cb = root.calls[id]
            if (cb === undefined) return
            const c = Object.assign({}, root.calls)
            delete c[id]
            root.calls = c
            cb(ok, data, code, message, status)
        }
    }
    function bytesText(n: real): string {
        const gib = Math.pow(1024, 3), mib = Math.pow(1024, 2)
        const trim = (v) => { const t = v.toFixed(1); return t.endsWith(".0") ? t.slice(0, -2) : t }
        if (n >= gib) return qsTr("%1 GB").arg(trim(n / gib))
        if (n >= mib) return qsTr("%1 MB").arg(Math.round(n / mib))
        return qsTr("%1 KB").arg(Math.max(1, Math.round(n / 1024)))
    }
    function dateText(iso: string): string {
        const d = new Date(iso)
        return isNaN(d.getTime()) ? "" : Qt.formatDateTime(d, "d MMM yyyy, HH:mm")
    }
    function dayText(iso: string): string {
        const d = new Date(iso)
        return isNaN(d.getTime()) ? "" : Qt.formatDate(d, "d MMM yyyy")
    }
    function copyText(text: string) {
        clip.text = text
        clip.selectAll()
        clip.copy()
    }

    // ------------------------------------------------------------ loading
    function load() {
        if (!root.admin.available) return
        root.call("GET", "/admin/settings", undefined, function (ok, data, code, message) {
            if (ok && data && data.hub_name !== undefined) {
                root.settings = data
                root.phase = "ready"
            } else if (root.phase !== "ready") {
                root.phase = "error"
                root.loadError = message
            } else {
                root.say(root.failText(qsTr("Could not read the settings"), message), "error")
            }
        })
        root.loadAccounts()
        root.loadBackupStatus()
    }
    function loadAccounts() {
        root.call("GET", "/admin/accounts/settings", undefined, function (ok, data) { if (ok && data) root.accounts = data })
    }
    function loadBackupStatus(done: var) {
        root.call("GET", "/admin/backups/status", undefined, function (ok, data) {
            if (ok && data) root.backupStatus = data
            if (done) done(ok, data)
        })
    }
    Component.onCompleted: if (root.visible) root.load()
    onVisibleChanged: if (visible) root.load()
    Connections {
        target: root.admin
        function onAvailableChanged() { if (root.visible) root.load() }
    }
    onPageChanged: root.note = ""

    // ------------------------------------------------------------ saving
    // PATCH /admin/settings (name, uploads, update channel / auto). `done(ok)` is optional.
    function patchSettings(body: var, okText: string, done: var) {
        root.call("PATCH", "/admin/settings", body, function (ok, data, code, message) {
            if (ok && data && data.hub_name !== undefined) {
                root.settings = data
                root.say(okText, "done")
                root.editFinished(true, "")
            } else {
                root.say(root.failText(qsTr("Could not save"), message), "error")
                root.editFinished(false, message)
            }
            if (done) done(ok)
        })
    }
    // PUT /admin/settings/network/<key>; the answer is the whole network block.
    function saveNet(key: string, body: var, okText: string, done: var) {
        root.call("PUT", "/admin/settings/network/" + key, body, function (ok, data, code, message) {
            root.afterNet(ok, data, message, okText, done)
        })
    }
    function resetNet(key: string, okText: string, done: var) {
        root.call("DELETE", "/admin/settings/network/" + key, undefined, function (ok, data, code, message) {
            root.afterNet(ok, data, message, okText, done)
        })
    }
    function afterNet(ok: bool, data: var, message: string, okText: string, done: var) {
        if (ok && data && data.settings !== undefined) {
            root.settings = root.copyWith(root.settings, "network", data)
            root.say(okText + (root.pendingRestart.length > 0 ? " " + qsTr("It applies after a restart.") : ""), "done")
            root.editFinished(true, "")
        } else {
            root.say(root.failText(qsTr("Could not save"), message), "error")
            root.editFinished(false, message)
        }
        if (done) done(ok)
    }
    // The external TURN servers live inside the network block.
    function applyExternal(data: var) {
        root.settings = root.copyWith(root.settings, "network", root.copyWith(root.network, "external_turn", data))
    }

    // ------------------------------------------------------------ restart
    function restart() {
        root.setBusy("restart", true)
        root.call("POST", "/admin/settings/network/restart", undefined, function (ok, data, code, message) {
            root.setBusy("restart", false)
            if (ok) {
                root.restarting = true
                const url = data && data.url ? data.url : ""
                root.say(data && data.port_changed
                         ? (url !== "" ? qsTr("Restarting. The listen port changed, so apps can't reconnect by themselves: use %1.").arg(url)
                                       : qsTr("Restarting. The listen port changed, so apps can't reconnect by themselves."))
                         : qsTr("Restarting. Apps reconnect in a moment."), "done")
                reloadTimer.tries = 0
                reloadTimer.restart()
            } else {
                root.say(root.failText(qsTr("Could not restart the server"), message), "error")
            }
        })
    }
    Timer {
        id: reloadTimer
        property int tries: 0
        interval: 4000
        onTriggered: {
            ++tries
            root.call("GET", "/admin/settings", undefined, function (ok, data) {
                if (ok && data && data.hub_name !== undefined) {
                    root.settings = data
                    root.restarting = false
                } else if (reloadTimer.tries < 8) {
                    reloadTimer.restart()
                } else {
                    root.restarting = false
                }
            })
        }
    }

    // ------------------------------------------------------------ generic edit dialog
    // openEdit({ title, text, label, value, value2, label2, mono, placeholder, save: function (v1, v2) })
    property var editSpec: ({})
    property string editError: ""
    property bool editBusy: false
    function openEdit(spec: var) {
        root.editSpec = spec
        root.editError = ""
        editField.text = spec.value ?? ""
        editField2.text = spec.value2 ?? ""
        editDialog.open()
        editField.forceActiveFocus()
    }
    function editFinished(ok: bool, message: string) {
        // Only the answer to the save of this dialog counts: a late answer of an earlier request (a toggle, another page) must
        // not close a dialog that was opened in the meantime.
        if (!editDialog.shown || !root.editBusy) return
        root.editBusy = false
        if (ok) {
            editDialog.close()
        } else {
            root.editError = message !== "" ? message : qsTr("The server did not answer.")
        }
    }
    UzDialog {
        id: editDialog
        objectName: "settingsEditDialog"
        parent: root.overlayParent
        label: qsTr("Server settings")
        title: root.editSpec.title ?? ""
        text: root.editSpec.text ?? ""
        error: root.editError
        busy: root.editBusy
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.space05
            Text {
                visible: (root.editSpec.label ?? "") !== ""
                text: root.editSpec.label ?? ""
                textFormat: Text.PlainText
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
            UzField {
                id: editField
                objectName: "settingsEditField"
                Layout.fillWidth: true
                fill: Theme.ground
                mono: root.editSpec.mono === true
                placeholderText: root.editSpec.placeholder ?? ""
                onAccepted: editSave.clicked()
            }
            Text {
                visible: (root.editSpec.label2 ?? "") !== ""
                text: root.editSpec.label2 ?? ""
                textFormat: Text.PlainText
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
            UzField {
                id: editField2
                objectName: "settingsEditField2"
                visible: (root.editSpec.label2 ?? "") !== ""
                Layout.fillWidth: true
                fill: Theme.ground
                mono: true
                onAccepted: editSave.clicked()
            }
        }
        actions: [
            UzButton {
                id: editSave
                objectName: "settingsEditSave"
                kind: "primary"
                text: root.editSpec.action ?? qsTr("Save")
                onClicked: {
                    root.editError = ""
                    root.editBusy = true
                    root.editSpec.save(editField.text.trim(), editField2.text.trim())
                }
            },
            UzButton { objectName: "settingsEditCancel"; text: qsTr("Cancel"); onClicked: editDialog.close() }
        ]
    }
    // A dialog that sent a request needing Confirm it's you steps aside while the global dialog asks (it would cover it).
    Binding { target: editDialog; property: "visible"; value: false; when: root.admin.confirmNeeded }
    TextEdit { id: clip; visible: false }

    // ------------------------------------------------------------ second column item
    property Component navDelegate: UzColumnNavRow {
        id: navItem
        required property var modelData
        objectName: "serverSettingsNav_" + modelData.id
        label: navItem.modelData.name
        selected: root.page === modelData.id
        onClicked: root.page = navItem.modelData.id
        UzLabel {
            objectName: "serverSettingsNavStatus_" + navItem.modelData.id
            Layout.fillWidth: true
            font.family: Theme.mono
            font.pixelSize: Theme.roles.small.size
            elide: Text.ElideRight
            color: Theme.muted
            text: root.navStatus(navItem.modelData.id)
        }
    }
    function navStatus(id: string): string {
        if (!root.ready) return ""
        switch (id) {
        case "network": return root.isOn(root.netValue("network_sharing")) ? qsTr("Internet") : ""
        case "signin": return root.accounts.require_passwords === true ? qsTr("Password") : ""
        case "updates": return root.updates.current_version ?? ""
        case "backups": {
            const b = root.backupStatus.settings
            return b && b.mode !== "off" && root.backupStatus.folder_set ? b.time : ""
        }
        case "storage": {
            const d = root.admin.status.disk
            return d && d.total_bytes > 0 ? qsTr("%1 %").arg(Math.round((d.total_bytes - d.free_bytes) * 100 / d.total_bytes)) : ""
        }
        default: return ""
        }
    }

    // ------------------------------------------------------------ page area
    UzStateBlock {
        objectName: "serverSettingsUnavailable"
        Layout.fillWidth: true
        Layout.maximumWidth: 520
        visible: !root.admin.available
        kind: "empty"
        title: qsTr("This server can't be managed from the app yet")
        text: qsTr("It does not offer the admin pages. Update the server, then come back.")
    }
    UzStateBlock {
        objectName: "serverSettingsLoading"
        Layout.fillWidth: true
        Layout.maximumWidth: 520
        visible: root.admin.available && root.phase === "loading"
        kind: "loading"
        title: qsTr("Loading server settings")
    }
    UzStateBlock {
        objectName: "serverSettingsError"
        Layout.fillWidth: true
        Layout.maximumWidth: 520
        visible: root.admin.available && root.phase === "error"
        kind: "error"
        title: qsTr("Can't read the server settings")
        text: root.loadError !== "" ? root.loadError : qsTr("The server did not answer.")
        action: qsTr("Try again")
        onActionClicked: { root.phase = "loading"; root.load() }
    }

    ColumnLayout {
        objectName: "serverSettingsPage"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: root.admin.available && root.ready
        spacing: Theme.space4

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.space1
            Eyebrow { Layout.fillWidth: true; text: root.pageName }
            UzStatusIcon {
                objectName: "serverSettingsNote"
                visible: root.note !== ""
                kind: root.noteKind
                small: true
                text: root.note
                maxWidth: 640
            }
            RowLayout {
                objectName: "serverSettingsRestart"
                visible: root.pendingRestart.length > 0 && (root.page === "general" || root.page === "network")
                Layout.fillWidth: true
                spacing: Theme.space2
                UzStatusIcon {
                    Layout.fillWidth: true
                    kind: "warning"
                    small: true
                    text: qsTr("Restart the server to apply: %1.").arg(root.pendingRestart.join(", "))
                    maxWidth: 520
                }
                UzButton {
                    objectName: "serverSettingsRestartButton"
                    size: "sm"
                    visible: root.network.can_restart === true
                    busy: root.isBusy("restart") || root.restarting
                    text: qsTr("Restart")
                    onClicked: root.restart()
                }
                Text {
                    visible: root.network.can_restart !== true
                    text: qsTr("This server can't restart itself. Restart it on the device.")
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.muted
                }
            }
        }

        Loader {
            id: pageLoader
            objectName: "serverSettingsPageLoader"
            active: root.admin.available && root.ready
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            sourceComponent: {
                switch (root.page) {
                case "network": return networkPage
                case "signin": return signInPage
                case "updates": return updatesPage
                case "backups": return backupsPage
                case "storage": return storagePage
                case "diagnostics": return diagnosticsPage
                default: return generalPage
                }
            }
        }

        // Help of the shown page (the open column of the boards; the Settings screen has none, so it sits under the page).
        ColumnLayout {
            objectName: "serverSettingsHelp"
            Layout.fillWidth: true
            Layout.maximumWidth: 640
            visible: root.helpItems.length > 0
            spacing: 0
            UzSectionLabel { Layout.fillWidth: true; Layout.bottomMargin: Theme.space1; text: qsTr("Help") }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule }
            Repeater {
                model: root.helpItems
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.space15
                    Layout.bottomMargin: Theme.space15
                    spacing: 2
                    Text {
                        visible: modelData.title !== undefined
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: modelData.title ?? ""
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        color: Theme.ink
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: modelData.text
                        textFormat: Text.PlainText
                        font.pixelSize: 12
                        lineHeight: 18
                        lineHeightMode: Text.FixedHeight
                        color: Theme.muted
                    }
                    Rectangle { Layout.fillWidth: true; Layout.topMargin: Theme.space15; height: 1; color: Theme.rule2 }
                }
            }
        }
    }

    Component { id: generalPage; ServerSettingsGeneral { pane: root } }
    Component { id: networkPage; ServerSettingsNetwork { pane: root } }
    Component { id: signInPage; ServerSettingsSignIn { pane: root } }
    Component { id: updatesPage; ServerSettingsUpdates { pane: root } }
    Component { id: backupsPage; ServerSettingsBackups { pane: root } }
    Component { id: storagePage; ServerSettingsStorage { pane: root } }
    Component { id: diagnosticsPage; ServerSettingsDiagnostics { pane: root } }
}
