import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Setup assistant (accounts-and-server.md 7a-app-1 and 7a-app-2): six skippable steps over the window, opened by
// `player.showSetupAssistant()` (the first start and Server › Settings › General › "Run setup again" call it; admins of a server
// with the admin pages only): 01 Systems, 02 Cores, 03 ROM folders, 04 Internet access, 05 Backups, 06 Done.
// It reads what the server has (GET /admin/systems, /admin/settings, /admin/backups/settings) and never resets anything: only a
// value the admin changed is sent (PUT /admin/settings/network/import_dir and /public_host, PUT /admin/backups/settings, a core is
// installed and made the default with POST .../install and PATCH /admin/systems/{id}); "Skip this step" and "Only home network"
// send nothing. The last step lists what is set up and what is open (the same open items show in Status).
// objectNames: setupAssistant, setupStep_<n>, setupSkipAll, setupSkip, setupNext, setupHomeOnly, setupCoreRow_<system>, setupCoreInstall_<system>,
// setupImportDir, setupPublicHost, setupRelay, setupPorts, setupBackupMode_<m>, setupBackupFolder, setupBackupKeep, setupBackupTime,
// setupError, setupSummary_<name>, setupSummarySetUp_<name>, setupCreateInvite, setupOpenLibrary, setupRetry.
FocusScope {
    id: root
    objectName: "setupAssistant"
    required property PlayerController player
    readonly property AdminController admin: root.player.admin
    readonly property bool shown: root.player.setupAssistantOpen && root.admin.available
    property int step: 0
    property string phase: "idle"      // idle | loading | ready | error
    property bool busy: false
    property string error: ""
    property var systems: []
    property var settings: ({})        // AdminSettings
    property var backup: ({})          // AdminBackupSettings
    property var skipped: ({})         // step name -> true when the admin skipped it
    property string importNote: ""
    property string internetNote: ""
    property string coreNote: ""
    property var coreBusy: ({})        // system id -> true while its core installs
    readonly property var stepNames: [qsTr("Systems"), qsTr("Cores"), qsTr("ROM folders"), qsTr("Internet access"), qsTr("Backups"), qsTr("Done")]
    readonly property var stepKeys: ["systems", "cores", "roms", "internet", "backups", "done"]
    readonly property string serverName: root.player.hubName !== "" ? root.player.hubName : qsTr("The server")

    anchors.fill: parent
    z: 100
    visible: shown
    focus: shown
    onShownChanged: if (shown) { root.forceActiveFocus(); root.start() }
    Keys.onEscapePressed: (event) => { root.player.hideSetupAssistant(); event.accepted = true }

    AdminCall { id: api; admin: root.admin }

    // ------------------------------------------------------------ data
    function netValue(key: string): string {
        const list = root.settings.network ? root.settings.network.settings : []
        for (const s of (list ?? [])) if (s.key === key) return s.value
        return ""
    }
    function netOn(key: string): bool { const v = root.netValue(key); return v === "1" || v === "true" || v === "on" }
    function start() {
        root.step = 0
        root.skipped = ({})
        root.error = ""
        root.importNote = ""
        root.internetNote = ""
        root.coreNote = ""
        root.phase = "loading"
        let pending = 3
        let bad = false
        const done = () => { pending -= 1; if (pending === 0) { root.phase = bad ? "error" : "ready"; if (!bad) root.fillFields() } }
        api.call("GET", "/admin/systems", undefined, function (ok, data) {
            if (ok && data && data.systems !== undefined) root.systems = data.systems; else bad = true
            done()
        })
        api.call("GET", "/admin/settings", undefined, function (ok, data) {
            if (ok && data && data.network !== undefined) root.settings = data; else bad = true
            done()
        })
        api.call("GET", "/admin/backups/settings", undefined, function (ok, data) {
            if (ok && data && data.mode !== undefined) root.backup = data; else bad = true
            done()
        })
    }
    function fillFields() {
        importDir.text = root.netValue("import_dir")
        publicHost.text = root.netValue("public_host")
        folderField.text = root.backup.folder ?? ""
        keepField.text = String(root.backup.keep ?? 7)
        timeField.text = root.backup.time ?? "03:00"
        root.backupMode = root.backup.mode ?? "off"
    }
    property string backupMode: "off"

    // ------------------------------------------------------------ summaries (what the server has now)
    readonly property string systemsText: root.systems.map(s => s.display_name).join(", ")
    readonly property var withoutCore: root.systems.filter(s => !s.default_core_id)
    readonly property string coresText: root.systems.length === 0 ? qsTr("No systems")
        : root.withoutCore.length === 0 ? qsTr("%1 of %2").arg(root.systems.length).arg(root.systems.length)
        : qsTr("%1 of %2 · %3 has no core").arg(root.systems.length - root.withoutCore.length).arg(root.systems.length).arg(root.withoutCore.map(s => s.display_name).join(", "))
    readonly property string importValue: root.netValue("import_dir")
    readonly property string romsText: root.importValue === "" ? qsTr("No import folder") : qsTr("%1 · %2").arg(root.importValue).arg(Theme.plural(root.player.library.totalCount, qsTr("game"), qsTr("games")))
    readonly property bool internetReady: root.netValue("public_host") !== ""
    readonly property string internetText: root.internetReady ? qsTr("Prepared for the internet") : qsTr("Home network only")
    readonly property string backupsText: {
        const b = root.backup
        if (b.mode === undefined || b.mode === "off") return qsTr("Off")
        const keep = qsTr("keep %1").arg(b.keep)
        return b.mode === "weekly" ? qsTr("Weekly at %1 · %2").arg(b.time).arg(keep) : qsTr("Daily at %1 · %2").arg(b.time).arg(keep)
    }
    function summaryRows(): var {
        return [
            { key: "systems", label: qsTr("Systems"), text: root.systemsText, ok: root.systems.length > 0, step: 0 },
            { key: "cores", label: qsTr("Cores"), text: root.coresText, ok: root.withoutCore.length === 0 && root.systems.length > 0, step: 1 },
            { key: "roms", label: qsTr("ROM folders"), text: root.romsText, ok: root.importValue !== "", step: 2 },
            { key: "internet", label: qsTr("Internet access"), text: root.internetText, ok: true, step: 3 },
            { key: "backups", label: qsTr("Backups"), text: root.backupsText, ok: root.backup.mode !== undefined && root.backup.mode !== "off", step: 4 }
        ]
    }

    // ------------------------------------------------------------ actions
    function adoptSystem(sys: var) {
        if (!sys || sys.id === undefined) return
        root.systems = root.systems.map(s => s.id === sys.id ? sys : s)
    }
    function installCore(sys: var) {
        const offer = (sys.available_cores ?? [])[0]
        if (!offer) return
        const c = Object.assign({}, root.coreBusy); c[sys.id] = true; root.coreBusy = c
        root.coreNote = ""
        const finish = (text) => { const d = Object.assign({}, root.coreBusy); delete d[sys.id]; root.coreBusy = d; root.coreNote = text }
        api.call("POST", "/admin/systems/" + sys.id + "/cores/" + offer.core_id + "/install", undefined, function (ok, data, code, message) {
            if (!ok) { finish(api.failText(qsTr("%1 didn't download").arg(offer.display_name), message)); return }
            root.adoptSystem(data)
            api.call("PATCH", "/admin/systems/" + sys.id, { default_core_id: offer.core_id }, function (ok2, data2, code2, message2) {
                if (ok2) root.adoptSystem(data2)
                finish(ok2 ? "" : api.failText(qsTr("Could not use %1").arg(offer.display_name), message2))
            })
        })
    }
    // Sends a network value only when the admin changed it; fn(ok) reports.
    function putNet(key: string, value: string, fn: var) {
        api.call("PUT", "/admin/settings/network/" + key, { value: value }, function (ok, data, code, message) {
            if (ok && data && data.settings !== undefined) {
                const s = Object.assign({}, root.settings)
                s.network = data
                root.settings = s
            }
            fn(ok, code === "confirmation_cancelled" ? qsTr("Not confirmed. Nothing was changed.") : message)
        })
    }
    function advance() {
        root.error = ""
        root.busy = false
        root.step = Math.min(5, root.step + 1)
    }
    function skip() {
        const s = Object.assign({}, root.skipped)
        s[root.stepKeys[root.step]] = true
        root.skipped = s
        root.advance()
    }
    function next() {
        if (root.busy) return
        root.error = ""
        const key = root.stepKeys[root.step]
        if (key === "roms" && importDir.text.trim() !== root.importValue) {
            root.busy = true
            root.putNet("import_dir", importDir.text.trim(), (ok, message) => {
                root.busy = false
                if (ok) root.advance(); else root.error = api.failText(qsTr("Could not save the folder"), message)
            })
        } else if (key === "internet" && publicHost.text.trim() !== root.netValue("public_host")) {
            root.busy = true
            root.putNet("public_host", publicHost.text.trim(), (ok, message) => {
                root.busy = false
                if (ok) root.advance(); else root.error = api.failText(qsTr("Could not save the address"), message)
            })
        } else if (key === "backups" && root.backupEdited()) {
            root.busy = true
            const keep = parseInt(keepField.text)
            const body = { mode: root.backupMode, time: timeField.text.trim(), keep: isNaN(keep) ? root.backup.keep : keep, folder: folderField.text.trim() }
            api.call("PUT", "/admin/backups/settings", body, function (ok, data, code, message) {
                root.busy = false
                if (ok) { if (data && data.mode !== undefined) root.backup = data; root.advance() }
                else root.error = code === "confirmation_cancelled" ? qsTr("Not confirmed. Nothing was changed.") : api.failText(qsTr("Could not save the backup schedule"), message)
            })
        } else {
            root.advance()
        }
    }
    function backupEdited(): bool {
        return root.backupMode !== (root.backup.mode ?? "off") || folderField.text.trim() !== (root.backup.folder ?? "")
               || String(root.backup.keep ?? 7) !== keepField.text.trim() || (root.backup.time ?? "03:00") !== timeField.text.trim()
    }
    function homeOnly() { root.skip() }

    // ------------------------------------------------------------ view
    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
        MouseArea { anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }
    }
    Rectangle {
        id: frame
        anchors.centerIn: parent
        width: Math.min(880, Math.max(0, root.width - 2 * Theme.space3))
        height: Math.min(560, Math.max(0, root.height - 2 * Theme.space3))
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
        clip: true
        MouseArea { anchors.fill: parent }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.space4
            spacing: Theme.space2

            RowLayout {
                Layout.fillWidth: true
                UzMono { Layout.fillWidth: true; text: qsTr("Setup assistant · %1").arg(root.serverName).toUpperCase() }
                UzButton { objectName: "setupSkipAll"; kind: "link"; text: qsTr("Skip setup"); onClicked: root.player.hideSetupAssistant() }
            }
            // stepper on hairlines
            RowLayout {
                Layout.fillWidth: true
                spacing: 0
                Repeater {
                    model: root.stepNames
                    delegate: Item {
                        id: tab
                        required property string modelData
                        required property int index
                        objectName: "setupStep_" + tab.index
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        implicitHeight: 40
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: tab.index === root.step ? 2 : 1; color: tab.index === root.step ? Theme.accent : Theme.rule2 }
                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.space1
                            anchors.verticalCenter: parent.verticalCenter
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: String(tab.index + 1).padStart(2, "0") + "  " + tab.modelData
                            font.family: Theme.mono
                            font.pixelSize: 11
                            font.weight: tab.index === root.step ? Font.Medium : Font.Normal
                            color: tab.index === root.step ? Theme.ink : (tab.index < root.step ? Theme.muted : Theme.dim)
                        }
                    }
                }
            }

            UzStateBlock {
                objectName: "setupLoading"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.phase === "loading"
                kind: "loading"
                title: qsTr("Reading the server")
            }
            UzStateBlock {
                objectName: "setupLoadError"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.phase === "error"
                kind: "error"
                title: qsTr("Could not read the server")
                text: qsTr("Setup changes nothing until you press Next. Try again or skip setup.")
                action: qsTr("Try again")
                onActionClicked: root.start()
            }

            Flickable {
                id: scroller
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.phase === "ready"
                contentWidth: width
                contentHeight: stack.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                StackLayout {
                    id: stack
                    width: scroller.width - Theme.space2
                    currentIndex: root.step

                    // ---- 01 Systems
                    ColumnLayout {
                        spacing: Theme.space15
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: 20; font.weight: Font.Medium; color: Theme.ink; text: qsTr("Systems on %1").arg(root.serverName) }
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.ink2; text: qsTr("These are the systems the server can run games for. Games are sorted into them by file type.") }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.space1
                            Repeater {
                                model: root.systems
                                delegate: UzSystemChip { required property var modelData; objectName: "setupSystem_" + modelData.id; system: modelData.id.toUpperCase() }
                            }
                        }
                    }

                    // ---- 02 Cores
                    ColumnLayout {
                        spacing: Theme.space15
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: 20; font.weight: Font.Medium; color: Theme.ink; text: qsTr("Cores") }
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.ink2; text: qsTr("A core runs the games of a system. The server downloads them from the libretro buildbot and keeps them up to date.") }
                        Repeater {
                            model: root.systems
                            delegate: RowLayout {
                                id: coreRow
                                required property var modelData
                                objectName: "setupCoreRow_" + coreRow.modelData.id
                                readonly property var offer: (coreRow.modelData.available_cores ?? [])[0]
                                readonly property var def: (coreRow.modelData.cores ?? []).filter(c => c.default)[0]
                                Layout.fillWidth: true
                                spacing: Theme.space2
                                Text { Layout.preferredWidth: 160; elide: Text.ElideRight; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.ink; text: coreRow.modelData.display_name }
                                UzStatusIcon {
                                    Layout.fillWidth: true
                                    small: true
                                    kind: coreRow.modelData.default_core_id ? "done" : "warning"
                                    text: coreRow.def ? qsTr("%1 %2").arg(coreRow.def.display_name).arg(coreRow.def.version)
                                                      : coreRow.modelData.default_core_id ? coreRow.modelData.default_core_id : qsTr("No core")
                                }
                                UzButton {
                                    objectName: "setupCoreInstall_" + coreRow.modelData.id
                                    size: "sm"
                                    visible: !coreRow.modelData.default_core_id && coreRow.offer !== undefined
                                    busy: root.coreBusy[coreRow.modelData.id] === true
                                    text: coreRow.offer ? qsTr("Install %1").arg(coreRow.offer.display_name) : ""
                                    onClicked: root.installCore(coreRow.modelData)
                                }
                            }
                        }
                        UzStatusIcon { objectName: "setupCoreNote"; Layout.fillWidth: true; visible: root.coreNote !== ""; kind: "error"; small: true; text: root.coreNote; maxWidth: 640 }
                    }

                    // ---- 03 ROM folders
                    ColumnLayout {
                        spacing: Theme.space15
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: 20; font.weight: Font.Medium; color: Theme.ink; text: qsTr("Where are your games?") }
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.ink2; text: qsTr("The server reads game files from this folder on its own disk. Files are only read, never changed. You can also add games from the Library.") }
                        UzMono { text: qsTr("Import folder").toUpperCase() }
                        UzField { id: importDir; objectName: "setupImportDir"; Layout.fillWidth: true; mono: true; fill: Theme.ground; Accessible.name: qsTr("Import folder"); onAccepted: root.next() }
                        UzMono { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("%1 in the Library now").arg(Theme.plural(root.player.library.totalCount, qsTr("game"), qsTr("games"))) }
                    }

                    // ---- 04 Internet access
                    ColumnLayout {
                        spacing: Theme.space15
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: 20; font.weight: Font.Medium; color: Theme.ink; text: qsTr("Prepare %1 for the internet").arg(root.serverName) }
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.ink2; text: qsTr("Skip this if you only play at home. You can come back from Server › Settings.") }
                        UzMono { text: qsTr("Public address").toUpperCase() }
                        UzField { id: publicHost; objectName: "setupPublicHost"; Layout.fillWidth: true; mono: true; fill: Theme.ground; placeholderText: qsTr("server.example.com"); Accessible.name: qsTr("Public address"); onAccepted: root.next() }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space2
                            UzMono { text: qsTr("Relay (TURN)").toUpperCase() }
                            UzMono { objectName: "setupRelay"; Layout.fillWidth: true; color: Theme.ink; text: root.netOn("turn") ? qsTr("Built-in · on") : qsTr("Built-in · off") }
                        }
                        UzMono { text: qsTr("Ports to forward").toUpperCase(); visible: ports.text !== "" }
                        UzMono { id: ports; objectName: "setupPorts"; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.ink; text: root.settings.network && root.settings.network.turn ? (root.settings.network.turn.forwards ?? []).join(" · ") : "" }
                        UzStatusIcon { objectName: "setupInternetResult"; Layout.fillWidth: true; kind: root.internetReady ? "done" : "warning"; small: true; text: root.internetText }
                    }

                    // ---- 05 Backups
                    ColumnLayout {
                        spacing: Theme.space15
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: 20; font.weight: Font.Medium; color: Theme.ink; text: qsTr("Back up saves and settings") }
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.ink2; text: qsTr("Choose a folder on another disk and when to back up. Nothing changes if you skip this.") }
                        UzSegmented {
                            objectName: "setupBackupMode"
                            options: [{ value: "off", label: qsTr("Off"), name: "setupBackupMode_off" }, { value: "daily", label: qsTr("Daily"), name: "setupBackupMode_daily" }, { value: "weekly", label: qsTr("Weekly"), name: "setupBackupMode_weekly" }]
                            current: root.backupMode
                            onPicked: (value) => root.backupMode = value
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: Theme.space2
                            rowSpacing: Theme.space1
                            enabled: root.backupMode !== "off"
                            opacity: enabled ? 1 : 0.5
                            UzMono { text: qsTr("Folder").toUpperCase() }
                            UzField { id: folderField; objectName: "setupBackupFolder"; Layout.fillWidth: true; mono: true; fill: Theme.ground; Accessible.name: qsTr("Backup folder") }
                            UzMono { text: qsTr("Time").toUpperCase() }
                            UzField { id: timeField; objectName: "setupBackupTime"; Layout.preferredWidth: 100; mono: true; fill: Theme.ground; Accessible.name: qsTr("Time of day") }
                            UzMono { text: qsTr("Keep the last").toUpperCase() }
                            UzField { id: keepField; objectName: "setupBackupKeep"; Layout.preferredWidth: 100; mono: true; fill: Theme.ground; inputMethodHints: Qt.ImhDigitsOnly; Accessible.name: qsTr("Backups to keep") }
                        }
                    }

                    // ---- 06 Done
                    ColumnLayout {
                        spacing: Theme.space15
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: 20; font.weight: Font.Medium; color: Theme.ink; text: qsTr("%1 is ready").arg(root.serverName) }
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.ink2; text: qsTr("Here is what was set up. Anything open also shows in Status.") }
                        Repeater {
                            model: root.step === 5 ? root.summaryRows() : []
                            delegate: RowLayout {
                                id: sum
                                required property var modelData
                                objectName: "setupSummary_" + sum.modelData.key
                                Layout.fillWidth: true
                                spacing: Theme.space2
                                Text { Layout.preferredWidth: 150; elide: Text.ElideRight; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.ink; text: sum.modelData.label }
                                UzStatusIcon {
                                    objectName: "setupSummaryText_" + sum.modelData.key
                                    Layout.fillWidth: true
                                    small: true
                                    kind: sum.modelData.ok ? "done" : "warning"
                                    text: root.skipped[sum.modelData.key] === true && !sum.modelData.ok ? qsTr("%1 · skipped").arg(sum.modelData.text) : sum.modelData.text
                                }
                                UzButton {
                                    objectName: "setupSummarySetUp_" + sum.modelData.key
                                    size: "sm"
                                    visible: !sum.modelData.ok
                                    text: qsTr("Set up")
                                    onClicked: { root.error = ""; root.step = sum.modelData.step }
                                }
                            }
                        }
                        Text { Layout.fillWidth: true; Layout.topMargin: Theme.space1; wrapMode: Text.Wrap; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.input.size; color: Theme.ink2; text: qsTr("Next: invite friends from Users › Invites.") }
                    }
                }
            }

            UzStatusIcon { objectName: "setupError"; Layout.fillWidth: true; visible: root.error !== ""; kind: "error"; small: true; text: root.error; maxWidth: 720 }
            Item { Layout.fillHeight: true; visible: root.phase !== "ready" }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space1
                visible: root.phase === "ready"
                UzMono { Layout.fillWidth: true; text: root.step < 5 ? qsTr("Skip setup at any time; Status shows what is left.") : "" }
                UzButton { objectName: "setupHomeOnly"; visible: root.step === 3; enabled: !root.busy; text: qsTr("Only home network"); onClicked: root.homeOnly() }
                UzButton { objectName: "setupSkip"; visible: root.step < 5; kind: "ghost"; enabled: !root.busy; text: qsTr("Skip this step"); onClicked: root.skip() }
                UzButton { objectName: "setupNext"; visible: root.step < 5; kind: "primary"; busy: root.busy; text: qsTr("Next"); onClicked: root.next() }
                UzButton { objectName: "setupCreateInvite"; visible: root.step === 5; text: qsTr("Create an invite"); onClicked: { root.player.hideSetupAssistant(); root.player.showServerPage("users") } }
                UzButton { objectName: "setupOpenLibrary"; visible: root.step === 5; kind: "primary"; text: qsTr("Open Library"); onClicked: { root.player.hideSetupAssistant(); root.player.showLibrary() } }
            }
        }
    }
}
