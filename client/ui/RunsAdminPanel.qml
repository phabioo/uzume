import QtQuick
import QtQuick.Layouts
import Uzume.App

// Runs, admin actions (runs-and-server.md 4j-app-1, admin part): "All runs" for admins under the Runs screen: every run of every
// person, archived ones included (GET /admin/runs), and for the selected run its people and the actions the state allows:
// Pause / Resume, Finish run... (asks), Archive / Unarchive, Send invitations again, Start now / Start without the people who have
// not joined (asks, names them), per invited person Send again / Remove invitation (asks), Export event log (JSON file).
// The answer of each action is the run itself; the list and the Runs screen are read again. Shown for admins only
// (`player.admin.available`, set by the caller).
// Two uses: `withTable` (default) is the "More runs" section under the Runs list, shown only when there are runs the list above
// leaves out, with the actions of the run picked in its table. Without `withTable` the panel is the admin part of ONE run of the
// main list (`runId`): it sits in that run's own detail area (selected run in the list, open run view) and shows the actions the
// state allows, plus the invited people when someone is still invited; no heading, no table.
// objectNames: runsAdminPanel, runsAdminTable (prefix), runsAdminDetails, runsAdminPause, runsAdminResume, runsAdminFinish,
// runsAdminArchive, runsAdminUnarchive, runsAdminResend, runsAdminStart, runsAdminStartWithout, runsAdminExport,
// runsAdminPersonResend_<id>, runsAdminPersonRemove_<id>, runsAdminNote, runsAdminEmpty.
Item {
    id: root
    objectName: "runsAdminPanel"
    required property PlayerController player
    property string runId: ""
    property bool withTable: true
    // Ids of the runs the Runs list above already shows: the table below lists only the other runs (archived ones, runs of
    // other people that list leaves out), so each run appears once; the admin actions of a listed run work on the open
    // or selected run through `runId`.
    property var listedIds: []
    readonly property AdminController admin: root.player.admin
    property var items: []            // [{run, archived}]
    property string phase: "idle"     // idle | loading | ready | error
    property string selectedId: ""
    property string note: ""
    property string noteKind: "done"
    property bool busy: false
    readonly property int selectedIndex: root.indexOf(root.selectedId)
    readonly property var sel: root.selectedIndex >= 0 ? root.items[root.selectedIndex] : null
    readonly property var run: root.sel ? root.sel.run : null
    readonly property string status: root.run ? root.run.status : ""
    readonly property bool over: root.status === "finished" || root.status === "abandoned"
    readonly property var invited: root.run ? root.run.participants.filter(p => p.status === "invited") : []
    readonly property bool hasExtra: root.withTable && root.phase === "ready" && root.extraItems.length > 0
    readonly property var extraItems: root.items.filter(it => root.listedIds.indexOf(it.run.run_id) < 0)
    implicitHeight: col.implicitHeight

    AdminCall { id: api; admin: root.admin }
    AdminAskDialog { id: ask }

    function indexOf(id: string): int {
        for (let i = 0; i < root.items.length; ++i) if (root.items[i].run.run_id === id) return i
        return -1
    }
    function statusText(s: string): string {
        switch (s) {
        case "starting": return qsTr("Starting")
        case "active": return qsTr("Active")
        case "paused": return qsTr("Paused")
        case "finished": return qsTr("Finished")
        case "abandoned": return qsTr("Abandoned")
        default: return s
        }
    }
    function participantText(s: string): string {
        switch (s) {
        case "invited": return qsTr("Invited")
        case "joined": return qsTr("Joined")
        case "declined": return qsTr("Declined")
        case "left": return qsTr("Left")
        default: return s
        }
    }
    function whenText(iso: string): string {
        const d = new Date(iso)
        return isNaN(d.getTime()) ? "" : Qt.formatDateTime(d, "d MMM, HH:mm")
    }
    function rowOf(it: var): var {
        const joined = it.run.participants.filter(p => p.status === "joined").length
        return { id: it.run.run_id, name: it.run.name, status: root.statusText(it.run.status) + (it.archived ? " · " + qsTr("Archived") : ""),
                 people: joined + " / " + it.run.participants.length, updated: root.whenText(it.run.updated_at) }
    }
    function say(text: string, kind: string) { root.note = text; root.noteKind = kind }

    function load() {
        if (!root.visible || !root.admin.available) return
        if (root.items.length === 0) root.phase = "loading"
        api.call("GET", "/admin/runs", undefined, function (ok, data) {
            if (!ok || !data || data.runs === undefined) { root.phase = "error"; return }
            root.items = data.runs
            root.phase = "ready"
            if (!root.withTable) root.selectedId = root.runId !== "" && root.indexOf(root.runId) >= 0 ? root.runId : ""
            else if (root.indexOf(root.selectedId) < 0) root.selectedId = ""
        })
    }
    onVisibleChanged: if (visible) root.load()
    Component.onCompleted: root.load()
    onRunIdChanged: {
        if (root.withTable) return
        if (root.runId !== "" && root.indexOf(root.runId) >= 0) root.selectedId = root.runId
        else if (root.runId !== "") root.load()
        else root.selectedId = ""
        root.note = ""
    }

    // The answer of every action is the run: it replaces the row; the Runs screen reads its own list again.
    function adopt(detail: var) {
        if (!detail || !detail.run) return
        const next = root.items.slice()
        const i = root.indexOf(detail.run.run_id)
        const entry = { run: detail.run, archived: detail.archived === true }
        if (i >= 0) next[i] = entry; else next.unshift(entry)
        root.items = next
        root.player.runs.refresh()
    }
    function act(action: string, doneText: string, body: var) {   // plain action without a question
        if (root.busy || !root.run) return
        const id = root.run.run_id
        root.busy = true
        root.note = ""
        api.call("POST", "/admin/runs/" + id + "/" + action, body, function (ok, data, code, message) {
            root.busy = false
            if (ok) { root.adopt(data); root.say(doneText, "done") }
            else root.say(code === "confirmation_cancelled" ? qsTr("Not confirmed. Nothing was changed.")
                                                              : api.failText(qsTr("Could not do that"), message), "error")
        })
    }
    function askThen(spec: var, action: string, doneText: string, body: var) {
        const id = root.run.run_id
        spec.label = qsTr("Runs")
        spec.run = (value, finish) => api.call("POST", "/admin/runs/" + id + "/" + action, body, function (ok, data, code, message) {
            if (ok) { root.adopt(data); root.say(doneText, "done") }
            finish(ok, ok ? "" : code === "confirmation_cancelled" ? qsTr("Not confirmed. Nothing was changed.") : api.failText(qsTr("Could not do that"), message))
        })
        ask.ask(spec)
    }
    function names(list: var): string { return list.map(p => p.user_name).join(", ") }
    function finishRun() {
        askThen({ title: qsTr("Finish %1 for everyone?").arg(root.run.name),
                  text: qsTr("The run ends for all people in it. Their saves are not touched."), action: qsTr("Finish run"), danger: true },
                "finish", qsTr("Finished %1").arg(root.run.name))
    }
    function startWithout() {
        askThen({ title: qsTr("Start without %1?").arg(root.names(root.invited)),
                  text: qsTr("The invitations that are still open are removed and the run starts for everyone who joined."), action: qsTr("Start without them") },
                "start", qsTr("Started %1 without %2").arg(root.run.name).arg(root.names(root.invited)), { without_missing: true })
    }
    function removeInvite(p: var) {
        const id = root.run.run_id
        ask.ask({
            label: qsTr("Runs"), title: qsTr("Remove the invitation of %1?").arg(p.user_name),
            text: qsTr("%1 can no longer join %2 with this invitation.").arg(p.user_name).arg(root.run.name),
            action: qsTr("Remove invitation"), danger: true,
            run: (value, finish) => api.call("DELETE", "/admin/runs/" + id + "/invites/" + encodeURIComponent(p.user_id), undefined, function (ok, data, code, message) {
                if (ok) { root.say(qsTr("Removed the invitation of %1").arg(p.user_name), "done"); root.reloadRun(id) }
                finish(ok, ok ? "" : api.failText(qsTr("Could not remove the invitation"), message))
            })
        })
    }
    function resendOne(p: var) {
        const id = root.run.run_id
        api.call("POST", "/admin/runs/" + id + "/invites/" + encodeURIComponent(p.user_id) + "/resend", undefined, function (ok, data, code, message) {
            root.say(ok ? qsTr("Sent the invitation to %1 again").arg(p.user_name) : api.failText(qsTr("Could not send it"), message), ok ? "done" : "error")
        })
    }
    function reloadRun(id: string) {
        api.call("GET", "/admin/runs/" + id, undefined, function (ok, data) { if (ok) root.adopt(data) })
    }

    // ---- export
    Loader {
        id: exportLoader
        active: false
        source: "AdminSaveDialog.qml"
        property string runName: ""
        onLoaded: {
            item.player = root.player
            item.title = qsTr("Save the event log")
            item.suffix = "json"
            item.chosen.connect(root.exportTo)
            item.prepare(exportLoader.suggested())
            item.open()
        }
        function suggested(): string { return "run-" + root.selectedId.slice(0, 8) + "-events.json" }
        onStatusChanged: if (status === Loader.Error) root.say(qsTr("The file dialog is not available in this installation."), "error")
    }
    function exportRun() {
        if (exportLoader.item) { exportLoader.item.prepare(exportLoader.suggested()); exportLoader.item.open() }
        else exportLoader.active = true
    }
    function exportTo(pathOrUrl: string) {
        api.download("/admin/runs/" + root.selectedId + "/export", pathOrUrl, function (ok, filePath, message) {
            root.say(ok ? qsTr("Saved the event log to %1").arg(filePath) : api.failText(qsTr("Could not export the event log"), message), ok ? "done" : "error")
        })
    }

    ColumnLayout {
        id: col
        width: root.width
        spacing: Theme.space2

        // No extra runs: no heading, no table (the runs of the list above are not repeated here).
        UzSectionLabel {
            objectName: "runsAdminHeading"
            Layout.fillWidth: true
            visible: root.hasExtra || (root.withTable && root.phase === "error")
            text: qsTr("More runs · for admins")
            meta: root.hasExtra ? String(root.extraItems.length) : ""
        }
        UzStateBlock {
            objectName: "runsAdminError"
            Layout.fillWidth: true
            visible: root.withTable && root.phase === "error"
            kind: "error"
            title: qsTr("Could not read the runs")
            action: qsTr("Try again")
            onActionClicked: root.load()
        }
        UzTable {
            objectPrefix: "runsAdminTable"
            Layout.fillWidth: true
            visible: root.hasExtra
            emptyText: qsTr("No runs yet.")
            columns: [
                { key: "name", title: qsTr("Run"), fill: 1.4 },
                { key: "status", title: qsTr("Status"), fill: 1 },
                { key: "people", title: qsTr("Joined"), width: 80, align: "right", mono: true },
                { key: "updated", title: qsTr("Changed"), width: 130, align: "right", mono: true }
            ]
            rows: root.extraItems.map(it => root.rowOf(it))
            currentIndex: {
                for (let i = 0; i < root.extraItems.length; ++i) if (root.extraItems[i].run.run_id === root.selectedId) return i
                return -1
            }
            onPicked: (i, row) => { root.selectedId = row.id; root.note = "" }
        }

        ColumnLayout {
            objectName: "runsAdminDetails"
            Layout.fillWidth: true
            visible: root.phase === "ready" && root.run !== null && (root.withTable ? root.hasExtra : true)
            spacing: Theme.space2
            UzSectionLabel {
                Layout.fillWidth: true
                text: root.withTable ? (root.run ? root.run.name : "") : qsTr("Admin actions")
                meta: root.run ? root.statusText(root.status) : ""
            }
            UzStatusIcon {
                objectName: "runsAdminNote"
                Layout.fillWidth: true
                visible: root.note !== ""
                kind: root.noteKind
                small: true
                text: root.note
                maxWidth: 640
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space1
                UzButton { objectName: "runsAdminStart"; size: "sm"; kind: "primary"; visible: root.status === "starting"; enabled: !root.busy; text: qsTr("Start now"); onClicked: root.act("start", qsTr("Started %1").arg(root.run.name)) }
                UzButton { objectName: "runsAdminStartWithout"; size: "sm"; visible: root.status === "starting" && root.invited.length > 0; enabled: !root.busy; text: qsTr("Start without the others…"); onClicked: root.startWithout() }
                UzButton { objectName: "runsAdminPause"; size: "sm"; visible: root.status === "active"; enabled: !root.busy; text: qsTr("Pause run"); onClicked: root.act("pause", qsTr("Paused %1").arg(root.run.name)) }
                UzButton { objectName: "runsAdminResume"; size: "sm"; visible: root.status === "paused"; enabled: !root.busy; text: qsTr("Resume run"); onClicked: root.act("resume", qsTr("Resumed %1").arg(root.run.name)) }
                UzButton { objectName: "runsAdminResend"; size: "sm"; visible: !root.over && root.invited.length > 0; enabled: !root.busy; text: qsTr("Send invitations again"); onClicked: root.act("resend", qsTr("Sent the invitations again")) }
                UzButton { objectName: "runsAdminArchive"; size: "sm"; visible: root.over && root.sel !== null && !root.sel.archived; enabled: !root.busy; text: qsTr("Archive"); onClicked: root.act("archive", qsTr("Archived %1. The apps no longer list it.").arg(root.run.name)) }
                UzButton { objectName: "runsAdminUnarchive"; size: "sm"; visible: root.sel !== null && root.sel.archived; enabled: !root.busy; text: qsTr("Unarchive"); onClicked: root.act("unarchive", qsTr("%1 is listed again").arg(root.run.name)) }
                UzButton { objectName: "runsAdminExport"; size: "sm"; text: qsTr("Export event log…"); onClicked: root.exportRun() }
                UzButton { objectName: "runsAdminFinish"; size: "sm"; kind: "danger"; visible: !root.over; enabled: !root.busy; text: qsTr("Finish run…"); onClicked: root.finishRun() }
            }
            UzTable {
                objectPrefix: "runsAdminPeople"
                Layout.fillWidth: true
                visible: root.withTable || root.invited.length > 0
                selectable: false
                emptyText: qsTr("Nobody is in this run.")
                columns: [
                    { key: "name", title: qsTr("Person"), fill: 1 },
                    { key: "status", title: qsTr("State"), fill: 1 },
                    { key: "actions", title: "", width: 240, align: "right", custom: true }
                ]
                rows: root.run ? root.run.participants.map(p => ({ name: p.user_name, status: root.participantText(p.status), raw: p })) : []
                cellDelegate: Component {
                    Item {
                        id: cell
                        property var row: null
                        property var column: null
                        implicitHeight: Theme.row44
                        Row {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: cell.row !== null && cell.row.raw.status === "invited" && !root.over
                            UzButton { objectName: cell.row ? "runsAdminPersonResend_" + cell.row.raw.user_id : ""; kind: "link"; text: qsTr("Send again"); onClicked: root.resendOne(cell.row.raw) }
                            UzButton { objectName: cell.row ? "runsAdminPersonRemove_" + cell.row.raw.user_id : ""; kind: "link"; text: qsTr("Remove invitation…"); onClicked: root.removeInvite(cell.row.raw) }
                        }
                    }
                }
            }
        }
    }
}
