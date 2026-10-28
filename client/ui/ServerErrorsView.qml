import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server > Reports > Errors (ADR 0035 D1, D2): the error groups of this server from GET /admin/reports/errors, one row per group
// (new mark, title, component, count, devices, last seen, newest version, linked issue) and, under the table, the details of the
// selected group (GET /admin/reports/errors/{fingerprint}): the newest events and the actions Report on GitHub, Link issue,
// Mark as seen and Delete. "Report on GitHub" looks the fingerprint up in the public issues first (no credentials); a hit becomes
// the group's issue, otherwise a preview shows the exact public text and the prefilled address opens in the system browser. The
// button is hidden when the server names no repository for the group (issues_base null). No embedded browser.
// Shown by ServerReportsPage only when the server advertises reports_v2.
Item {
    id: root
    objectName: "serverErrorsView"
    required property PlayerController player
    required property Item dialogHost   // the dialogs fill this item (the page), not this view
    readonly property AdminController admin: root.player.admin
    readonly property ErrorsController errors: root.player.errors
    signal say(string text, string kind)

    property var groups: []
    property int countNew: 0
    property bool loaded: false
    property bool loadFailed: false
    property string loadError: ""
    property bool listBusy: false
    property string selectedFp: ""
    property var detail: null
    property string detailError: ""
    property bool actionBusy: false
    property bool lookupBusy: false
    property var handlers: ({})
    property var inFlight: null

    readonly property var rows: root.groups.map(g => ({
        fp: g.fingerprint, isNew: g.seen === false, title: g.title, issue: g.issue_url ?? "",
        component: g.component, count: String(g.count), devices: String(g.devices),
        last: root.sentText(g.last_seen), version: (g.versions ?? []).length > 0 ? g.versions[0] : "" }))
    readonly property int selectedIndex: root.rows.findIndex(r => r.fp === root.selectedFp)
    readonly property var current: root.selectedIndex >= 0 ? root.groups[root.selectedIndex] : null
    readonly property string issuesBase: root.detail !== null && root.detail.issues_base ? root.detail.issues_base : ""
    readonly property string issueUrl: root.detail !== null && root.detail.issue_url ? root.detail.issue_url : ""

    implicitHeight: col.implicitHeight

    function sentText(iso: string): string {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const now = new Date()
        const same = d.getFullYear() === now.getFullYear() && d.getMonth() === now.getMonth() && d.getDate() === now.getDate()
        return same ? qsTr("Today %1").arg(Qt.formatTime(d, "HH:mm")) : Qt.formatDate(d, "d MMM")
    }
    function whenText(iso: string): string {
        const d = new Date(iso)
        return isNaN(d.getTime()) ? "" : Qt.formatDateTime(d, "d MMM HH:mm")
    }
    function issueNumber(url: string): string {
        const m = /\/issues\/(\d+)$/.exec(url)
        return m ? m[1] : ""
    }
    function errorText(code: string, message: string): string {
        if (code === "confirmation_cancelled") return qsTr("Cancelled. Nothing has changed.")
        return message !== "" ? message : qsTr("The server did not answer.")
    }

    // ------------------------------------------------------------ calls
    function call(method: string, path: string, body: var, done: var) {
        // A reply can be emitted before request() returns: the handler in flight takes it then.
        root.inFlight = done
        const id = root.admin.request(method, path, body)
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
    }

    property bool reloadAgain: false
    function reload() {
        if (root.listBusy) { root.reloadAgain = true; return }
        root.listBusy = true
        root.call("GET", "/admin/reports/errors", undefined, (ok, data, code, message) => {
            root.listBusy = false
            if (root.reloadAgain) { root.reloadAgain = false; Qt.callLater(root.reload) }
            if (ok && data && data.groups !== undefined) {
                root.groups = data.groups
                root.countNew = data.new ?? 0
                root.loaded = true
                root.loadFailed = false
                root.admin.refreshCounts()
                if (root.selectedFp !== "" && root.current === null) { root.selectedFp = ""; root.detail = null }
            } else if (!root.loaded) {
                root.loadFailed = true
                root.loadError = root.errorText(code, message)
            }
        })
    }
    function select(fp: string) {
        root.selectedFp = fp
        root.detail = null
        root.detailError = ""
        root.loadDetail(fp)
    }
    function loadDetail(fp: string) {
        root.call("GET", "/admin/reports/errors/" + encodeURIComponent(fp), undefined, (ok, data, code, message) => {
            if (root.selectedFp !== fp) return
            if (ok && data && data.fingerprint !== undefined) root.detail = data
            else root.detailError = root.errorText(code, message)
        })
    }
    onVisibleChanged: if (visible) root.reload()

    // ------------------------------------------------------------ actions
    function patch(fp: string, body: var, okText: string, after: var) {
        root.actionBusy = true
        root.call("PATCH", "/admin/reports/errors/" + encodeURIComponent(fp), body, (ok, data, code, message) => {
            root.actionBusy = false
            if (ok && data && data.fingerprint !== undefined) {
                if (root.selectedFp === fp) root.detail = data
                if (okText !== "") root.say(okText, "done")
                if (after) after()
                root.reload()
            } else {
                root.say(root.errorText(code, message), "error")
            }
        })
    }
    function markSeen() {
        if (root.detail === null || root.actionBusy) return
        root.patch(root.detail.fingerprint, { seen: true }, qsTr("%1 is marked as seen.").arg(root.detail.fingerprint), null)
    }
    function storeIssue(fp: string, url: string, okText: string) {
        root.patch(fp, { issue_url: url }, okText, null)
    }
    function reportOnGithub() {
        if (root.detail === null || root.actionBusy || root.lookupBusy || root.issuesBase === "") return
        root.lookupBusy = true
        root.errors.lookupIssue(root.detail.fingerprint, root.issuesBase)
    }
    Connections {
        target: root.errors
        function onIssueLookupFinished(fingerprint, ok, found, number, url) {
            if (!root.lookupBusy || root.detail === null || root.detail.fingerprint !== fingerprint) return
            root.lookupBusy = false
            if (ok && found) {
                root.storeIssue(fingerprint, url, qsTr("This problem already has issue #%1. It is linked to the group.").arg(number))
            } else {
                previewDialog.open()
            }
        }
    }
    function openPreviewed() {
        const d = root.detail
        const versions = d.versions ?? []
        const url = root.errors.newIssueUrl(root.issuesBase, d.public_title ?? d.title, d.fingerprint, versions.length > 0 ? versions[0] : "", d.public_summary ?? "")
        if (url !== "" && root.errors.openInBrowser(url)) {
            previewDialog.close()
            root.say(qsTr("GitHub opened in your browser. Submit the issue there, then paste its address under Link issue."), "done")
        } else {
            previewDialog.error = qsTr("The address could not be opened.")
        }
    }

    // ------------------------------------------------------------ page
    ColumnLayout {
        id: col
        width: root.width
        spacing: Theme.space2
        UzStateBlock {
            objectName: "errorsLoading"
            Layout.fillWidth: true
            visible: !root.loaded && !root.loadFailed
            kind: "loading"
            title: qsTr("Loading errors")
        }
        UzStateBlock {
            objectName: "errorsError"
            Layout.fillWidth: true
            visible: root.loadFailed
            kind: "error"
            title: qsTr("Can't read the errors")
            text: root.loadError
            action: qsTr("Try again")
            onActionClicked: { root.loadFailed = false; root.reload() }
        }
        UzStateBlock {
            objectName: "errorsEmpty"
            Layout.fillWidth: true
            visible: root.loaded && root.groups.length === 0
            kind: "empty"
            title: qsTr("No errors")
            text: qsTr("When an app of this server reports an error, such as a crash, a core that does not load or a plugin that stops, it shows up here, grouped by problem. Apps send errors only when their person turned that on.")
        }

        UzTable {
            id: table
            objectPrefix: "errorsTable"
            Layout.fillWidth: true
            visible: root.loaded && root.groups.length > 0
            columns: [
                { key: "isNew", title: "", width: 20, custom: true },
                { key: "title", title: qsTr("Problem"), fill: 3, custom: true },
                { key: "component", title: qsTr("Component"), fill: 1.4, mono: true },
                { key: "count", title: qsTr("Events"), width: 64, mono: true, align: "right" },
                { key: "devices", title: qsTr("Devices"), width: 72, mono: true, align: "right" },
                { key: "last", title: qsTr("Last seen"), width: 100, mono: true },
                { key: "version", title: qsTr("Version"), width: 110, mono: true }
            ]
            rows: root.rows
            currentIndex: root.selectedIndex
            onPicked: (i, row) => root.select(row.fp)
            cellDelegate: Component {
                Item {
                    id: cell
                    property var row: null
                    property var column: null
                    implicitHeight: cell.column && cell.column.key === "isNew" ? 18 : col.implicitHeight
                    StatusDot {
                        objectName: "errorNew_" + (cell.row ? cell.row.fp : "")
                        visible: cell.column !== null && cell.column.key === "isNew" && cell.row !== null && cell.row.isNew
                        anchors.verticalCenter: parent.verticalCenter
                        tone: "warn"
                    }
                    Column {
                        id: col
                        visible: cell.column !== null && cell.column.key === "title"
                        width: parent.width
                        Text {
                            objectName: "errorTitle_" + (cell.row ? cell.row.fp : "")
                            width: parent.width
                            elide: Text.ElideRight
                            text: cell.row ? cell.row.title : ""
                            textFormat: Text.PlainText
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.body.size
                            font.weight: cell.row && cell.row.isNew ? Font.Medium : Font.Normal
                            color: Theme.ink
                        }
                        Text {
                            visible: cell.row !== null && cell.row.issue !== ""
                            width: parent.width
                            elide: Text.ElideRight
                            text: cell.row && cell.row.issue !== "" ? qsTr("Issue #%1").arg(root.issueNumber(cell.row.issue)) : ""
                            textFormat: Text.PlainText
                            font.family: Theme.mono
                            font.pixelSize: Theme.roles.small.size
                            color: Theme.muted
                        }
                    }
                }
            }

            ColumnLayout {   // details of the selected group
                objectName: "errorDetails"
                width: parent ? parent.width : 0
                visible: root.current !== null
                spacing: Theme.space2
                Item { Layout.preferredHeight: Theme.space2 }
                UzSectionLabel {
                    Layout.fillWidth: true; Layout.leftMargin: Theme.space15; Layout.rightMargin: Theme.space15
                    text: root.current !== null ? root.current.fingerprint : ""
                }
                UzStatusIcon {
                    objectName: "errorDetailError"
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
                        Text { text: qsTr("Component"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                        Text {
                            objectName: "errorComponent"
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            text: root.detail !== null ? root.detail.kind + " · " + root.detail.component : ""
                            font.family: Theme.mono; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                        }
                        Text { text: qsTr("Seen"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                        Text {
                            objectName: "errorSeen"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: root.detail === null ? "" : qsTr("%1 on %2 · first %3 · last %4").arg(Theme.plural(root.detail.count, qsTr("event"), qsTr("events")))
                                      .arg(Theme.plural(root.detail.devices, qsTr("device"), qsTr("devices"))).arg(root.whenText(root.detail.first_seen)).arg(root.whenText(root.detail.last_seen))
                            font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                        }
                        Text { text: qsTr("Versions"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted }
                        Text {
                            objectName: "errorVersions"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: root.detail !== null ? (root.detail.versions ?? []).join(", ") : ""
                            font.family: Theme.mono; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                        }
                        Text {
                            visible: root.issueUrl !== ""
                            text: qsTr("Issue"); font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.muted
                        }
                        Text {
                            objectName: "errorIssue"
                            visible: root.issueUrl !== ""
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            textFormat: Text.PlainText
                            text: root.issueUrl
                            font.family: Theme.mono; font.pixelSize: Theme.roles.body.size; color: Theme.ink
                        }
                    }
                    UzSectionLabel {
                        Layout.fillWidth: true
                        text: root.detail !== null ? qsTr("Newest events · %1").arg((root.detail.events ?? []).length) : ""
                    }
                    Repeater {
                        model: root.detail !== null ? (root.detail.events ?? []) : []
                        delegate: ColumnLayout {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                objectName: "errorEventHead_" + index
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: [root.whenText(modelData.occurred_at), modelData.app_version, modelData.os, modelData.device_name, modelData.user_name]
                                      .filter(t => t !== undefined && t !== "").join(" · ")
                                font.family: Theme.mono; font.pixelSize: Theme.roles.meta11.size; color: Theme.muted
                            }
                            Text {
                                objectName: "errorEventMessage_" + index
                                Layout.fillWidth: true
                                visible: (modelData.message ?? "") !== ""
                                wrapMode: Text.Wrap
                                maximumLineCount: 4
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: modelData.message ?? ""
                                font.family: Theme.mono; font.pixelSize: Theme.roles.small.size; color: Theme.ink2
                            }
                        }
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: Theme.space1
                        UzButton {
                            objectName: "errorOpenIssue"
                            kind: "primary"
                            visible: root.issueUrl !== ""
                            text: qsTr("Open issue #%1").arg(root.issueNumber(root.issueUrl))
                            onClicked: if (!root.errors.openInBrowser(root.issueUrl)) root.say(qsTr("The address could not be opened."), "error")
                        }
                        UzButton {
                            objectName: "errorReportGithub"
                            kind: "primary"
                            visible: root.issueUrl === "" && root.issuesBase !== ""
                            text: qsTr("Report on GitHub")
                            busy: root.lookupBusy
                            enabled: !root.actionBusy
                            onClicked: root.reportOnGithub()
                        }
                        UzButton {
                            objectName: "errorLinkIssue"
                            text: qsTr("Link issue…")
                            enabled: !root.actionBusy
                            onClicked: linkDialog.openFor(root.issueUrl)
                        }
                        UzButton {
                            objectName: "errorMarkSeen"
                            visible: root.detail !== null && root.detail.seen === false
                            text: qsTr("Mark as seen")
                            busy: root.actionBusy
                            onClicked: root.markSeen()
                        }
                        UzButton {
                            objectName: "errorDelete"
                            kind: "danger"
                            text: qsTr("Delete…")
                            enabled: !root.actionBusy
                            onClicked: deleteDialog.openDelete()
                        }
                    }
                }
                Item { Layout.preferredHeight: Theme.space2 }
            }
        }
    }

    // Preview of the exact public text before the browser opens (ADR 0035 D3).
    UzDialog {
        id: previewDialog
        parent: root.dialogHost
        objectName: "errorsPreviewDialog"
        size: "lg"
        label: qsTr("Reports")
        title: qsTr("Report on GitHub")
        text: qsTr("This is the text that goes into the public issue. Host names, addresses, people and game titles are already removed. Your browser opens GitHub with it; you check it there and submit it with your own account. No logs are added.")
        actions: [
            UzButton { objectName: "errorsPreviewCancel"; text: qsTr("Cancel"); onClicked: previewDialog.close() },
            UzButton { objectName: "errorsPreviewOpen"; kind: "primary"; text: qsTr("Open GitHub"); onClicked: root.openPreviewed() }
        ]
        UzMono { text: qsTr("Title") }
        Text {
            objectName: "errorsPreviewTitle"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: root.detail !== null ? (root.detail.public_title ?? root.detail.title) : ""
            font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink
        }
        UzMono { text: qsTr("Description") }
        Text {
            objectName: "errorsPreviewSummary"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: root.detail !== null ? (root.detail.public_summary ?? "") : ""
            font.family: Theme.mono; font.pixelSize: Theme.roles.small.size; color: Theme.ink2
        }
    }

    // Link an issue by hand (paste the address of the issue after filing it).
    UzDialog {
        id: linkDialog
        parent: root.dialogHost
        objectName: "errorsLinkDialog"
        readonly property bool valid: root.errors.validIssueUrl(linkField.text.trim())
        function openFor(current: string) {
            linkDialog.error = ""
            linkField.text = current
            linkDialog.open()
        }
        label: qsTr("Reports")
        title: qsTr("Link issue")
        text: qsTr("Paste the address of the GitHub issue, for example https://github.com/phabioo/uzume/issues/12. Leave it empty to remove the link.")
        actions: [
            UzButton { objectName: "errorsLinkCancel"; text: qsTr("Cancel"); enabled: !linkDialog.busy; onClicked: linkDialog.close() },
            UzButton {
                objectName: "errorsLinkSave"
                kind: "primary"
                text: qsTr("Save")
                busy: linkDialog.busy
                enabled: linkField.text.trim() === "" || linkDialog.valid
                onClicked: {
                    const fp = root.detail.fingerprint
                    const url = linkField.text.trim()
                    linkDialog.busy = true
                    root.call("PATCH", "/admin/reports/errors/" + encodeURIComponent(fp), { issue_url: url === "" ? null : url }, (ok, data, code, message) => {
                        linkDialog.busy = false
                        if (ok && data && data.fingerprint !== undefined) {
                            if (root.selectedFp === fp) root.detail = data
                            linkDialog.close()
                            root.say(url === "" ? qsTr("The link was removed.") : qsTr("The issue is linked."), "done")
                            root.reload()
                        } else {
                            linkDialog.error = root.errorText(code, message)
                        }
                    })
                }
            }
        ]
        UzField {
            id: linkField
            objectName: "errorsLinkField"
            Layout.fillWidth: true
            mono: true
            fill: Theme.ground
            maximumLength: 200
            placeholderText: "https://github.com/…/issues/…"
            invalid: text.trim() !== "" && !linkDialog.valid
        }
    }

    UzDialog {
        id: deleteDialog
        parent: root.dialogHost
        objectName: "errorsDeleteDialog"
        function openDelete() { deleteDialog.error = ""; deleteDialog.busy = false; deleteDialog.open() }
        label: qsTr("Reports")
        title: qsTr("Delete this error group?")
        text: qsTr("The group and its events are deleted for good. If the problem comes back, a new group starts.")
        actions: [
            UzButton { objectName: "errorsDeleteCancel"; text: qsTr("Cancel"); enabled: !deleteDialog.busy; onClicked: deleteDialog.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "errorsDeleteConfirm"
                kind: "danger"
                text: qsTr("Delete")
                busy: deleteDialog.busy
                onClicked: {
                    const fp = root.detail.fingerprint
                    deleteDialog.error = ""
                    deleteDialog.busy = true
                    root.call("DELETE", "/admin/reports/errors/" + encodeURIComponent(fp), undefined, (ok, data, code, message) => {
                        deleteDialog.busy = false
                        if (ok) {
                            deleteDialog.close()
                            if (root.selectedFp === fp) { root.selectedFp = ""; root.detail = null }
                            root.say(qsTr("%1 was deleted.").arg(fp), "done")
                            root.reload()
                        } else {
                            deleteDialog.error = root.errorText(code, message)
                        }
                    })
                }
            }
        ]
    }
}
