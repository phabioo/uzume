import QtQuick
import QtQuick.Layouts
import Uzume.App

// Reports (accounts-and-server.md 6d-app-1 and 6d-app-2) from GET /admin/reports: Tabs All · New · Seen (with the New count), the
// inbox table (status Tag, report, from, sent) and, under it, the details section of the selected report (GET /admin/reports/{id}):
// who sent it, the linked event, the text, the attachments with Download (one file: /files/{name}) and Download all (/archive),
// Mark as seen and Delete. States: loading, server unreachable, no reports, no attachments.
// Left out because the API does not carry it: the game title in the report line (only the linked event title is known).
Item {
    id: root
    objectName: "serverReportsPage"
    required property PlayerController player
    readonly property AdminController admin: root.player.admin

    // "Reports | Errors" (ADR 0035): Errors only when the server advertises reports_v2.
    property string section: "Reports"
    readonly property bool errorsAvailable: root.player.errors.serverSupportsErrors
    readonly property bool showReports: root.section === "Reports" || !root.errorsAvailable
    property string tab: "All"
    property var reports: []
    property int countAll: 0
    property int countNew: 0
    property int countSeen: 0
    property bool loaded: false
    property bool loadFailed: false
    property string loadError: ""
    property bool listBusy: false
    property string selectedId: ""
    property var detail: null            // GET /admin/reports/{id} of the selection
    property string detailError: ""
    property bool actionBusy: false
    property string note: ""
    property string noteKind: "done"
    property var handlers: ({})
    property var inFlight: null
    property string dlPath: ""           // what the save dialog is about to fetch
    property string dlName: ""

    readonly property var shown: root.reports.filter(r => root.tab === "All" || r.status === root.tab.toLowerCase())
    readonly property var rows: root.shown.map(r => ({
        id: String(r.id), status: r.status, title: r.title, event: r.linked_event_title ?? "", from: r.user,
        sent: root.sentText(r.created_at) }))
    readonly property int selectedIndex: root.rows.findIndex(r => r.id === root.selectedId)
    readonly property var current: root.selectedIndex >= 0 ? root.shown[root.selectedIndex] : null

    function sentText(iso: string): string {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const now = new Date()
        const same = d.getFullYear() === now.getFullYear() && d.getMonth() === now.getMonth() && d.getDate() === now.getDate()
        return same ? qsTr("Today %1").arg(Qt.formatTime(d, "HH:mm")) : Qt.formatDate(d, "d MMM")
    }
    function bytesText(n: real): string {
        const gib = Math.pow(1024, 3), mib = Math.pow(1024, 2)
        const trim = (v) => { const t = v.toFixed(1); return t.endsWith(".0") ? t.slice(0, -2) : t }
        if (n >= gib) return qsTr("%1 GB").arg(trim(n / gib))
        if (n >= mib) return qsTr("%1 MB").arg(trim(n / mib))
        return qsTr("%1 KB").arg(Math.max(1, Math.round(n / 1024)))
    }
    function errorText(code: string, message: string): string {
        if (code === "confirmation_cancelled") return qsTr("Cancelled. Nothing has changed.")
        return message !== "" ? message : qsTr("The server did not answer.")
    }
    function say(text: string, kind: string) {
        root.note = text
        root.noteKind = kind ?? "done"
    }

    // ------------------------------------------------------------ calls
    function call(method: string, path: string, done: var) {
        // A reply can be emitted before request() returns: the handler in flight takes it then.
        root.inFlight = done
        const id = root.admin.request(method, path)
        if (root.inFlight !== null) root.handlers[id] = done
        root.inFlight = null
    }
    Connections {
        target: root.admin
        function onReplied(id, ok, status, data, code, message) {
            const h = root.handlers[id] ?? root.inFlight
            if (!h) return
            if (root.handlers[id] === undefined) root.inFlight = null
            delete root.handlers[id]
            h(ok, data, code, message, status)
        }
        function onDownloadFinished(id, ok, filePath, message) {
            // The page has at most one download at a time (the buttons are disabled meanwhile); the signal can come before
            // download() returns, so the id is not compared.
            if (!root.actionBusy) return
            const label = root.dlName
            root.actionBusy = false
            root.say(ok ? qsTr("Saved %1 to %2").arg(label).arg(filePath)
                        : qsTr("Could not save %1: %2").arg(label).arg(message !== "" ? message : qsTr("the server did not answer.")),
                     ok ? "done" : "error")
        }
    }

    property bool reloadAgain: false
    function reload() {
        if (root.listBusy) { root.reloadAgain = true; return }
        root.listBusy = true
        root.call("GET", "/admin/reports", (ok, data, code, message) => {
            root.listBusy = false
            if (root.reloadAgain) { root.reloadAgain = false; Qt.callLater(root.reload) }
            if (ok && data && data.reports !== undefined) {
                root.reports = data.reports
                root.countAll = data.count_all ?? root.reports.length
                root.countNew = data.count_new ?? 0
                root.countSeen = data.count_seen ?? 0
                root.loaded = true
                root.loadFailed = false
                root.admin.refreshCounts()
                if (root.selectedId !== "" && root.current === null) { root.selectedId = ""; root.detail = null }
            } else if (!root.loaded) {
                root.loadFailed = true
                root.loadError = root.errorText(code, message)
            }
        })
    }
    function select(id: string) {
        root.selectedId = id
        root.detail = null
        root.detailError = ""
        root.call("GET", "/admin/reports/" + encodeURIComponent(id), (ok, data, code, message) => {
            if (root.selectedId !== id) return
            if (ok && data && data.id !== undefined) root.detail = data
            else root.detailError = root.errorText(code, message)
        })
    }
    onVisibleChanged: if (visible) { root.reload(); if (root.errorsAvailable) errorsView.reload() }
    onErrorsAvailableChanged: if (root.errorsAvailable && root.visible) errorsView.reload()
    Component.onCompleted: { root.reload(); if (root.errorsAvailable) errorsView.reload() }

    // ------------------------------------------------------------ actions
    function markSeen() {
        if (root.detail === null || root.actionBusy) return
        const id = String(root.detail.id)
        root.actionBusy = true
        root.call("POST", "/admin/reports/" + encodeURIComponent(id) + "/seen", (ok, data, code, message) => {
            root.actionBusy = false
            if (ok && data && data.id !== undefined) {
                if (root.selectedId === id) root.detail = data
                root.say(qsTr("Report #%1 is marked as seen.").arg(id), "done")
                root.reload()
            } else {
                root.say(root.errorText(code, message), "error")
            }
        })
    }
    function deleteReport() {
        if (root.detail === null) return
        const id = String(root.detail.id)
        reportDialog.openDelete(id)
    }
    function startDownload(path: string, label: string, target: string) {
        root.actionBusy = true
        root.dlName = label
        root.admin.download(path, target)
    }
    function askSave(path: string, label: string, suggested: string, ext: string) {
        root.dlPath = path
        root.dlName = label
        saveLoader.suggested = suggested
        saveLoader.ext = ext
        saveLoader.open()
    }
    function downloadFile(file: var) {
        root.askSave("/admin/reports/" + encodeURIComponent(root.detail.id) + "/files/" + encodeURIComponent(file.name), file.name, file.name,
                     file.name.indexOf(".") > 0 ? file.name.slice(file.name.lastIndexOf(".") + 1) : "")
    }
    function downloadAll() {
        root.askSave("/admin/reports/" + encodeURIComponent(root.detail.id) + "/archive", qsTr("report #%1").arg(root.detail.id),
                     "uzume-report-" + root.detail.id + ".zip", "zip")
    }

    Loader {
        id: saveLoader
        property string suggested: ""
        property string ext: "txt"
        active: false
        source: "AdminSaveDialog.qml"
        function open() {
            if (item) {
                item.suffix = saveLoader.ext
                item.prepare(saveLoader.suggested)
                item.open()
            } else {
                active = true
            }
        }
        onLoaded: {
            item.player = root.player
            item.title = qsTr("Save report file")
            item.suffix = saveLoader.ext
            item.chosen.connect((target) => root.startDownload(root.dlPath, root.dlName, target))
            item.prepare(saveLoader.suggested)
            item.open()
        }
        onStatusChanged: if (status === Loader.Error) root.say(qsTr("The file dialog is not available in this installation."), "error")
    }

    // ------------------------------------------------------------ page
    AdminPage {
        anchors.fill: parent
        player: root.player
        title: qsTr("Reports")
        label: root.loaded ? qsTr("%1 · %2 new").arg(Theme.plural(root.countAll, qsTr("report"), qsTr("reports"))).arg(root.countNew) : qsTr("Server")
        area: qsTr("Reports")
        helpItems: root.showReports ? [
            { title: qsTr("Reports"), text: qsTr("Problem reports sent from the apps of this server. Opening one does not mark it as seen.") },
            { title: qsTr("Attachments"), text: qsTr("Attachments are kept 30 days. The description stays.") }
        ] : [
            { title: qsTr("Errors"), text: qsTr("Errors that apps of this server sent on their own, such as a crash, a core that does not load or a plugin that stops, grouped by problem. Apps send errors only when their person turned that on.") },
            { title: qsTr("Grouping"), text: qsTr("Equal errors count as one problem. Opening one shows its events and the devices it happened on.") }
        ]
        notice: [
            UzStatusIcon {
                objectName: "reportsNote"
                visible: root.note !== ""
                kind: root.noteKind
                small: true
                text: root.note
                maxWidth: 640
            }
        ]

        UzSegmented {
            objectName: "reportsSections"
            visible: root.errorsAvailable
            Layout.alignment: Qt.AlignLeft
            accessibleName: qsTr("Show")
            options: [
                { value: "Reports", label: root.countNew > 0 ? qsTr("Reports · %1 new").arg(root.countNew) : qsTr("Reports"), name: "reportsSectionReports" },
                { value: "Errors", label: errorsView.countNew > 0 ? qsTr("Errors · %1 new").arg(errorsView.countNew) : qsTr("Errors"), name: "reportsSectionErrors" }
            ]
            current: root.showReports ? "Reports" : "Errors"
            onPicked: (value) => root.section = value
        }
        ServerErrorsView {
            id: errorsView
            objectName: "serverErrorsView"
            Layout.fillWidth: true
            visible: root.errorsAvailable && !root.showReports
            player: root.player
            dialogHost: root
            onSay: (text, kind) => root.say(text, kind)
        }

        UzTabs {
            objectName: "reportsTabs"
            visible: root.showReports
            items: ["All", "New", "Seen"]
            active: root.tab
            counts: ({ "New": root.countNew })
            onPicked: (label) => root.tab = label
        }
        UzStateBlock {
            objectName: "reportsLoading"
            Layout.fillWidth: true
            visible: root.showReports && !root.loaded && !root.loadFailed
            kind: "loading"
            title: qsTr("Loading reports")
        }
        UzStateBlock {
            objectName: "reportsError"
            Layout.fillWidth: true
            visible: root.showReports && root.loadFailed
            kind: "error"
            title: qsTr("Can't read the reports")
            text: root.loadError
            action: qsTr("Try again")
            onActionClicked: { root.loadFailed = false; root.reload() }
        }
        UzStateBlock {
            objectName: "reportsEmpty"
            Layout.fillWidth: true
            visible: root.showReports && root.loaded && root.reports.length === 0
            kind: "empty"
            title: qsTr("No reports")
            text: qsTr("When someone sends a problem report from their app, it shows up here and Reports gets a count.")
        }

        UzTable {
            id: table
            objectPrefix: "reportsTable"
            Layout.fillWidth: true
            visible: root.showReports && root.loaded && root.reports.length > 0
            emptyText: root.tab === "New" ? qsTr("No new reports.") : qsTr("No reports have been seen yet.")
            columns: [
                { key: "status", title: qsTr("Status"), width: 80, custom: true },
                { key: "title", title: qsTr("Report"), fill: 2, custom: true },
                { key: "from", title: qsTr("From"), fill: 1 },
                { key: "sent", title: qsTr("Sent"), width: 100, mono: true }
            ]
            rows: root.rows
            currentIndex: root.selectedIndex
            onPicked: (i, row) => root.select(row.id)
            cellDelegate: Component {
                Item {
                    id: cell
                    property var row: null
                    property var column: null
                    implicitHeight: cell.column && cell.column.key === "status" ? 18 : col.implicitHeight
                    UzTag {
                        objectName: "reportStatus_" + (cell.row ? cell.row.id : "")
                        visible: cell.column !== null && cell.column.key === "status"
                        text: cell.row ? (cell.row.status === "new" ? qsTr("New") : qsTr("Seen")) : ""
                        tone: cell.row && cell.row.status === "new" ? "accent" : "neutral"
                        variant: cell.row && cell.row.status === "new" ? "plane" : "outline"
                    }
                    Column {
                        id: col
                        visible: cell.column !== null && cell.column.key === "title"
                        width: parent.width
                        Text {
                            objectName: "reportTitle_" + (cell.row ? cell.row.id : "")
                            width: parent.width
                            elide: Text.ElideRight
                            text: cell.row ? cell.row.title : ""
                            textFormat: Text.PlainText
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.body.size
                            font.weight: cell.row && cell.row.status === "new" ? Font.Medium : Font.Normal
                            color: Theme.ink
                        }
                        Text {
                            visible: cell.row !== null && cell.row.event !== ""
                            width: parent.width
                            elide: Text.ElideRight
                            text: cell.row ? qsTr("Linked: %1").arg(cell.row.event) : ""
                            textFormat: Text.PlainText
                            font.family: Theme.mono
                            font.pixelSize: Theme.roles.small.size
                            color: Theme.muted
                        }
                    }
                }
            }

            ColumnLayout {   // details section of the selected report
                objectName: "reportDetails"
                width: parent ? parent.width : 0
                visible: root.current !== null
                spacing: Theme.space2
                Item { Layout.preferredHeight: Theme.space2 }
                UzSectionLabel {
                    Layout.fillWidth: true; Layout.leftMargin: Theme.space15; Layout.rightMargin: Theme.space15
                    text: root.current !== null ? qsTr("Report #%1").arg(root.current.id) : ""
                }
                UzStatusIcon {
                    objectName: "reportDetailError"
                    Layout.leftMargin: Theme.space15
                    visible: root.detailError !== ""
                    kind: "error"
                    small: true
                    text: root.detailError
                    maxWidth: 560
                }
                UzSpinner { visible: root.detail === null && root.detailError === ""; Layout.leftMargin: Theme.space15 }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.space15
                    Layout.rightMargin: Theme.space15
                    visible: root.detail !== null
                    spacing: Theme.space2
                    GridLayout {
                        columns: 2
                        columnSpacing: Theme.space3
                        rowSpacing: Theme.space1
                        Layout.fillWidth: true
                        Text { text: qsTr("From"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                        Text {
                            objectName: "reportFrom"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: {
                                if (root.detail === null) return ""
                                const parts = [root.detail.user, root.detail.device]
                                if (root.detail.player_version) parts.push(root.detail.player_version)
                                if (root.detail.os) parts.push(root.detail.os)
                                return parts.join(" · ")
                            }
                            font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                        }
                        Text {
                            visible: linked.visible
                            text: qsTr("Linked"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted
                        }
                        Text {
                            id: linked
                            objectName: "reportLinked"
                            visible: root.detail !== null && (root.detail.linked_event_title ?? "") !== ""
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: {
                                if (!visible) return ""
                                const t = root.detail.linked_event_time ? new Date(root.detail.linked_event_time) : null
                                return t && !isNaN(t.getTime()) ? qsTr("%1 · %2").arg(root.detail.linked_event_title).arg(Qt.formatTime(t, "HH:mm"))
                                                                : root.detail.linked_event_title
                            }
                            font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                        }
                    }
                    Text {
                        objectName: "reportDescription"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        text: root.detail !== null ? "“" + root.detail.description + "”" : ""
                        font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink2
                    }
                    UzSectionLabel {
                        Layout.fillWidth: true
                        text: root.detail !== null ? qsTr("Attachments · %1").arg((root.detail.files ?? []).length) : ""
                        meta: root.detail !== null && (root.detail.files ?? []).length > 0 ? root.bytesText(root.detail.size) : ""
                    }
                    Text {
                        objectName: "reportNoFiles"
                        Layout.fillWidth: true
                        visible: root.detail !== null && (root.detail.files ?? []).length === 0
                        wrapMode: Text.Wrap
                        text: qsTr("No attachments. Attachments are kept 30 days; the description stays.")
                        font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted
                    }
                    Repeater {
                        model: root.detail !== null ? (root.detail.files ?? []) : []
                        delegate: RowLayout {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            Text {
                                objectName: "reportFile_" + index
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: modelData.name
                                font.family: Theme.mono; font.pixelSize: Theme.roles.meta11.size; color: Theme.ink
                            }
                            UzMono { text: root.bytesText(modelData.size) }
                            UzButton {
                                objectName: "reportFileDownload_" + index
                                size: "sm"
                                kind: "ghost"
                                text: qsTr("Download")
                                enabled: !root.actionBusy
                                onClicked: root.downloadFile(modelData)
                            }
                        }
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: Theme.space1
                        UzButton {
                            objectName: "reportDownloadAll"
                            visible: root.detail !== null && (root.detail.files ?? []).length > 0
                            text: qsTr("Download all")
                            enabled: !root.actionBusy
                            onClicked: root.downloadAll()
                        }
                        UzButton {
                            objectName: "reportMarkSeen"
                            kind: "primary"
                            visible: root.detail !== null && root.detail.status === "new"
                            text: qsTr("Mark as seen")
                            busy: root.actionBusy
                            onClicked: root.markSeen()
                        }
                        UzButton {
                            objectName: "reportDelete"
                            kind: "danger"
                            text: qsTr("Delete…")
                            enabled: !root.actionBusy
                            onClicked: root.deleteReport()
                        }
                    }
                }
                Item { Layout.preferredHeight: Theme.space2 }
            }
        }
    }

    UzDialog {
        id: reportDialog
        objectName: "reportsDeleteDialog"
        property string reportId: ""
        function openDelete(id: string) {
            reportDialog.reportId = id
            reportDialog.error = ""
            reportDialog.busy = false
            reportDialog.open()
        }
        label: qsTr("Reports")
        title: qsTr("Delete report #%1?").arg(reportDialog.reportId)
        text: qsTr("The description and all attachments are deleted for good.")
        actions: [
            UzButton { objectName: "reportDeleteCancel"; text: qsTr("Cancel"); enabled: !reportDialog.busy; onClicked: reportDialog.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "reportDeleteConfirm"
                kind: "danger"
                text: qsTr("Delete")
                busy: reportDialog.busy
                onClicked: {
                    reportDialog.error = ""
                    reportDialog.busy = true
                    const id = reportDialog.reportId
                    root.call("DELETE", "/admin/reports/" + encodeURIComponent(id), (ok, data, code, message) => {
                        reportDialog.busy = false
                        if (ok) {
                            reportDialog.close()
                            if (root.selectedId === id) { root.selectedId = ""; root.detail = null }
                            root.say(qsTr("Report #%1 was deleted.").arg(id), "done")
                            root.reload()
                        } else {
                            reportDialog.error = root.errorText(code, message)
                        }
                    })
                }
            }
        ]
    }
}
