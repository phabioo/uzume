import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server status (accounts-and-server.md 6f-app-1 and 6f-app-2) from GET /admin/status (`player.admin.status`, read again every
// 60 s and when the page opens). Title = server name, Mono label `<version> · UP <uptime> · <public address>`; "Needs attention · n"
// as one warning sentence per open problem (`player.attention`: server problems, core problems, a server update, save conflicts)
// with exactly one action each; the health facts in three columns (Sessions now, Apps connected, Relay and what the server
// reports: CPU, Memory, Disk, Backup disk); the tables
// Sessions and Connected apps; the last errors in the help column. States: loading, server unreachable (Try again, Connection
// check, hint), the member block (ServerScreen).
// What the API does not carry is left out: no relay Mbit/s, no Direct / Relayed per Session, no system per Session.
AdminPage {
    id: root
    objectName: "serverStatusPage"
    readonly property AdminController admin: root.player.admin
    readonly property var st: root.admin.status
    readonly property string phase: root.admin.statusState
    readonly property bool hasData: root.st.hub_name !== undefined
    readonly property bool ready: root.phase === "ready" && root.hasData
    readonly property bool unreachable: root.phase === "unreachable"
    readonly property bool forbidden: root.phase === "forbidden"
    readonly property var problems: root.ready ? (root.st.problems ?? []) : []
    readonly property var attention: root.ready ? root.player.attention : []
    readonly property var sessionRows: root.ready ? root.sessionsOf(root.st.sessions ?? []) : []
    readonly property var appRows: root.ready ? root.appsOf(root.st.players ?? []) : []
    readonly property var metricList: root.ready ? root.metricsOf(root.st) : []
    property string logNote: ""
    property string logNoteKind: "done"
    property int logId: -1
    property bool logBusy: false

    title: root.hasData ? root.st.hub_name : root.player.hubName
    // The header of the Server screen already shows address, state and version: no toolbar line while the data is fresh.
    label: root.ready ? ""
         : root.unreachable && root.hasData ? qsTr("Last known · %1").arg(root.st.hub_version)
         : qsTr("Server")
    area: qsTr("Overview")
    helpItems: root.ready ? root.errorHelp(root.st.errors ?? []) : []
    helpLines: root.ready ? [qsTr("Read %1").arg(root.clockText(root.st.time))] : []

    // ------------------------------------------------------------ formatting
    function uptimeText(seconds: real): string {
        const s = Math.max(0, Math.floor(seconds))
        const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60)
        if (d > 0) return qsTr("%1 %2 h").arg(Theme.plural(d, qsTr("day"), qsTr("days"))).arg(h)
        if (h > 0) return qsTr("%1 h %2 min").arg(h).arg(m)
        return qsTr("%1 min").arg(m)
    }
    function bytesText(n: real): string {
        const gib = Math.pow(1024, 3), mib = Math.pow(1024, 2)
        const trim = (v) => { const t = v.toFixed(1); return t.endsWith(".0") ? t.slice(0, -2) : t }
        if (n >= gib) return qsTr("%1 GB").arg(trim(n / gib))
        if (n >= mib) return qsTr("%1 MB").arg(Math.round(n / mib))
        return qsTr("%1 KB").arg(Math.max(1, Math.round(n / 1024)))
    }
    function clockText(iso: string): string {
        const d = new Date(iso)
        return isNaN(d.getTime()) ? "" : Qt.formatTime(d, "HH:mm")
    }
    function sinceText(iso: string): string {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const min = Math.max(0, Math.floor((Date.now() - d.getTime()) / 60000))
        if (min < 1) return qsTr("Just now")
        if (min < 60) return qsTr("%1 min").arg(min)
        return qsTr("%1 h %2 min").arg(Math.floor(min / 60)).arg(min % 60)
    }
    function stateText(state: string): string {
        switch (state) {
        case "in_session": return qsTr("In a Session")
        case "watching": return qsTr("Watching")
        case "playing": return qsTr("Playing")
        default: return qsTr("Idle")
        }
    }
    function hasProblem(prefix: string): bool {
        for (const p of root.problems) if ((p.id ?? "").indexOf(prefix) === 0) return true
        return false
    }

    // ------------------------------------------------------------ data
    function metricsOf(s: var): var {
        const out = []
        if (s.cpu_percent !== undefined) {
            out.push({ key: "cpu", label: qsTr("CPU"), value: qsTr("%1 %").arg(Math.round(s.cpu_percent)), fraction: s.cpu_percent / 100,
                       line: qsTr("Whole machine"), warning: root.hasProblem("cpu") })
        }
        const mem = s.memory ?? {}
        if (mem.system_total_bytes > 0 && mem.system_used_bytes !== undefined) {
            out.push({ key: "memory", label: qsTr("Memory"), value: root.bytesText(mem.system_used_bytes), fraction: mem.system_used_bytes / mem.system_total_bytes,
                       line: qsTr("of %1").arg(root.bytesText(mem.system_total_bytes)), warning: root.hasProblem("mem") })
        } else if (mem.process_bytes !== undefined) {
            out.push({ key: "memory", label: qsTr("Memory"), value: root.bytesText(mem.process_bytes), fraction: -1,
                       line: qsTr("Used by the server"), warning: root.hasProblem("mem") })
        }
        const disk = s.disk
        if (disk && disk.total_bytes > 0) {
            const f = (disk.total_bytes - disk.free_bytes) / disk.total_bytes
            out.push({ key: "disk", label: qsTr("Disk"), value: qsTr("%1 %").arg(Math.round(f * 100)), fraction: f,
                       line: qsTr("%1 free").arg(root.bytesText(disk.free_bytes)), warning: root.hasProblem("disk") || f >= 0.9 })
        }
        const bd = s.backup_disk
        if (bd && bd.total_bytes > 0 && bd.free_bytes !== undefined) {
            const f = (bd.total_bytes - bd.free_bytes) / bd.total_bytes
            out.push({ key: "backup", label: qsTr("Backup disk"), value: qsTr("%1 %").arg(Math.round(f * 100)), fraction: f,
                       line: qsTr("%1 free").arg(root.bytesText(bd.free_bytes)), warning: root.hasProblem("backup") || f >= 0.9 })
        }
        // Facts of the Sessions and the relay, in the same grid: only what the server reports.
        const sessions = s.sessions ?? []
        const watching = s.viewers ?? 0
        out.push({ key: "sessionsNow", label: qsTr("Sessions now"), value: String(sessions.length), fraction: -1,
                      line: watching > 0 ? qsTr("%1 running · %2 watching").arg(sessions.length).arg(watching) : qsTr("%1 running").arg(sessions.length), warning: false })
        out.push({ key: "appsConnected", label: qsTr("Apps connected"), value: String((s.players ?? []).length), fraction: -1,
                           line: qsTr("%1 online now").arg((s.players ?? []).filter(p => p.state !== undefined).length), warning: false })
        out.push({ key: "relay", label: qsTr("Relay"), value: s.turn ? qsTr("On") : qsTr("Off"), fraction: -1,
                           line: s.turn ? qsTr("%1 in use").arg(Theme.plural(s.turn.allocations, qsTr("allocation"), qsTr("allocations"))) : qsTr("Not running"), warning: false })
        return out
    }
    function sessionsOf(list: var): var {
        return list.map(s => ({ host: s.owner, game: s.game_title, device: s.device_name, viewers: String(s.viewers ?? 0), since: root.sinceText(s.started_at) }))
    }
    function appsOf(list: var): var {
        return list.map(p => ({ person: p.user, device: p.device_name, version: p.version, state: root.stateText(p.state),
                                note: p.outdated ? qsTr("Update offered") : "" }))
    }
    function errorHelp(errors: var): var {
        const out = [{ title: qsTr("Last errors"), text: errors.length === 0 ? qsTr("No warnings or errors in the last 24 hours.")
                                                                          : qsTr("Warnings and errors of the last 24 hours, newest first.") }]
        for (const e of errors.slice(0, 5)) {
            out.push({ title: (e.level === "error" ? qsTr("Error") : qsTr("Warning")) + " · " + root.clockText(e.time), text: e.message })
        }
        return out
    }

    // ------------------------------------------------------------ server log
    function saveLog(pathOrUrl: string) {
        root.logNote = ""
        root.logBusy = true
        root.logId = root.admin.download("/admin/status/log", pathOrUrl)
    }
    function suggestedLogName(): string {
        return "uzume-server-log-" + Qt.formatDateTime(new Date(), "yyyyMMdd-HHmmss") + ".txt"
    }
    Connections {
        target: root.admin
        function onDownloadFinished(id, ok, filePath, message) {
            if (id !== root.logId) return
            root.logBusy = false
            root.logNoteKind = ok ? "done" : "error"
            root.logNote = ok ? qsTr("Saved the server log to %1").arg(filePath)
                              : qsTr("Could not save the server log: %1").arg(message !== "" ? message : qsTr("the server did not answer."))
        }
    }
    Loader {
        id: logDialog
        active: false
        source: "AdminSaveDialog.qml"
        function open() {
            if (item) {
                item.prepare(root.suggestedLogName())
                item.open()
            } else {
                active = true
            }
        }
        onLoaded: {
            item.player = root.player
            item.title = qsTr("Save server log")
            item.suffix = "txt"
            item.chosen.connect(root.saveLog)
            item.prepare(root.suggestedLogName())
            item.open()
        }
        onStatusChanged: if (status === Loader.Error) {
            root.logNoteKind = "error"
            root.logNote = qsTr("The file dialog is not available in this installation.")
        }
    }

    onVisibleChanged: if (visible) root.admin.refreshStatus()
    Component.onCompleted: root.admin.refreshStatus()

    headerActions: [
        UzButton {
            objectName: "statusConnectionCheck"
            size: "sm"
            visible: root.player.connectionCheck.available
            text: qsTr("Connection check")
            onClicked: root.player.connectionCheck.show()
        },
        UzButton {
            objectName: "statusCollectLogs"
            size: "sm"
            kind: "ghost"
            visible: root.ready
            busy: root.logBusy
            text: qsTr("Collect server logs…")
            onClicked: logDialog.open()
        }
    ]

    notice: [
        UzStatusIcon {
            objectName: "statusLogNote"
            visible: root.logNote !== ""
            kind: root.logNoteKind
            small: true
            text: root.logNote
            maxWidth: 640
        },
        UzStatusIcon {
            objectName: "statusAllFine"
            visible: root.ready && root.attention.length === 0
            kind: "done"
            text: qsTr("Everything looks fine.")
        }
    ]

    // ------------------------------------------------------------ states
    UzStateBlock {
        objectName: "statusLoading"
        Layout.fillWidth: true
        visible: !root.ready && !root.unreachable && !root.forbidden
        kind: "loading"
        title: qsTr("Loading server status")
    }
    UzStateBlock {
        objectName: "statusForbidden"
        Layout.fillWidth: true
        visible: root.forbidden
        kind: "empty"
        title: qsTr("Only admins see server status")
        text: qsTr("Your account is not an admin of this server.")
        action: qsTr("Back to Library")
        onActionClicked: root.player.showLibrary()
    }
    ColumnLayout {
        objectName: "statusUnreachable"
        Layout.fillWidth: true
        visible: root.unreachable
        spacing: Theme.space2
        UzStateBlock {
            Layout.fillWidth: true
            kind: "error"
            title: qsTr("Can't reach the server")
            text: root.hasData ? qsTr("Last known: %1 · up %2.").arg(root.st.hub_version).arg(root.uptimeText(root.st.uptime_seconds))
                               : qsTr("The status could not be read.")
            action: qsTr("Try again")
            onActionClicked: root.admin.refreshStatus()
        }
        UzMono {
            Layout.fillWidth: true
            text: qsTr("On the device: uzume-server status")
            wrapMode: Text.Wrap
        }
    }

    // ------------------------------------------------------------ ready
    ColumnLayout {
        objectName: "statusAttention"
        Layout.fillWidth: true
        visible: root.ready && root.attention.length > 0
        spacing: Theme.space1
        UzSectionLabel { objectName: "statusAttentionHead"; Layout.fillWidth: true; text: qsTr("Needs attention · %1").arg(root.attention.length) }
        Repeater {
            model: root.attention
            delegate: RowLayout {
                id: arow
                required property var modelData
                required property int index
                objectName: "statusAttentionRow_" + index
                Layout.fillWidth: true
                spacing: Theme.space2
                UzStatusIcon {
                    objectName: "statusAttentionText_" + arow.index
                    Layout.fillWidth: true
                    kind: arow.modelData.severity === "error" ? "error" : "warning"
                    text: arow.modelData.text
                    maxWidth: 720
                }
                UzButton {
                    objectName: "statusAttentionAction_" + arow.index
                    size: "sm"
                    text: arow.modelData.actionLabel
                    onClicked: root.player.openAttention(arow.modelData.target)
                }
            }
        }
    }

    GridLayout {
        id: metricsGrid
        objectName: "statusMetrics"
        Layout.fillWidth: true
        visible: root.ready && root.metricList.length > 0
        columns: root.width >= 640 ? 3 : 1
        columnSpacing: 0
        rowSpacing: Theme.space3
        Repeater {
            model: root.metricList
            delegate: UzMetric {
                required property var modelData
                required property int index
                objectName: "metric_" + modelData.key
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                leftRule: metricsGrid.columns === 3 && index % 3 > 0
                label: modelData.label
                value: modelData.value
                fraction: modelData.fraction
                line: modelData.line
                warning: modelData.warning
            }
        }
    }

    ColumnLayout {
        objectName: "statusSessions"
        Layout.fillWidth: true
        visible: root.ready
        spacing: Theme.space1
        UzSectionLabel {
            Layout.fillWidth: true
            text: qsTr("Sessions · %1").arg(root.sessionRows.length)
            meta: {
                const parts = []
                if ((root.st.viewers ?? 0) > 0) parts.push(Theme.plural(root.st.viewers, qsTr("viewer"), qsTr("viewers")))
                if (root.st.turn) parts.push(qsTr("Relay · %1").arg(Theme.plural(root.st.turn.allocations, qsTr("allocation"), qsTr("allocations"))))
                return parts.join(" · ")
            }
        }
        UzTable {
            objectPrefix: "sessionsTable"
            Layout.fillWidth: true
            selectable: false
            emptyText: qsTr("No Sessions are running.")
            columns: [
                { key: "host", title: qsTr("Host"), fill: 1 },
                { key: "game", title: qsTr("Game"), fill: 1.5 },
                { key: "device", title: qsTr("Device"), fill: 1 },
                { key: "viewers", title: qsTr("Viewers"), width: 80, align: "right", mono: true },
                { key: "since", title: qsTr("Running"), width: 110, align: "right", mono: true }
            ]
            rows: root.sessionRows
        }
    }

    ColumnLayout {
        objectName: "statusApps"
        Layout.fillWidth: true
        visible: root.ready
        spacing: Theme.space1
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Connected apps · %1").arg(root.appRows.length) }
        UzTable {
            objectPrefix: "appsTable"
            Layout.fillWidth: true
            selectable: false
            emptyText: qsTr("No apps are connected.")
            columns: [
                { key: "person", title: qsTr("Person"), fill: 1 },
                { key: "device", title: qsTr("Device"), fill: 1 },
                { key: "version", title: qsTr("App version"), width: 110, mono: true },
                { key: "state", title: qsTr("State"), fill: 0.8 },
                { key: "note", title: "", fill: 0.8 }
            ]
            rows: root.appRows
        }
    }
}
