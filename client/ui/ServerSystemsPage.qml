import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Systems & cores (accounts-and-server.md 3l-app-1 to 3l-app-3) from GET /admin/systems. A second column "SYSTEMS" lists the systems
// with their picked core or the warning "No core" (same for everyone on the server); the page shows the tabs Cores and Firmware of
// the selected system. Nothing is preselected: a system without a default core stays without one until an admin picks it.
//   Cores:    table of the buildbot catalog plus installed cores; "Use this" installs the core when needed and sets it as the
//             default of the system (asks before switching away from another core); the details of the selected core hold
//             "Update now" and "Remove from server..."; "Check now" refreshes the catalog (POST /admin/cores/sync);
//             SettingsRow "Update cores automatically" (PATCH /admin/cores/settings).
//   Firmware: files of the system with a status Tag; Add... / Replace... upload the file (PUT, raw body), Keep anyway pins the
//             file that is there (POST .../pin), Remove deletes the stored file.
// What the API does not carry is left out: no core size (the catalog has no size), no download progress.
// objectNames: serverSystemsPage, systemsColumn, systemItem_<id>, systemCore_<id>, systemsTabs, coresTable (rows coresTableRow_<n>),
// useCore_<core_id>, coreUpdate, coreRemove, checkNow, autoUpdateRow, firmwareTable, firmwareAdd_<file>, firmwareRemove_<file>,
// firmwareKeep_<file>, systemsNote, systemsNoCore, switchCoreDialog, removeCoreDialog, removeFirmwareDialog.
Item {
    id: root
    objectName: "serverSystemsPage"
    required property PlayerController player
    readonly property AdminController admin: root.player.admin

    property string phase: "loading"          // loading | ready | error
    property string loadError: ""
    property var systems: []
    property var source: ({})
    property string selectedId: ""
    property string tab: "cores"              // cores | firmware
    property int coreIndex: -1
    property string note: ""
    property string noteKind: "done"
    property var busy: ({})                   // key -> true while a call runs
    property var calls: ({})                  // request id -> callback(ok, data, code, message)
    property string uploadFileId: ""

    readonly property var system: root.systemById(root.selectedId)
    readonly property var coreRows: root.system ? root.coreRowsOf(root.system) : []
    readonly property var firmwareRows: root.system ? root.firmwareRowsOf(root.system) : []
    readonly property var selectedCore: root.coreIndex >= 0 && root.coreIndex < root.coreRows.length ? root.coreRows[root.coreIndex] : null
    readonly property bool sourceFailed: (root.source.last_error ?? "") !== ""

    // ------------------------------------------------------------ data
    // "Nintendo - Game Boy / Color (Gambatte)" -> "Gambatte": the part that tells the cores of one system apart comes first.
    function shortCoreName(name: string): string {
        const m = /\(([^()]+)\)\s*$/.exec(name ?? "")
        return m ? m[1] : (name ?? "")
    }
    function systemById(id: string): var {
        for (const s of root.systems) if (s.id === id) return s
        return null
    }
    function defaultCoreOf(sys: var): var {
        if (!sys || !sys.default_core_id) return null
        for (const c of sys.cores ?? []) if (c.core_id === sys.default_core_id) return c
        return null
    }
    function coreName(sys: var, coreId: string): string {
        for (const c of sys.cores ?? []) if (c.core_id === coreId) return c.display_name
        for (const c of sys.available_cores ?? []) if (c.core_id === coreId) return c.display_name
        return coreId
    }
    function coreRowsOf(sys: var): var {
        const installed = {}
        for (const c of sys.cores ?? []) installed[c.core_id] = c
        const out = []
        const seen = {}
        const note = (a) => {
            const parts = []
            if (a.experimental) parts.push(qsTr("Experimental."))
            if (a.non_commercial) parts.push(qsTr("Non-commercial use only."))
            for (const n of a.notes ?? []) parts.push(n)
            return parts.join(" ")
        }
        for (const a of sys.available_cores ?? []) {
            const i = installed[a.core_id]
            seen[a.core_id] = true
            out.push({ core_id: a.core_id, name: a.display_name, version: i ? i.version : (a.build_date ?? ""), note: note(a),
                       installed: i !== undefined, isDefault: sys.default_core_id === a.core_id, update: i ? i.update_available : false,
                       updateDate: i ? (i.update_build_date ?? "") : "", license: a.license })
        }
        for (const c of sys.cores ?? []) {
            if (seen[c.core_id]) continue
            out.push({ core_id: c.core_id, name: c.display_name, version: c.version, note: c.experimental ? qsTr("Experimental.") : "",
                       installed: true, isDefault: sys.default_core_id === c.core_id, update: c.update_available,
                       updateDate: c.update_build_date ?? "", license: c.license })
        }
        return out
    }
    function firmwareRowsOf(sys: var): var {
        return (sys.firmware ?? []).map(f => ({ id: f.id, file: f.display_name, system: sys.id, state: f.state, present: f.present,
                                                required: f.required, sha256: f.sha256 ?? "" }))
    }
    function firmwareStatus(row: var): var {
        switch (row.state) {
        case "valid": return { text: qsTr("Matches"), tone: "ok", hint: "" }
        case "mismatch": return { text: qsTr("Doesn't match"), tone: "danger", hint: "" }
        case "missing": return { text: qsTr("Missing"), tone: "danger", hint: qsTr("needed") }
        default: return { text: qsTr("Missing"), tone: "neutral", hint: qsTr("optional") }
        }
    }
    function clockText(iso: string): string {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const now = new Date()
        const same = d.getFullYear() === now.getFullYear() && d.getMonth() === now.getMonth() && d.getDate() === now.getDate()
        return same ? qsTr("today %1").arg(Qt.formatTime(d, "HH:mm")) : Qt.formatDateTime(d, "d MMM HH:mm")
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
    function call(method: string, path: string, body: var, done: var) {
        const id = root.admin.request(method, path, body)
        const c = Object.assign({}, root.calls)
        c[id] = done
        root.calls = c
    }
    function adopt(sys: var) {   // the answer of every change is the system now stored
        if (!sys || sys.id === undefined) return
        root.systems = root.systems.map(s => s.id === sys.id ? sys : s)
    }
    function failText(what: string, message: string): string {
        return message !== "" ? qsTr("%1: %2").arg(what).arg(message) : qsTr("%1: the server did not answer.").arg(what)
    }

    function load() {
        root.call("GET", "/admin/systems", undefined, function (ok, data, code, message) {
            if (ok && data && data.systems !== undefined) {
                root.systems = data.systems
                root.source = data.core_source ?? ({})
                root.phase = "ready"
                if (root.systemById(root.selectedId) === null) {
                    root.selectedId = root.systems.length > 0 ? root.systems[0].id : ""
                    root.coreIndex = -1
                }
            } else if (root.phase !== "ready") {
                root.phase = "error"
                root.loadError = message
            } else {
                root.say(root.failText(qsTr("Could not read the systems"), message), "error")
            }
        })
    }
    function reloadSoon() { reloadTimer.restart() }
    Timer { id: reloadTimer; interval: 1500; onTriggered: root.load() }

    // ------------------------------------------------------------ actions
    function useCore(coreId: string) {
        const sys = root.system
        if (!sys) return
        const current = root.defaultCoreOf(sys)
        if (current !== null && current.core_id !== coreId) {
            switchDialog.coreId = coreId
            switchDialog.fromName = current.display_name
            switchDialog.open()
            return
        }
        root.doUseCore(sys.id, coreId)
    }
    function doUseCore(systemId: string, coreId: string) {
        const key = "use:" + coreId
        const sys = root.systemById(systemId)
        const name = root.coreName(sys, coreId)
        root.say("", "done")
        root.setBusy(key, true)
        const setDefault = function () {
            root.call("PATCH", "/admin/systems/" + systemId, { default_core_id: coreId }, function (ok, data, code, message) {
                root.setBusy(key, false)
                if (ok) {
                    root.adopt(data)
                    root.say(qsTr("%1 is now the core for %2.").arg(name).arg(sys.display_name), "done")
                } else {
                    root.say(root.failText(qsTr("Could not use %1").arg(name), message), "error")
                }
            })
        }
        const installed = (sys.cores ?? []).some(c => c.core_id === coreId)
        if (installed) {
            setDefault()
            return
        }
        root.call("POST", "/admin/systems/" + systemId + "/cores/" + coreId + "/install", undefined, function (ok, data, code, message) {
            if (!ok) {
                root.setBusy(key, false)
                root.say(root.failText(qsTr("%1 didn't download").arg(name), message), "error")
                return
            }
            root.adopt(data)
            setDefault()
        })
    }
    function updateCore(coreId: string) {
        const sys = root.system
        const name = root.coreName(sys, coreId)
        root.say("", "done")
        root.setBusy("update:" + coreId, true)
        root.call("POST", "/admin/systems/" + sys.id + "/cores/" + coreId + "/update", undefined, function (ok, data, code, message) {
            root.setBusy("update:" + coreId, false)
            if (ok) {
                root.adopt(data)
                root.say(qsTr("%1 is up to date.").arg(name), "done")
            } else {
                root.say(root.failText(qsTr("Could not update %1").arg(name), message), "error")
            }
        })
    }
    function removeCore(coreId: string) {
        const sys = root.system
        const name = root.coreName(sys, coreId)
        root.say("", "done")
        root.setBusy("remove:" + coreId, true)
        root.call("DELETE", "/admin/systems/" + sys.id + "/cores/" + coreId, undefined, function (ok, data, code, message) {
            root.setBusy("remove:" + coreId, false)
            if (ok) {
                root.adopt(data)
                root.say(qsTr("Removed %1 from the server.").arg(name), "done")
                root.load()
            } else {
                root.say(root.failText(qsTr("Could not remove %1").arg(name), message), "error")
            }
        })
    }
    function checkNow() {
        root.say("", "done")
        root.setBusy("sync", true)
        root.call("POST", "/admin/cores/sync", undefined, function (ok, data, code, message) {
            if (ok) {
                root.say(qsTr("Checking the buildbot…"), "done")
                root.reloadSoon()
                syncDone.restart()
            } else {
                root.setBusy("sync", false)
                root.say(root.failText(qsTr("Could not check for cores"), message), "error")
            }
        })
    }
    Timer { id: syncDone; interval: 1800; onTriggered: { root.setBusy("sync", false); root.say("", "done") } }
    function setAutoUpdate(on: bool) {
        root.say("", "done")
        root.call("PATCH", "/admin/cores/settings", { auto_update: on }, function (ok, data, code, message) {
            if (ok) {
                root.source = data
                root.say(on ? qsTr("Cores update automatically.") : qsTr("Cores no longer update automatically."), "done")
            } else {
                root.source = Object.assign({}, root.source)   // puts the toggle back
                root.say(root.failText(qsTr("Could not change the setting"), message), "error")
            }
        })
    }
    // Parity with the old web admin: the automatic update per system and per core, and the firmware mode of a system.
    function patchSystem(body: var, doneText: string) {
        const sys = root.system
        if (!sys) return
        root.say("", "done")
        root.call("PATCH", "/admin/systems/" + sys.id, body, function (ok, data, code, message) {
            if (ok) { root.adopt(data); root.say(doneText, "done") }
            else { root.systems = root.systems.slice(); root.say(root.failText(qsTr("Could not change the setting"), message), "error") }   // puts the control back
        })
    }
    function patchCore(coreId: string, auto: string) {
        const sys = root.system
        if (!sys) return
        root.say("", "done")
        root.call("PATCH", "/admin/systems/" + sys.id + "/cores/" + coreId, { auto_update: auto }, function (ok, data, code, message) {
            if (ok) {
                root.adopt(data)
                root.say(auto === "off" ? qsTr("This build stays as it is; it is no longer updated automatically.") : qsTr("This core follows the automatic updates of the system again."), "done")
            } else {
                root.systems = root.systems.slice()
                root.say(root.failText(qsTr("Could not change the setting"), message), "error")
            }
        })
    }
    readonly property var selectedInstalled: {
        const sys = root.system
        if (!sys || !root.selectedCore) return null
        return (sys.cores ?? []).filter(c => c.core_id === root.selectedCore.core_id)[0] ?? null
    }
    function uploadFirmware(fileUrl: string) {
        const sys = root.system
        const fileId = root.uploadFileId
        if (!sys || fileId === "") return
        root.say("", "done")
        root.setBusy("fw:" + fileId, true)
        const id = root.admin.upload("PUT", "/admin/systems/" + sys.id + "/firmware/" + fileId, fileUrl, "application/octet-stream")
        const c = Object.assign({}, root.calls)
        c[id] = function (ok, data, code, message) {
            root.setBusy("fw:" + fileId, false)
            if (ok) {
                root.adopt(data)
                root.say(qsTr("Added %1.").arg(fileId), "done")
            } else {
                root.say(root.failText(qsTr("Could not add %1").arg(fileId), message), "error")
            }
        }
        root.calls = c
    }
    function keepFirmware(fileId: string, sha256: string) {
        const sys = root.system
        root.say("", "done")
        root.setBusy("fw:" + fileId, true)
        root.call("POST", "/admin/systems/" + sys.id + "/firmware/" + fileId + "/pin", { sha256: sha256 }, function (ok, data, code, message) {
            root.setBusy("fw:" + fileId, false)
            if (ok) {
                root.adopt(data)
                root.say(qsTr("Keeping %1 as it is.").arg(fileId), "done")
            } else {
                root.say(root.failText(qsTr("Could not keep %1").arg(fileId), message), "error")
            }
        })
    }
    function removeFirmware(fileId: string) {
        const sys = root.system
        root.say("", "done")
        root.setBusy("fw:" + fileId, true)
        root.call("DELETE", "/admin/systems/" + sys.id + "/firmware/" + fileId, undefined, function (ok, data, code, message) {
            root.setBusy("fw:" + fileId, false)
            if (ok) {
                root.adopt(data)
                root.say(qsTr("Removed %1.").arg(fileId), "done")
            } else {
                root.say(root.failText(qsTr("Could not remove %1").arg(fileId), message), "error")
            }
        })
    }
    function pickFirmware(fileId: string) {
        root.uploadFileId = fileId
        fileLoader.open()
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
    onSelectedIdChanged: { root.coreIndex = -1; root.say("", "done") }
    Component.onCompleted: root.load()
    onVisibleChanged: if (visible && root.phase !== "loading") root.load()

    // ------------------------------------------------------------ file dialog
    Loader {
        id: fileLoader
        active: false
        source: "ServerSystemsFileDialog.qml"
        function open() {
            if (item) item.open()
            else active = true
        }
        onLoaded: {
            item.title = qsTr("Choose a firmware file")
            item.chosen.connect(root.uploadFirmware)
            item.open()
        }
        onStatusChanged: if (status === Loader.Error) root.say(qsTr("The file dialog is not available in this installation."), "error")
    }

    // ------------------------------------------------------------ layout
    RowLayout {
        anchors.fill: parent
        spacing: 0

        // The second column: SYSTEMS.
        Item {
            objectName: "systemsColumn"
            Layout.fillHeight: true
            Layout.preferredWidth: 216
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.rule2 }
            Column {
                x: Theme.space3
                y: 48
                width: parent.width - Theme.space3 - 1
                spacing: 0
                UzSectionLabel { width: parent.width; text: qsTr("Systems") }
                Item { width: 1; height: Theme.space1 }
                Text {
                    width: parent.width - Theme.space2
                    wrapMode: Text.Wrap
                    bottomPadding: Theme.space2
                    text: qsTr("Same for everyone on %1.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("this server"))
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.muted
                }
                Repeater {
                    model: root.systems
                    delegate: Item {
                        id: entry
                        required property var modelData
                        readonly property bool current: modelData.id === root.selectedId
                        readonly property var core: root.defaultCoreOf(modelData)
                        objectName: "systemItem_" + modelData.id
                        width: parent.width
                        height: Theme.row44 + 8
                        activeFocusOnTab: true
                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData.display_name
                        Accessible.selected: entry.current
                        Keys.onReturnPressed: root.selectedId = modelData.id
                        Keys.onSpacePressed: root.selectedId = modelData.id
                        Rectangle {
                            anchors.fill: parent
                            color: entry.current ? Theme.selected : (hover.hovered ? Theme.hover : "transparent")
                        }
                        Rectangle { visible: entry.current; width: 2; height: parent.height; color: Theme.accent }   // marker-start
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            x: Theme.space15
                            width: parent.width - Theme.space15 - Theme.space1
                            spacing: 2
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                text: modelData.display_name
                                textFormat: Text.PlainText
                                font.family: Theme.sans
                                font.pixelSize: Theme.roles.body.size
                                font.weight: entry.current ? Font.Medium : Font.Normal
                                color: Theme.ink
                            }
                            Row {
                                spacing: 6
                                width: parent.width
                                UzStatusIcon {
                                    visible: entry.core === null
                                    kind: "warning"
                                    small: true
                                }
                                Text {
                                    objectName: "systemCore_" + modelData.id
                                    width: parent.width - (entry.core === null ? 20 : 0)
                                    elide: Text.ElideRight
                                    text: entry.core !== null ? root.shortCoreName(entry.core.display_name) : qsTr("No core")
                                    textFormat: Text.PlainText
                                    font.family: Theme.sans
                                    font.pixelSize: Theme.roles.input.size
                                    color: entry.core !== null ? Theme.muted : Theme.ink
                                }
                            }
                        }
                        ToolTip.visible: hover.hovered
                        ToolTip.delay: 500
                        ToolTip.text: modelData.display_name + "\n" + (entry.core !== null ? entry.core.display_name : qsTr("No core"))
                        HoverHandler { id: hover }
                        TapHandler { onTapped: { entry.forceActiveFocus(); root.selectedId = modelData.id } }
                    }
                }
            }
        }

        AdminPage {
            id: page
            Layout.fillWidth: true
            Layout.fillHeight: true
            player: root.player
            title: root.system ? root.system.display_name : qsTr("Systems & cores")
            label: qsTr("Systems & cores")
            area: qsTr("Systems & cores")
            helpItems: [
                { title: qsTr("Cores"), text: qsTr("A core runs the games of a system. Pick one per system; nothing is picked for you. Saves keep working when you switch, states made with the old core may not load.") },
                { title: qsTr("Firmware"), text: qsTr("Firmware files are your own. None ship with Uzume. Some systems start without them, some need them.") }
            ]
            helpLines: root.sourceFailed ? [] : (root.source.last_success ? [qsTr("Catalog checked %1").arg(root.clockText(root.source.last_success))] : [])

            headerActions: [
                UzButton {
                    objectName: "checkNow"
                    size: "sm"
                    visible: root.phase === "ready"
                    busy: root.busy["sync"] === true
                    text: qsTr("Check now")
                    onClicked: root.checkNow()
                }
            ]

            notice: [
                UzStatusIcon {
                    objectName: "systemsNote"
                    visible: root.note !== ""
                    kind: root.noteKind
                    small: true
                    text: root.note
                    maxWidth: 640
                },
                UzStatusIcon {
                    objectName: "systemsNoCore"
                    visible: root.phase === "ready" && root.tab === "cores" && root.system !== null && root.defaultCoreOf(root.system) === null
                    kind: "warning"
                    text: root.system ? qsTr("No core picked. %1 games can't start until you pick one.").arg(root.system.id.toUpperCase()) : ""
                    maxWidth: 640
                },
                UzStatusIcon {
                    objectName: "systemsSourceError"
                    visible: root.phase === "ready" && root.tab === "cores" && root.sourceFailed
                    kind: "warning"
                    text: qsTr("Can't load the core list: %1. Installed cores keep working.").arg(root.source.last_error ?? "")
                    maxWidth: 640
                },
                Repeater {
                    model: root.phase === "ready" && root.tab === "firmware" ? root.firmwareRows.filter(r => r.state === "mismatch") : []
                    delegate: UzStatusIcon {
                        required property var modelData
                        objectName: "firmwareMismatch"
                        kind: "warning"
                        text: qsTr("%1 doesn't match the expected file. Some games may not start. The file was kept.").arg(modelData.file)
                        maxWidth: 640
                    }
                }
            ]

            UzStateBlock {
                objectName: "systemsLoading"
                Layout.fillWidth: true
                visible: root.phase === "loading"
                kind: "loading"
                title: qsTr("Loading systems")
            }
            UzStateBlock {
                objectName: "systemsError"
                Layout.fillWidth: true
                visible: root.phase === "error"
                kind: "error"
                title: qsTr("Can't load the systems")
                text: root.loadError !== "" ? root.loadError : qsTr("The server did not answer.")
                action: qsTr("Try again")
                onActionClicked: { root.phase = "loading"; root.load() }
            }
            UzStateBlock {
                objectName: "systemsEmpty"
                Layout.fillWidth: true
                visible: root.phase === "ready" && root.systems.length === 0
                kind: "empty"
                title: qsTr("This server has no systems")
            }

            UzTabs {
                objectName: "systemsTabs"
                Layout.fillWidth: true
                visible: root.phase === "ready" && root.system !== null
                items: [qsTr("Cores"), qsTr("Firmware")]
                active: root.tab === "cores" ? qsTr("Cores") : qsTr("Firmware")
                onPicked: (label) => root.tab = label === qsTr("Cores") ? "cores" : "firmware"
            }

            // ---------------------------------------------------- Cores
            ColumnLayout {
                objectName: "coresTab"
                Layout.fillWidth: true
                visible: root.phase === "ready" && root.system !== null && root.tab === "cores"
                spacing: Theme.space4
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space1
                    UzSectionLabel {
                        Layout.fillWidth: true
                        text: qsTr("Cores from the buildbot")
                        meta: root.source.last_success ? qsTr("Checked %1").arg(root.clockText(root.source.last_success)) : ""
                    }
                    UzTable {
                        id: coresTable
                        objectPrefix: "coresTable"
                        Layout.fillWidth: true
                        currentIndex: root.coreIndex
                        onPicked: (i, row) => root.coreIndex = i
                        emptyText: root.sourceFailed ? qsTr("The core list could not be loaded.") : qsTr("The buildbot offers no core for this system.")
                        columns: [
                            { key: "name", title: qsTr("Core"), fill: 1 },
                            { key: "version", title: qsTr("Version"), width: 110, mono: true },
                            { key: "note", title: qsTr("Good to know"), fill: 1.6 },
                            { key: "action", title: "", width: 190, custom: true, align: "right" }
                        ]
                        rows: root.coreRows
                        cellDelegate: Item {
                            id: cell
                            property var row: null
                            property var column: null
                            implicitHeight: Theme.row32
                            height: implicitHeight
                            Row {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: Theme.space1
                                UzTag {
                                    visible: cell.row !== null && cell.row.update === true
                                    anchors.verticalCenter: parent.verticalCenter
                                    tone: "accent"
                                    text: qsTr("Update")
                                }
                                UzTag {
                                    objectName: cell.row ? "coreInUse_" + cell.row.core_id : ""
                                    visible: cell.row !== null && cell.row.isDefault === true
                                    anchors.verticalCenter: parent.verticalCenter
                                    tone: "ok"
                                    text: qsTr("In use")
                                }
                                UzButton {
                                    objectName: cell.row ? "useCore_" + cell.row.core_id : ""
                                    visible: cell.row !== null && cell.row.isDefault !== true
                                    size: "sm"
                                    kind: "outline"
                                    busy: cell.row !== null && root.busy["use:" + cell.row.core_id] === true
                                    text: qsTr("Use this")
                                    onClicked: root.useCore(cell.row.core_id)
                                }
                            }
                        }

                        // Details of the selected core, under the table.
                        ColumnLayout {
                            objectName: "coreDetails"
                            visible: root.selectedCore !== null
                            width: parent ? parent.width : 0
                            spacing: Theme.space1
                            UzSectionLabel {
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.space3
                                text: root.selectedCore ? root.selectedCore.name : ""
                                meta: root.selectedCore && root.selectedCore.installed ? qsTr("Installed") : qsTr("Not installed")
                            }
                            Text {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                visible: text !== ""
                                text: !root.selectedCore ? "" : root.selectedCore.update
                                      ? qsTr("%1 has a newer build%2. It installs tonight with automatic updates, or now. States from the old build may not load after the update.")
                                            .arg(root.selectedCore.name).arg(root.selectedCore.updateDate !== "" ? " (" + root.selectedCore.updateDate + ")" : "")
                                      : root.selectedCore.installed ? qsTr("This core is up to date.")
                                      : qsTr("\"Use this\" downloads the core to the server first.")
                                textFormat: Text.PlainText
                                font.family: Theme.sans
                                font.pixelSize: Theme.roles.input.size
                                color: Theme.muted
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: Theme.space1
                                UzButton {
                                    objectName: "coreUpdate"
                                    size: "sm"
                                    visible: root.selectedCore !== null && root.selectedCore.update === true
                                    busy: root.selectedCore !== null && root.busy["update:" + root.selectedCore.core_id] === true
                                    text: qsTr("Update now")
                                    onClicked: root.updateCore(root.selectedCore.core_id)
                                }
                                UzButton {
                                    objectName: "corePin"
                                    size: "sm"
                                    visible: root.selectedInstalled !== null
                                    text: root.selectedInstalled && root.selectedInstalled.auto_update === "off" ? qsTr("Follow automatic updates") : qsTr("Keep this build")
                                    onClicked: root.patchCore(root.selectedCore.core_id, root.selectedInstalled.auto_update === "off" ? "auto" : "off")
                                }
                                UzButton {
                                    objectName: "coreRemove"
                                    size: "sm"
                                    kind: "danger"
                                    visible: root.selectedCore !== null && root.selectedCore.installed === true
                                    text: qsTr("Remove from server…")
                                    onClicked: removeCoreDialog.open()
                                }
                            }
                        }
                    }
                }

                UzSettingsRow {
                    objectName: "autoUpdateRow"
                    Layout.fillWidth: true
                    label: qsTr("Update cores automatically")
                    description: qsTr("Every night. Saves never change; states made with an older core may not load.")
                    ctrl: "toggle"
                    checked: root.source.auto_update === true
                    toggleName: "autoUpdateToggle"
                    resetMode: "none"
                    onToggled: (on) => root.setAutoUpdate(on)
                }
                UzSettingsRow {
                    objectName: "systemAutoUpdateRow"
                    Layout.fillWidth: true
                    label: qsTr("Automatic updates for %1").arg(root.system ? root.system.display_name : "")
                    description: qsTr("Follow the server setting, or turn the nightly update on or off for this system only.")
                    ctrl: "segment"
                    controlName: "systemAutoUpdate"
                    values: [{ value: "default", label: qsTr("Server setting") }, { value: "on", label: qsTr("On") }, { value: "off", label: qsTr("Off") }]
                    current: root.system ? root.system.auto_update : "default"
                    resetMode: "none"
                    onValuePicked: (value) => root.patchSystem({ auto_update: value }, qsTr("Automatic updates for %1 are set.").arg(root.system.display_name))
                }
            }

            // ---------------------------------------------------- Firmware
            ColumnLayout {
                objectName: "firmwareTab"
                Layout.fillWidth: true
                visible: root.phase === "ready" && root.system !== null && root.tab === "firmware"
                spacing: Theme.space1
                UzSettingsRow {
                    objectName: "firmwareModeRow"
                    Layout.fillWidth: true
                    label: qsTr("Firmware")
                    description: qsTr("Built-in: the core brings its own firmware and files are optional. Files: this system needs the files below.")
                    ctrl: "segment"
                    controlName: "firmwareMode"
                    values: [{ value: "builtin", label: qsTr("Built-in") }, { value: "native", label: qsTr("Files") }]
                    current: root.system ? root.system.firmware_mode : "builtin"
                    resetMode: "none"
                    onValuePicked: (value) => root.patchSystem({ firmware_mode: value }, qsTr("Firmware mode of %1 is set.").arg(root.system.display_name))
                }
                UzSectionLabel { Layout.fillWidth: true; text: qsTr("Firmware files") }
                UzTable {
                    objectPrefix: "firmwareTable"
                    Layout.fillWidth: true
                    selectable: false
                    emptyText: qsTr("This system needs no firmware files.")
                    columns: [
                        { key: "file", title: qsTr("File"), fill: 1 },
                        { key: "system", title: qsTr("System"), width: 90, custom: true },
                        { key: "status", title: qsTr("Status"), width: 190, custom: true },
                        { key: "action", title: "", width: 250, custom: true, align: "right" }
                    ]
                    rows: root.firmwareRows
                    cellDelegate: Item {
                        id: fcell
                        property var row: null
                        property var column: null
                        readonly property var st: fcell.row ? root.firmwareStatus(fcell.row) : ({ text: "", tone: "neutral", hint: "" })
                        readonly property bool working: fcell.row !== null && root.busy["fw:" + fcell.row.id] === true
                        implicitHeight: Theme.row32
                        height: implicitHeight
                        UzSystemChip {
                            visible: fcell.column !== null && fcell.column.key === "system"
                            anchors.verticalCenter: parent.verticalCenter
                            system: fcell.row ? fcell.row.system : ""
                        }
                        Row {
                            visible: fcell.column !== null && fcell.column.key === "status"
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.space1
                            UzTag {
                                objectName: fcell.row ? "firmwareStatus_" + fcell.row.id : ""
                                anchors.verticalCenter: parent.verticalCenter
                                tone: fcell.st.tone
                                text: fcell.st.text
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: fcell.st.hint !== ""
                                text: fcell.st.hint
                                textFormat: Text.PlainText
                                font.family: Theme.sans
                                font.pixelSize: Theme.roles.input.size
                                color: Theme.muted
                            }
                        }
                        Row {
                            visible: fcell.column !== null && fcell.column.key === "action"
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Theme.space1
                            UzButton {
                                objectName: fcell.row ? "firmwareKeep_" + fcell.row.id : ""
                                visible: fcell.row !== null && fcell.row.state === "mismatch"
                                size: "sm"
                                kind: "ghost"
                                enabled: !fcell.working
                                text: qsTr("Keep anyway")
                                onClicked: root.keepFirmware(fcell.row.id, fcell.row.sha256)
                            }
                            UzButton {
                                objectName: fcell.row ? "firmwareAdd_" + fcell.row.id : ""
                                size: "sm"
                                busy: fcell.working
                                text: fcell.row !== null && fcell.row.present ? qsTr("Replace…") : qsTr("Add…")
                                onClicked: root.pickFirmware(fcell.row.id)
                            }
                            UzButton {
                                objectName: fcell.row ? "firmwareRemove_" + fcell.row.id : ""
                                visible: fcell.row !== null && fcell.row.present === true
                                size: "sm"
                                kind: "ghost"
                                enabled: !fcell.working
                                text: qsTr("Remove…")
                                onClicked: { removeFirmwareDialog.fileId = fcell.row.id; removeFirmwareDialog.open() }
                            }
                        }
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------ dialogs
    UzDialog {
        id: switchDialog
        objectName: "switchCoreDialog"
        property string coreId: ""
        property string fromName: ""
        label: qsTr("Systems & cores")
        title: qsTr("Switch from %1?").arg(switchDialog.fromName)
        text: qsTr("Saves keep working. States made with %1 won't load with the new core.").arg(switchDialog.fromName)
        actions: [
            UzButton {
                objectName: "switchCoreConfirm"
                kind: "primary"
                text: qsTr("Switch")
                onClicked: {
                    switchDialog.close()
                    if (root.system) root.doUseCore(root.system.id, switchDialog.coreId)
                }
            },
            UzButton { objectName: "switchCoreCancel"; text: qsTr("Cancel"); onClicked: switchDialog.close() }
        ]
    }
    UzDialog {
        id: removeCoreDialog
        objectName: "removeCoreDialog"
        label: qsTr("Systems & cores")
        title: qsTr("Remove %1?").arg(root.selectedCore ? root.selectedCore.name : "")
        text: qsTr("Apps can no longer download it from this server. Apps keep the copy they have. You can install it again from the buildbot.")
        actions: [
            UzButton { objectName: "removeCoreCancel"; text: qsTr("Cancel"); onClicked: removeCoreDialog.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "removeCoreConfirm"
                kind: "danger"
                text: qsTr("Remove")
                onClicked: {
                    removeCoreDialog.close()
                    if (root.selectedCore) root.removeCore(root.selectedCore.core_id)
                }
            }
        ]
    }
    UzDialog {
        id: removeFirmwareDialog
        objectName: "removeFirmwareDialog"
        property string fileId: ""
        label: qsTr("Systems & cores")
        title: qsTr("Remove %1?").arg(removeFirmwareDialog.fileId)
        text: qsTr("The stored file is deleted. Games that need it can't start until you add it again.")
        actions: [
            UzButton { objectName: "removeFirmwareCancel"; text: qsTr("Cancel"); onClicked: removeFirmwareDialog.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "removeFirmwareConfirm"
                kind: "danger"
                text: qsTr("Remove")
                onClicked: {
                    removeFirmwareDialog.close()
                    root.removeFirmware(removeFirmwareDialog.fileId)
                }
            }
        ]
    }
}
