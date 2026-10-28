import QtQuick
import QtQuick.Layouts
import Uzume.App

// Plugins of the server (accounts-and-server.md 4i-app-1 and 4i-app-3) from GET /admin/plugins. Table of the plugins this server
// offers (newest offered version of each: name, version, origin Tag Official / Not signed, permissions) with a details section under
// it (Remove from server...), the table "Official index" (Add installs from the signed plugin index) and the header action "Upload
// plugin...": the file is staged (POST /admin/plugins/stage), the dialog shows its manifest and one Checkbox per permission, and
// "Offer to everyone" offers it (POST .../offer) once every permission is ticked. Cancel discards the staged file.
// What the API does not carry is left out: no per-plugin On switch and no user count (there is no enabled flag), no "Use previous
// version" (no route sets the served version), no "Download file" (the file route is not an admin route), no "needs server x.y"
// state (the catalog carries the plugin API number, not a server version).
// objectNames: serverPluginsPage, pluginsTable (rows pluginsTableRow_<n>), pluginsDetails, pluginRemove, indexTable, indexAdd_<id>,
// pluginUpload, indexRefresh, pluginsNote, pluginsIndexError, stageDialog, stagePermission_<n>, stageOffer, stageCancel, stageError,
// removePluginDialog, removePluginConfirm.
AdminPage {
    id: root
    objectName: "serverPluginsPage"
    readonly property AdminController admin: root.player.admin

    property string phase: "loading"          // loading | ready | error
    property string loadError: ""
    property var offered: []                  // AdminPlugin, oldest version first within a plugin
    property var index: ({})
    property int pluginIndex: -1
    property string note: ""
    property string noteKind: "done"
    property var busy: ({})
    property var calls: ({})
    property var stage: null                  // AdminPluginStage while the dialog is open
    property var ticks: ({})                  // permission -> bool
    property string stageError: ""
    property bool stageBusy: false
    property var engineTool: null              // AdminTool of the randomizer engine, read while the Randomizer plugin is selected (10h-3)

    readonly property var pluginRows: root.rowsOf(root.offered)
    readonly property var selected: root.pluginIndex >= 0 && root.pluginIndex < root.pluginRows.length ? root.pluginRows[root.pluginIndex] : null
    readonly property var catalogRows: (root.index.catalog ?? []).map(e => ({ id: e.id, name: e.name, version: e.version,
                                                                              description: e.state === "replace" && e.replaces
                                                                                           ? (e.description ? e.description + " " : "") + qsTr("Replaces %1 %2").arg(e.replaces.name).arg(e.replaces.version)
                                                                                           : e.description,
                                                                              state: e.state, installedVersion: e.installed_version ?? "",
                                                                              replacesId: e.replaces ? e.replaces.id : "" }))
    // Old plugin id -> name of the index plugin that replaces it (catalog state "replace").
    readonly property var replacedBy: {
        const out = ({})
        for (const e of (root.index.catalog ?? []))
            if (e.state === "replace" && e.replaces && e.replaces.id) out[e.replaces.id] = e.name
        return out
    }
    readonly property bool allTicked: root.stage !== null && (root.stage.permissions ?? []).every(p => root.ticks[p] === true)

    title: qsTr("Plugins")
    label: qsTr("Server")
    area: qsTr("Plugins")
    helpItems: [
        { title: qsTr("Offered to everyone"), text: qsTr("People see these plugins in their own plugin list and turn them on themselves. A file Uzume hasn't checked shows a warning first.") },
        { title: qsTr("Plugin ids"), text: qsTr("A run plugin and its shared base are separate plugins, for example uzume.soullink and uzume.soullink-core. Add both.") }
    ]
    helpLines: root.index.last_check ? [qsTr("Index checked %1").arg(root.clockText(root.index.last_check))] : []

    // ------------------------------------------------------------ data
    function rowsOf(list: var): var {
        const order = []
        const byId = {}
        for (const p of list) {
            const id = p.info.plugin_id
            if (byId[id] === undefined) {
                byId[id] = []
                order.push(id)
            }
            byId[id].push(p)
        }
        return order.map(id => {
            const versions = byId[id]
            const latest = versions[versions.length - 1]
            const previous = versions.length > 1 ? versions[versions.length - 2] : null
            return { plugin_id: id, name: latest.info.name, version: latest.info.version, official: latest.info.origin === "official",
                     origin: latest.info.origin, permissions: root.permissionsText(latest.info.permissions ?? []), source: latest.source,
                     writes: root.changesGame(latest.info.permissions ?? []), permissionCount: (latest.info.permissions ?? []).length,
                     uploadedBy: latest.uploaded_by_name ?? "", previous: previous ? previous.info.version : "",
                     previousDate: previous ? root.dateText(previous.info.created_at) : "" }
        })
    }
    function permissionLabel(p: string): string {
        switch (p) {
        case "memory.read": return qsTr("Read game memory")
        case "rom.read": return qsTr("Read the game file")
        case "ui": return qsTr("Show panels in game")
        case "run.state": return qsTr("Share run progress")
        case "video.capture": return qsTr("Capture the game picture")
        case "variants.build": return qsTr("Build game variants")
        case "memory.write": return qsTr("Change game memory")
        case "cheats": return qsTr("Turn cheats on and off")
        case "store": return qsTr("Keep its data with your account")
        case "sound": return qsTr("Play sounds")
        default: return p
        }
    }
    // cheats and memory.write change the running game and its saves: they get a warning tag and one explanatory line.
    function changesGame(list: var): bool { return list.indexOf("cheats") >= 0 || list.indexOf("memory.write") >= 0 }
    function permissionsText(list: var): string {
        return list.length === 0 ? qsTr("None") : list.map(p => root.permissionLabel(p)).join(", ")
    }
    function dateText(iso: string): string {
        const d = new Date(iso)
        return isNaN(d.getTime()) ? "" : Qt.formatDate(d, "d MMM")
    }
    function clockText(iso: string): string {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const now = new Date()
        const same = d.getFullYear() === now.getFullYear() && d.getMonth() === now.getMonth() && d.getDate() === now.getDate()
        return same ? qsTr("today %1").arg(Qt.formatTime(d, "HH:mm")) : Qt.formatDateTime(d, "d MMM HH:mm")
    }
    function sourceText(row: var): string {
        if (row.source === "index") return qsTr("Installed from the official index.")
        if (row.source === "import") return qsTr("Imported on the server.")
        return row.uploadedBy !== "" ? qsTr("Uploaded by %1.").arg(row.uploadedBy) : qsTr("Uploaded by an admin.")
    }
    function say(text: string, kind: string) {
        root.note = text
        root.noteKind = kind
    }
    function setBusy(key: string, on: bool) {
        const b = Object.assign({}, root.busy)
        if (on) b[key] = true
        else delete b[key]
        root.busy = b
    }
    function failText(what: string, message: string): string {
        return message !== "" ? qsTr("%1: %2").arg(what).arg(message) : qsTr("%1: the server did not answer.").arg(what)
    }
    function remember(id: int, done: var) {
        const c = Object.assign({}, root.calls)
        c[id] = done
        root.calls = c
    }
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
            cb(ok, data, code, message)
        }
    }

    // ------------------------------------------------------------ actions
    function load() {
        root.call("GET", "/admin/plugins", undefined, function (ok, data, code, message) {
            if (ok && data && data.plugins !== undefined) {
                root.offered = data.plugins
                root.index = data.index ?? ({})
                root.phase = "ready"
                if (root.pluginIndex >= root.pluginRows.length) root.pluginIndex = -1
            } else if (root.phase !== "ready") {
                root.phase = "error"
                root.loadError = message
            } else {
                root.say(root.failText(qsTr("Could not read the plugins"), message), "error")
            }
        })
    }
    function refreshIndex() {
        root.say("", "done")
        root.setBusy("refresh", true)
        root.call("POST", "/admin/plugin-index/refresh", undefined, function (ok, data, code, message) {
            root.setBusy("refresh", false)
            if (ok && data) {
                root.index = data
                root.say(data.last_error ? "" : qsTr("The index is up to date."), "done")
            } else {
                root.say(root.failText(qsTr("Could not check the index"), message), "error")
            }
        })
    }
    function addFromIndex(entry: var) {
        root.say("", "done")
        root.setBusy("add:" + entry.id, true)
        root.call("POST", "/admin/plugin-index/install", { id: entry.id, version: entry.version }, function (ok, data, code, message) {
            root.setBusy("add:" + entry.id, false)
            if (ok) {
                root.say(entry.state === "replace" ? qsTr("Replaced the old plugin with %1 %2.").arg(entry.name).arg(entry.version)
                                                   : qsTr("Added %1 %2.").arg(entry.name).arg(entry.version), "done")
                root.load()
            } else {
                root.say(root.failText(qsTr("Could not add %1").arg(entry.name), message), "error")
            }
        })
    }
    function removeSelected() {
        const row = root.selected
        if (row === null) return
        root.say("", "done")
        root.setBusy("remove", true)
        root.call("DELETE", "/admin/plugins/" + row.plugin_id + "/" + row.version, undefined, function (ok, data, code, message) {
            root.setBusy("remove", false)
            if (ok) {
                root.say(qsTr("Removed %1 %2 from the server.").arg(row.name).arg(row.version), "done")
                root.pluginIndex = -1
                root.load()
            } else {
                root.say(root.failText(qsTr("Could not remove %1").arg(row.name), message), "error")
            }
        })
    }
    function uploadPlugin(fileUrl: string) {
        root.say("", "done")
        root.setBusy("upload", true)
        root.remember(root.admin.upload("POST", "/admin/plugins/stage", fileUrl, "application/octet-stream"), function (ok, data, code, message) {
            root.setBusy("upload", false)
            if (ok && data && data.stage_id !== undefined) {
                root.ticks = ({})
                root.stageError = ""
                root.stage = data
                stageDialog.open()
            } else {
                root.say(root.failText(qsTr("Could not upload the plugin"), message), "error")
            }
        })
    }
    function toggleTick(permission: string, on: bool) {
        const t = Object.assign({}, root.ticks)
        t[permission] = on
        root.ticks = t
    }
    function offerStage() {
        const st = root.stage
        if (st === null || !root.allTicked) return
        root.stageBusy = true
        root.stageError = ""
        root.call("POST", "/admin/plugins/stage/" + st.stage_id + "/offer", { granted_permissions: st.permissions }, function (ok, data, code, message) {
            root.stageBusy = false
            if (ok) {
                stageDialog.shown = false
                root.stage = null
                root.say(qsTr("Offered %1 %2 to everyone.").arg(st.name).arg(st.version), "done")
                root.load()
            } else {
                root.stageError = root.failText(qsTr("Could not offer %1").arg(st.name), message)
            }
        })
    }
    function discardStage() {
        const st = root.stage   // the dialog was closed (Cancel, Esc): the staged file is thrown away
        root.stage = null
        if (st !== null) root.call("DELETE", "/admin/plugins/stage/" + st.stage_id, undefined, function () {})
    }

    // 10h-3: the Randomizer plugin's detail says which engine the server offers to apps.
    readonly property bool randomizerSelected: root.selected !== null && root.selected.plugin_id === "uzume.randomizer"
    readonly property string engineLine: {
        const t = root.engineTool
        if (t === null) return ""
        const vs = (t.versions ?? []).filter(v => v.installed === true)
        if (vs.length === 0) return qsTr("No engine is installed yet.")
        return t.offered === true ? qsTr("Engine %1 is offered to apps.").arg(vs[0].version) : qsTr("Engine %1 is installed but not offered to apps.").arg(vs[0].version)
    }
    onRandomizerSelectedChanged: if (root.randomizerSelected) root.loadEngine()
    function loadEngine() {
        root.call("GET", "/admin/tools", undefined, function (ok, data) {
            root.engineTool = ok && data && data.tools ? (data.tools.find(t => t.id === "randomizer") ?? null) : null
        })
    }

    Component.onCompleted: root.load()
    onVisibleChanged: if (visible && root.phase !== "loading") root.load()

    Loader {
        id: fileLoader
        active: false
        source: "ServerSystemsFileDialog.qml"
        function open() {
            if (item) item.open()
            else active = true
        }
        onLoaded: {
            item.title = qsTr("Plugin file to upload")
            item.nameFilters = [qsTr("Uzume plugins (*.uzplugin *.fbplugin)"), qsTr("All files (*)")]
            item.chosen.connect(root.uploadPlugin)
            item.open()
        }
        onStatusChanged: if (status === Loader.Error) root.say(qsTr("The file dialog is not available in this installation."), "error")
    }

    headerActions: [
        UzButton {
            objectName: "indexRefresh"
            size: "sm"
            kind: "ghost"
            visible: root.phase === "ready" && root.index.configured === true
            busy: root.busy["refresh"] === true
            text: qsTr("Check index")
            onClicked: root.refreshIndex()
        },
        UzButton {
            objectName: "pluginUpload"
            size: "sm"
            kind: "primary"
            visible: root.phase === "ready"
            busy: root.busy["upload"] === true
            text: qsTr("Upload plugin…")
            onClicked: fileLoader.open()
        }
    ]

    notice: [
        UzStatusIcon {
            objectName: "pluginsNote"
            visible: root.note !== ""
            kind: root.noteKind
            small: true
            text: root.note
            maxWidth: 640
        }
    ]

    // ------------------------------------------------------------ states
    UzStateBlock {
        objectName: "pluginsLoading"
        Layout.fillWidth: true
        visible: root.phase === "loading"
        kind: "loading"
        title: qsTr("Loading plugins")
    }
    UzStateBlock {
        objectName: "pluginsError"
        Layout.fillWidth: true
        visible: root.phase === "error"
        kind: "error"
        title: qsTr("Can't load the plugins")
        text: root.loadError !== "" ? root.loadError : qsTr("The server did not answer.")
        action: qsTr("Try again")
        onActionClicked: { root.phase = "loading"; root.load() }
    }

    // ------------------------------------------------------------ offered plugins
    ColumnLayout {
        objectName: "pluginsOffered"
        Layout.fillWidth: true
        visible: root.phase === "ready"
        spacing: Theme.space1
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Offered by this server · %1").arg(root.pluginRows.length) }
        UzTable {
            objectPrefix: "pluginsTable"
            Layout.fillWidth: true
            currentIndex: root.pluginIndex
            onPicked: (i, row) => root.pluginIndex = i
            emptyText: qsTr("No plugins are offered yet. Upload one or add it from the official index.")
            columns: [
                { key: "name", title: qsTr("Plugin"), fill: 1.2 },
                { key: "version", title: qsTr("Version"), width: 90, mono: true },
                { key: "origin", title: qsTr("Origin"), width: 130, custom: true },
                { key: "permissions", title: qsTr("Permissions"), fill: 1.6, custom: true }
            ]
            rows: root.pluginRows
            cellDelegate: Item {
                property var row: null
                property var column: null
                implicitHeight: Theme.row32
                height: implicitHeight
                // Permissions: the list can be long, so the row shows a warning tag for the ones that change the game, else a count; the
                // whole list is in the details under the table.
                Row {
                    visible: column !== null && column.key === "permissions"
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space1
                    UzTag {
                        objectName: row ? "pluginWrites_" + row.plugin_id : ""
                        visible: row !== null && row.writes === true
                        tone: "amber"
                        icon: "warn"
                        text: qsTr("Changes the game")
                    }
                    UzTag {
                        objectName: row ? "pluginReplaced_" + row.plugin_id : ""
                        visible: row !== null && root.replacedBy[row.plugin_id] !== undefined
                        tone: "amber"
                        icon: "warn"
                        text: row !== null && root.replacedBy[row.plugin_id] !== undefined ? qsTr("Replaced by %1").arg(root.replacedBy[row.plugin_id]) : ""
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: row === null ? "" : row.permissionCount === 0 ? qsTr("None")
                              : Theme.plural(row.permissionCount, qsTr("permission"), qsTr("permissions"))
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.muted
                    }
                }
                UzTag {
                    visible: column === null || column.key === "origin"
                    objectName: row ? "pluginOrigin_" + row.plugin_id : ""
                    anchors.verticalCenter: parent.verticalCenter
                    tone: row !== null && row.official ? "ok" : "danger"
                    icon: row !== null && row.official ? "auto" : "warn"
                    text: row !== null && row.official ? qsTr("Official") : qsTr("Not signed")
                }
            }

            ColumnLayout {
                objectName: "pluginsDetails"
                visible: root.selected !== null
                width: parent ? parent.width : 0
                spacing: Theme.space1
                UzSectionLabel {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.space3
                    text: root.selected ? root.selected.name : ""
                    meta: root.selected ? root.selected.plugin_id : ""
                }
                UzStatusIcon {
                    visible: root.selected !== null && !root.selected.official
                    kind: "warning"
                    small: true
                    text: qsTr("Uzume hasn't checked this file. People see a warning before turning it on.")
                    maxWidth: 640
                }
                Text {
                    objectName: "pluginPermissionsLine"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: root.selected ? qsTr("Permissions: %1").arg(root.selected.permissions) : ""
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.ink2
                }
                UzStatusIcon {
                    objectName: "pluginWritesNote"
                    visible: root.selected !== null && root.selected.writes === true
                    kind: "warning"
                    small: true
                    text: qsTr("It can change the game: turn cheats on and change game memory. Uzume saves a copy first and asks each person for each game.")
                    maxWidth: 640
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: !root.selected ? "" : root.sourceText(root.selected)
                          + (root.selected.previous !== "" ? "  " + qsTr("Previous version %1 · %2").arg(root.selected.previous).arg(root.selected.previousDate) : "")
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.muted
                }
                Text {
                    objectName: "pluginRandomizerInfo"
                    Layout.fillWidth: true
                    visible: root.randomizerSelected
                    wrapMode: Text.Wrap
                    text: qsTr("Adds Randomize… to game pages and the Randomizer section to New run. It needs the engine from Server › Tools.")
                          + (root.engineLine !== "" ? "  " + root.engineLine : "")
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.muted
                }
                Row {
                    spacing: Theme.space2
                    UzButton {
                        objectName: "pluginOpenTools"
                        visible: root.randomizerSelected
                        size: "sm"
                        text: qsTr("Open Tools")
                        onClicked: root.player.showServerPage("tools")
                    }
                    UzButton {
                        objectName: "pluginRemove"
                        size: "sm"
                        kind: "danger"
                        busy: root.busy["remove"] === true
                        text: qsTr("Remove from server…")
                        onClicked: removeDialog.open()
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------ official index
    ColumnLayout {
        objectName: "pluginsIndex"
        Layout.fillWidth: true
        visible: root.phase === "ready"
        spacing: Theme.space1
        UzSectionLabel {
            Layout.fillWidth: true
            text: qsTr("Official index")
            meta: root.index.last_check ? qsTr("Signed · checked %1").arg(root.clockText(root.index.last_check)) : qsTr("Signed")
        }
        UzStatusIcon {
            objectName: "pluginsIndexError"
            visible: (root.index.last_error ?? "") !== ""
            kind: "warning"
            small: true
            text: root.index.last_check ? qsTr("Can't load the index: %1. Showing the last good copy from %2.").arg(root.index.last_error).arg(root.clockText(root.index.last_check))
                                        : qsTr("Can't load the index: %1.").arg(root.index.last_error ?? "")
            maxWidth: 640
        }
        UzTable {
            objectPrefix: "indexTable"
            Layout.fillWidth: true
            selectable: false
            emptyText: root.index.configured === false ? qsTr("This server has no plugin index.") : qsTr("The index lists no plugins.")
            columns: [
                { key: "name", title: qsTr("Plugin"), fill: 1 },
                { key: "version", title: qsTr("Version"), width: 90, mono: true },
                { key: "description", title: qsTr("What it does"), fill: 1.6 },
                { key: "action", title: "", width: 120, custom: true, align: "right" }
            ]
            rows: root.catalogRows
            cellDelegate: Item {
                property var row: null
                property var column: null
                implicitHeight: Theme.row32
                height: implicitHeight
                UzTag {
                    visible: row !== null && row.state === "installed"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    tone: "ok"
                    text: qsTr("Added")
                }
                UzButton {
                    objectName: row ? "indexAdd_" + row.id : ""
                    visible: row !== null && row.state !== "installed"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    size: "sm"
                    busy: row !== null && root.busy["add:" + row.id] === true
                    text: row !== null && row.state === "update" ? qsTr("Update") : row !== null && row.state === "replace" ? qsTr("Replace") : qsTr("Add")
                    onClicked: root.addFromIndex(row)
                }
            }
        }
    }

    // ------------------------------------------------------------ dialogs
    UzDialog {
        id: stageDialog
        objectName: "stageDialog"
        parent: root
        size: "lg"
        label: qsTr("Plugins")
        title: root.stage ? qsTr("Offer %1 %2 to everyone?").arg(root.stage.name).arg(root.stage.version) : ""
        text: !root.stage ? ""
            : root.stage.official ? qsTr("This file is signed by Uzume. Tick each permission it asks for.")
            : qsTr("Uzume hasn't checked this file. Tick each permission you checked.")
        busy: root.stageBusy
        error: root.stageError !== "" ? root.stageError
             : root.stage && root.stage.conflict ? qsTr("A different file with this version is already offered. Remove that one first.")
             : root.stage && root.stage.already_offered ? qsTr("This file is already offered.") : ""
        onClosed: root.discardStage()
        Repeater {
            model: root.stage ? root.stage.permissions : []
            delegate: UzCheckbox {
                required property string modelData
                required property int index
                objectName: "stagePermission_" + index
                text: root.permissionLabel(modelData)
                checked: root.ticks[modelData] === true
                onToggled: root.toggleTick(modelData, checked)
            }
        }
        actions: [
            UzButton {
                objectName: "stageOffer"
                kind: "primary"
                enabled: root.allTicked && root.stage !== null && !root.stage.conflict && !root.stage.already_offered
                text: qsTr("Offer to everyone")
                onClicked: root.offerStage()
            },
            UzButton { objectName: "stageCancel"; text: qsTr("Cancel"); onClicked: stageDialog.close() }
        ]
    }
    UzDialog {
        id: removeDialog
        objectName: "removePluginDialog"
        parent: root
        label: qsTr("Plugins")
        title: qsTr("Remove %1?").arg(root.selected ? root.selected.name : "")
        text: qsTr("People keep their copy until they uninstall it. Its run data stays.")
        actions: [
            UzButton { objectName: "removePluginCancel"; text: qsTr("Cancel"); onClicked: removeDialog.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "removePluginConfirm"
                kind: "danger"
                text: qsTr("Remove")
                onClicked: {
                    removeDialog.close()
                    root.removeSelected()
                }
            }
        ]
    }
}
