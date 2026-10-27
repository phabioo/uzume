import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server > Tools (randomizer.md 10h-1, 10h-2 a to f) from GET /admin/tools: the programs the server offers to apps, like cores. Today one
// tool, the randomizer engine: a facts list (engine, version and when the source was last checked, size per PC, platforms, license
// with a link to its source), the "Offer to apps" toggle (PUT /admin/tools/{id}), an update line with "Update to x" (installs the newest
// release the source listed, POST .../versions/{v}/install), "Check now" (POST .../sync) and the table "Versions kept" with Remove...
// (DELETE .../versions/{v}); the server refuses to remove a version that a variant or a run still pins (409 tool_in_use with the counts)
// and the dialog then says so.
// States: a no internet (source error or off: the offline import is a server command, shown as a hint, never an upload from the app),
// b install error message (the file or the download was refused), c Offer to apps off, d updating, e no tools yet (Get the engine),
// f remove with the counts.
// objectNames: serverToolsPage, toolsIntro, toolsLoading, toolsError, toolsNote, toolsEmpty, toolGetEngine, toolCard, toolFactEngine,
// toolFactVersion, toolFactSize, toolFactPlatforms, toolFactLicense, toolSourceLink, toolOfferRow (toggle toolOffer), toolOfferNote,
// toolUpdateLine, toolUpdate, toolCheckNow, toolSourceProblem, toolImportHint, toolUpdating, toolUpdatingStays, toolVersionsTable
// (rows toolVersionsTableRow_<n>, Remove button toolRemove_<version>), toolRemoveDialog, toolRemoveCancel, toolRemoveConfirm.
AdminPage {
    id: root
    objectName: "serverToolsPage"
    readonly property AdminController admin: root.player.admin

    property string phase: "loading"          // loading | ready | error
    property string loadError: ""
    property var tools: []                    // AdminTool
    property string note: ""
    property string noteKind: "done"
    property var busy: ({})
    property var calls: ({})
    property string installing: ""            // version being installed ("" = none)
    property string removeVersion: ""         // version asked about in the remove dialog
    property string removeError: ""

    // The engine: the tool the Randomizer plugin uses.
    readonly property var tool: root.tools.find(t => t.id === "randomizer") ?? (root.tools.length > 0 ? root.tools[0] : null)
    readonly property var versions: root.tool ? (root.tool.versions ?? []) : []
    readonly property var installedVersions: root.versions.filter(v => v.installed === true)
    readonly property var newest: root.installedVersions.length > 0 ? root.installedVersions[0] : null
    readonly property var source: root.tool ? (root.tool.source ?? ({})) : ({})
    readonly property bool sourceProblem: root.tool !== null && (root.source.state === "error" || root.source.state === "off")
    readonly property bool noTools: root.phase === "ready" && (root.tool === null || root.installedVersions.length === 0)
    readonly property string latest: root.tool && root.tool.update_available === true ? (root.tool.latest_version ?? "") : ""
    readonly property var removeRow: root.versions.find(v => v.version === root.removeVersion) ?? null

    title: qsTr("Tools")
    label: qsTr("Server")
    area: qsTr("Tools")
    helpItems: [
        { title: qsTr("Versions"), text: qsTr("Updating never changes a running run or an existing recipe. %1 keeps every version still in use.").arg(root.serverName) }
    ]

    // ------------------------------------------------------------ text helpers
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
    function clockText(iso: string): string {
        const d = new Date(iso)
        if (isNaN(d.getTime())) return ""
        const now = new Date()
        const same = d.getFullYear() === now.getFullYear() && d.getMonth() === now.getMonth() && d.getDate() === now.getDate()
        return same ? qsTr("today %1").arg(Qt.formatTime(d, "HH:mm")) : Qt.formatDateTime(d, "d MMM HH:mm")
    }
    function bytesText(n: real): string {
        const gib = Math.pow(1024, 3), mib = Math.pow(1024, 2)
        const trim = (v) => { const t = v.toFixed(1); return t.endsWith(".0") ? t.slice(0, -2) : t }
        if (n >= gib) return qsTr("%1 GB").arg(trim(n / gib))
        if (n >= mib) return qsTr("%1 MB").arg(Math.round(n / mib))
        return qsTr("%1 KB").arg(Math.max(1, Math.round(n / 1024)))
    }
    function sizeOf(v: var): real {
        let m = 0
        for (const p of (v && v.platforms ? v.platforms : [])) m = Math.max(m, p.size ?? 0)
        return m
    }
    function platformName(id: string): string {
        if (id.indexOf("windows") === 0) return qsTr("Windows")
        if (id.indexOf("linux") === 0) return qsTr("Linux")
        if (id.indexOf("macos") === 0 || id.indexOf("darwin") === 0) return qsTr("macOS")
        return id
    }
    function platformsText(v: var): string {
        const seen = []
        for (const p of (v && v.platforms ? v.platforms : [])) {
            const n = root.platformName(p.platform ?? "")
            if (n !== "" && seen.indexOf(n) < 0) seen.push(n)
        }
        return seen.length > 0 ? seen.join(" · ") : "—"
    }
    function usedCount(v: var): int {
        const u = v && v.used_by ? v.used_by : ({})
        return (u.variants ?? 0) + (u.runs ?? 0)
    }
    function usageText(u: var): string {
        const parts = []
        const variants = u ? (u.variants ?? 0) : 0
        const runs = u ? (u.runs ?? 0) : 0
        if (variants > 0) parts.push(variants === 1 ? qsTr("1 variant recipe") : qsTr("%1 variant recipes").arg(variants))
        if (runs > 0) parts.push(runs === 1 ? qsTr("1 run") : qsTr("%1 runs").arg(runs))
        return parts.length > 0 ? parts.join(" · ") : qsTr("Not used")
    }
    function originText(v: var): string {
        const ps = v && v.platforms ? v.platforms : []
        if (ps.length > 0 && ps.every(p => p.origin === "import")) return qsTr("Imported")
        if (ps.length > 0) return qsTr("Downloaded")
        return ""
    }
    readonly property var versionRows: root.installedVersions.map((v, i) => ({
        version: v.version, used: root.usageText(v.used_by), size: root.bytesText(root.sizeOf(v)),
        offered: i === 0 && root.tool !== null && root.tool.offered === true, origin: root.originText(v), inUse: root.usedCount(v) > 0 }))

    // One line of the facts list: Mono label (96 wide) at the left, the value beside it, a hairline under it.
    component Fact: Item {
        id: fact
        property string label: ""
        property string value: ""
        property string valueName: ""
        property bool mono: false
        property bool inlineExtra: false   // the extra items follow the value text instead of sitting at the right edge
        default property alias extra: extraRow.data
        Layout.fillWidth: true
        implicitHeight: Math.max(32, Math.max(factValue.implicitHeight, extraRow.implicitHeight) + Theme.space1)
        Text {
            x: 0
            width: 96
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            text: fact.label
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.tableHeader.size
            font.weight: Font.Medium
            font.letterSpacing: Theme.roles.tableHeader.spacing
            font.capitalization: Font.AllUppercase
            color: Theme.dim
        }
        Text {
            id: factValue
            objectName: fact.valueName
            x: 96 + Theme.space15
            width: Math.max(0, fact.width - x - extraRow.width - (fact.inlineExtra ? Theme.space1 : 0))
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.Wrap
            text: fact.value
            textFormat: Text.PlainText
            font.family: fact.mono ? Theme.mono : Theme.sans
            font.pixelSize: fact.mono ? Theme.roles.small.size : Theme.roles.input.size
            color: Theme.ink2
        }
        Row {
            id: extraRow
            x: fact.inlineExtra ? factValue.x + factValue.contentWidth : fact.width - width
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space1
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
    }

    // ------------------------------------------------------------ data
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
    function applyTool(t: var) {
        if (!t || t.id === undefined) return
        const list = root.tools.slice()
        const i = list.findIndex(x => x.id === t.id)
        if (i >= 0) list[i] = t
        else list.push(t)
        root.tools = list
    }

    function load() {
        root.call("GET", "/admin/tools", undefined, function (ok, data, code, message) {
            if (ok && data && data.tools !== undefined) {
                root.tools = data.tools
                root.phase = "ready"
            } else if (root.phase !== "ready") {
                root.phase = "error"
                root.loadError = message
            } else {
                root.say(root.failText(qsTr("Could not read the tools"), message), "error")
            }
        })
    }
    function setOffered(on: bool) {
        if (root.tool === null) return
        root.say("", "done")
        root.setBusy("offer", true)
        root.call("PUT", "/admin/tools/" + root.tool.id, { offered: on }, function (ok, data, code, message) {
            root.setBusy("offer", false)
            if (ok && data) {
                root.applyTool(data)
            } else {
                root.say(root.failText(qsTr("Could not change Offer to apps"), message), "error")
                root.load()
            }
        })
    }
    function checkNow() {
        if (root.tool === null) return
        root.say("", "done")
        root.setBusy("sync", true)
        root.call("POST", "/admin/tools/" + root.tool.id + "/sync", undefined, function (ok, data, code, message) {
            root.setBusy("sync", false)
            if (ok && data) {
                root.applyTool(data)
                const err = data.source ? (data.source.last_error ?? "") : ""
                if (err === "") root.say(data.update_available === true ? "" : qsTr("The engine is up to date."), "done")
            } else {
                root.say(root.failText(qsTr("Could not check for updates"), message), "error")
            }
        })
    }
    function install(version: string) {
        if (root.tool === null || version === "" || root.installing !== "") return
        root.say("", "done")
        root.installing = version
        root.call("POST", "/admin/tools/" + root.tool.id + "/versions/" + encodeURIComponent(version) + "/install", undefined, function (ok, data, code, message) {
            root.installing = ""
            if (ok && data) {
                root.applyTool(data)
            } else {
                // The download was refused or the file is not the engine (10h-2b): the server's reason is the message.
                root.say(code === "tool_hash_mismatch" ? qsTr("Could not get %1: this file isn't the release %2 pinned.").arg(version).arg(root.serverName)
                                                      : root.failText(qsTr("Could not get %1").arg(version), message), "error")
                root.load()
            }
        })
    }
    // "Get the engine" (10h-2e): the newest release the source lists; a source never read is read first.
    function getEngine() {
        if (root.tool === null) return
        if ((root.tool.latest_version ?? "") !== "") {
            root.install(root.tool.latest_version)
            return
        }
        root.say("", "done")
        root.setBusy("sync", true)
        root.call("POST", "/admin/tools/" + root.tool.id + "/sync", undefined, function (ok, data, code, message) {
            root.setBusy("sync", false)
            if (ok && data) {
                root.applyTool(data)
                if ((data.latest_version ?? "") !== "") root.install(data.latest_version)
                else root.say(qsTr("%1 found no release to download.").arg(root.serverName), "warning")
            } else {
                root.say(root.failText(qsTr("Could not check for updates"), message), "error")
            }
        })
    }
    function askRemove(version: string) {
        root.removeVersion = version
        root.removeError = ""
        removeDialog.open()
    }
    function removeConfirmed() {
        if (root.tool === null || root.removeVersion === "") return
        const version = root.removeVersion
        root.setBusy("remove", true)
        root.removeError = ""
        root.call("DELETE", "/admin/tools/" + root.tool.id + "/versions/" + encodeURIComponent(version), undefined, function (ok, data, code, message) {
            root.setBusy("remove", false)
            if (ok) {
                removeDialog.shown = false
                root.say(qsTr("Removed %1.").arg(version), "done")
                root.load()
            } else if (code === "tool_in_use") {
                // 409: a variant or a run still pins it; the counts come from the answer.
                root.removeError = qsTr("%1 keeps %2 while %3 use it.").arg(root.serverName).arg(version).arg(root.usageText(data ? data.used_by : null))
                root.load()
            } else {
                root.removeError = root.failText(qsTr("Could not remove %1").arg(version), message)
            }
        })
    }

    Component.onCompleted: root.load()
    onVisibleChanged: if (visible && root.phase !== "loading") root.load()

    notice: [
        UzStatusIcon {
            objectName: "toolsNote"
            visible: root.note !== ""
            kind: root.noteKind
            small: true
            text: root.note
            maxWidth: 640
        }
    ]

    // ------------------------------------------------------------ intro and states
    Text {
        objectName: "toolsIntro"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: qsTr("Programs %1 offers to apps, like cores. Apps run them on their own PC; game files never come here.").arg(root.serverName)
        font.family: Theme.sans
        font.pixelSize: Theme.roles.input.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
        color: Theme.ink2
    }
    UzStateBlock {
        objectName: "toolsLoading"
        Layout.fillWidth: true
        visible: root.phase === "loading"
        kind: "loading"
        title: qsTr("Loading tools")
    }
    UzStateBlock {
        objectName: "toolsError"
        Layout.fillWidth: true
        visible: root.phase === "error"
        kind: "error"
        title: qsTr("Can't load the tools")
        text: root.loadError !== "" ? root.loadError : qsTr("The server did not answer.")
        action: qsTr("Try again")
        onActionClicked: { root.phase = "loading"; root.load() }
    }
    // 10h-2e: nothing installed yet.
    ColumnLayout {
        objectName: "toolsEmpty"
        Layout.fillWidth: true
        visible: root.noTools && root.installing === ""
        spacing: Theme.space2
        UzStateBlock {
            objectName: "toolsEmptyState"
            Layout.fillWidth: true
            kind: "empty"
            title: qsTr("No tools yet")
            text: qsTr("Tools are programs %1 offers to apps, like cores. The Randomizer plugin needs its engine.").arg(root.serverName)
            action: root.tool !== null && root.source.state !== "off" ? qsTr("Get the engine") : ""
            onActionClicked: root.getEngine()
        }
        ColumnLayout {
            visible: root.sourceProblem
            Layout.fillWidth: true
            spacing: Theme.space1
            UzStatusIcon {
                objectName: "toolSourceProblemEmpty"
                kind: "warning"
                small: true
                text: root.source.state === "off" ? qsTr("%1 has no place to get tools from.").arg(root.serverName) : qsTr("%1 can't reach the internet.").arg(root.serverName)
                maxWidth: 640
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("Import the engine from a file you downloaded on another PC, on the server:")
                font.family: Theme.sans
                font.pixelSize: Theme.roles.input.size
                color: Theme.ink2
            }
            UzMono { objectName: "toolImportHintEmpty"; Layout.fillWidth: true; text: "uzume-server import-tools <engine.zip>"; color: Theme.ink2; wrapMode: Text.Wrap }
        }
    }

    // 10h-2d: a download is running.
    ColumnLayout {
        objectName: "toolUpdating"
        Layout.fillWidth: true
        visible: root.installing !== ""
        spacing: Theme.space1
        UzSectionLabel {
            Layout.fillWidth: true
            text: root.newest ? qsTr("Updating the engine to %1").arg(root.installing) : qsTr("Getting the engine %1").arg(root.installing)
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space15
            UzSpinner { size: 16 }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("Downloading %1 for every platform. This can take a few minutes.").arg(root.installing)
                font.family: Theme.sans
                font.pixelSize: Theme.roles.input.size
                color: Theme.ink2
            }
        }
        Text {
            objectName: "toolUpdatingStays"
            Layout.fillWidth: true
            visible: root.newest !== null && root.usedCount(root.newest) > 0
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: root.newest ? qsTr("%1 stays: runs and recipes pin it.").arg(root.newest.version) : ""
            font.family: Theme.mono
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
    }

    // ------------------------------------------------------------ the engine
    ColumnLayout {
        objectName: "toolCard"
        Layout.fillWidth: true
        visible: root.phase === "ready" && root.tool !== null && root.installedVersions.length > 0
        spacing: Theme.space1

        UzSectionLabel {
            Layout.fillWidth: true
            text: root.tool ? root.tool.name : ""
            meta: qsTr("Used by the Randomizer plugin")
        }
        Fact {
            label: qsTr("Engine")
            valueName: "toolFactEngine"
            mono: true
            value: root.tool && root.tool.id === "randomizer" ? qsTr("FVX · OPEN-SOURCE RANDOMIZER") : (root.tool ? root.tool.name : "")
        }
        Fact {
            label: qsTr("Version")
            valueName: "toolFactVersion"
            value: !root.newest ? "—" : (root.source.last_check ? qsTr("%1 · checked %2").arg(root.newest.version).arg(root.clockText(root.source.last_check))
                                                               : root.newest.version)
            UzButton {
                objectName: "toolCheckNow"
                size: "sm"
                kind: "ghost"
                visible: root.source.state !== "off"
                busy: root.busy["sync"] === true
                text: qsTr("Check now")
                onClicked: root.checkNow()
            }
        }
        Fact {
            label: qsTr("Size")
            valueName: "toolFactSize"
            value: qsTr("%1 per PC · downloaded once").arg(root.bytesText(root.sizeOf(root.newest)))
        }
        Fact {
            label: qsTr("Platforms")
            valueName: "toolFactPlatforms"
            value: root.platformsText(root.newest)
        }
        Fact {
            label: qsTr("License")
            inlineExtra: true
            valueName: "toolFactLicense"
            value: root.tool ? qsTr("%1 ·").arg(root.tool.license) : ""
            UzButton {
                objectName: "toolSourceLink"
                kind: "link"
                visible: root.tool !== null && (root.tool.source_url ?? "") !== ""
                text: qsTr("source and license")
                onClicked: Qt.openUrlExternally(root.tool.source_url)
            }
        }

        UzSettingsRow {
            objectName: "toolOfferRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "offer"
            label: qsTr("Offer to apps")
            descriptionName: "toolOfferNote"
            description: root.tool && root.tool.offered === true ? qsTr("Apps download the engine from %1 when someone builds a variant.").arg(root.serverName)
                                                                : qsTr("Apps can't build new variants. Existing variants keep playing.")
            ctrl: "toggle"
            controlName: "toolOffer"
            controlEnabled: root.busy["offer"] !== true
            checked: root.tool !== null && root.tool.offered === true
            onToggled: value => root.setOffered(value)
        }

        // Update line: 4 padding, the button at the right.
        RowLayout {
            objectName: "toolUpdateLine"
            Layout.fillWidth: true
            Layout.topMargin: Theme.space05
            visible: root.latest !== "" && root.installing === ""
            spacing: Theme.space2
            UzStatusIcon {
                Layout.fillWidth: true
                kind: "warning"
                small: true
                text: qsTr("Version %1 is available").arg(root.latest)
            }
            UzButton {
                objectName: "toolUpdate"
                size: "sm"
                text: qsTr("Update to %1").arg(root.latest)
                onClicked: root.install(root.latest)
            }
        }

        // 10h-2a: the source cannot be reached (or is off). The offline import is a command on the server.
        ColumnLayout {
            visible: root.sourceProblem
            Layout.fillWidth: true
            Layout.topMargin: Theme.space1
            spacing: Theme.space1
            UzStatusIcon {
                objectName: "toolSourceProblem"
                kind: "warning"
                small: true
                text: root.source.state === "off" ? qsTr("%1 has no place to check for updates.").arg(root.serverName) : qsTr("%1 can't reach the internet.").arg(root.serverName)
                maxWidth: 640
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: qsTr("Import the engine from a file you downloaded on another PC, on the server:")
                font.family: Theme.sans
                font.pixelSize: Theme.roles.input.size
                color: Theme.ink2
            }
            UzMono { objectName: "toolImportHint"; Layout.fillWidth: true; text: "uzume-server import-tools <engine.zip>"; color: Theme.ink2; wrapMode: Text.Wrap }
        }
    }

    // ------------------------------------------------------------ versions kept
    ColumnLayout {
        objectName: "toolVersions"
        Layout.fillWidth: true
        visible: root.phase === "ready" && root.installedVersions.length > 0
        spacing: Theme.space1
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Versions kept"); meta: qsTr("Runs and recipes pin a version") }
        UzTable {
            objectPrefix: "toolVersionsTable"
            Layout.fillWidth: true
            selectable: false
            columns: [
                { key: "version", title: qsTr("Version"), fill: 1, custom: true },
                { key: "used", title: qsTr("Used by"), fill: 1.4 },
                { key: "size", title: qsTr("Size"), width: 64, mono: true },
                { key: "action", title: "", width: 104, custom: true, align: "right" }
            ]
            rows: root.versionRows
            cellDelegate: Item {
                id: vc
                property var row: null
                property var column: null
                implicitHeight: Theme.row32
                height: implicitHeight
                Row {
                    visible: vc.column !== null && vc.column.key === "version"
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space1
                    UzMono { anchors.verticalCenter: parent.verticalCenter; color: Theme.ink; text: vc.row ? vc.row.version : "" }
                    UzTag {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: vc.row !== null && vc.row.offered
                        small: true
                        tone: "ok"
                        text: qsTr("Offered")
                    }
                    UzMono { anchors.verticalCenter: parent.verticalCenter; visible: vc.row !== null && vc.row.origin !== ""; text: vc.row ? vc.row.origin : "" }
                }
                UzButton {
                    objectName: vc.row ? "toolRemove_" + vc.row.version : ""
                    visible: vc.column !== null && vc.column.key === "action"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    size: "sm"
                    kind: "ghost"
                    text: qsTr("Remove…")
                    onClicked: root.askRemove(vc.row.version)
                }
            }
        }
    }

    // ------------------------------------------------------------ remove (10h-2f)
    UzDialog {
        id: removeDialog
        objectName: "toolRemoveDialog"
        parent: root
        label: qsTr("Tools")
        title: qsTr("Remove version %1?").arg(root.removeVersion)
        text: root.removeRow && root.usedCount(root.removeRow) > 0
              ? qsTr("%1 use it. Their owners can still play; rebuilding them needs %2 back.").arg(root.usageText(root.removeRow.used_by)).arg(root.removeVersion)
              : qsTr("Nothing uses it. Apps keep their copy.")
        error: root.removeError
        busy: root.busy["remove"] === true
        actions: [
            UzButton { objectName: "toolRemoveCancel"; text: qsTr("Cancel"); onClicked: removeDialog.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "toolRemoveConfirm"
                kind: "danger"
                text: qsTr("Remove")
                onClicked: root.removeConfirmed()
            }
        ]
    }
}
