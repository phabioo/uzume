import QtQuick
import QtQuick.Layouts
import Uzume.App

// The cheat database setting (cheat_db_v1, design 11h): Download on/off, the source address of the libretro lists and of the 3DS lists, the
// last update with "Update now", and the status line. Admins edit it through GET/PUT /admin/cheat-db and POST /admin/cheat-db/refresh;
// "Confirm it's you" is asked by AdminController. Used in Server > Settings (ServerSettingsCheatDb) and in Settings > Servers when the
// server runs only on this PC (design 11h-3). Others see one read-only line.
// objectNames: cheatDbBlock, cheatDbToggle, cheatDbSourceRow, cheatDbSourceChange, cheatDb3dsRow, cheatDb3dsChange, cheatDbUpdatedRow,
// cheatDbUpdateNow, cheatDbStatus, cheatDbReadOnly, cheatDbEditDialog, cheatDbEditField, cheatDbEditSave, cheatDbEditCancel.
ColumnLayout {
    id: root
    objectName: "cheatDbBlock"
    required property PlayerController player
    property bool pcOnly: false           // "THIS PC · NO SERVER": the block configures the app's own server
    property bool readOnly: false         // only the line "set on the server" (Settings of an app that uses another person's server)
    readonly property AdminController admin: root.player.admin
    readonly property bool editable: !root.readOnly && root.player.cheatDbSupported && root.admin.available
    // The built-in source of the server (the libretro-database archive), used when the database is turned on again and no address is known.
    readonly property string defaultUrl: "https://github.com/libretro/libretro-database/archive/refs/heads/master.zip"

    property string phase: "loading"      // loading | ready | error
    property string loadError: ""
    property var db: ({})
    property bool updating: false
    property string lastKnownUrl: ""      // the address before it was turned off
    property string editWhich: ""         // "url" | "url_3ds"
    property string editError: ""
    property bool editBusy: false
    property int polls: 0
    property string checkedBefore: ""

    readonly property bool on: root.db.enabled === true
    readonly property bool sourceOn: (root.db.url ?? "none") !== "none"
    spacing: Theme.space2
    Layout.fillWidth: true
    Layout.minimumWidth: 0

    AdminCall { id: api; admin: root.admin }

    function dateText(iso: string): string {
        const d = new Date(iso)
        return isNaN(d.getTime()) ? "" : Qt.formatDate(d, "d MMM yyyy")
    }
    function sizeText(n: real): string {
        const mib = 1024 * 1024
        return n >= mib ? qsTr("%1 MB").arg(Math.round(n / mib)) : qsTr("%1 KB").arg(Math.max(1, Math.round(n / 1024)))
    }
    readonly property bool failed: (root.db.last_error ?? "") !== "" && (root.db.last_success ?? "") < (root.db.last_check ?? "")
    readonly property string statusText: {
        if (root.updating) return qsTr("Updating… The current copy stays in use until this finishes.")
        if (!root.on) return qsTr("The cheat database is off. People can still enter and import their own cheats.")
        const when = root.db.updated_at ? root.dateText(root.db.updated_at) : ""
        if (root.failed)
            return qsTr("Update failed: %1.").arg(root.db.last_error) + (when !== "" ? " " + qsTr("The copy from %1 stays in use.").arg(when) : "")
        if (when === "") return qsTr("Nothing downloaded yet.")
        return qsTr("Up to date · DB %1 · %2").arg(when).arg(root.sizeText(root.db.size_bytes ?? 0))
    }

    function load() {
        if (!root.editable) return
        api.call("GET", "/admin/cheat-db", undefined, function (ok, data, code, message) {
            if (ok && data && data.enabled !== undefined) {
                root.db = data
                root.phase = "ready"
                if (data.url && data.url !== "none") root.lastKnownUrl = data.url
            } else if (root.phase !== "ready") {
                root.phase = "error"
                root.loadError = message
            }
        })
    }
    Component.onCompleted: root.load()
    onEditableChanged: root.load()

    function save(body: var) {
        root.editBusy = true
        root.editError = ""
        api.call("PUT", "/admin/cheat-db", body, function (ok, data, code, message) {
            root.editBusy = false
            if (ok && data && data.enabled !== undefined) {
                root.db = data
                if (data.url && data.url !== "none") root.lastKnownUrl = data.url
                editDialog.close()
                if (root.on) root.watchRefresh()
            } else {
                root.editError = message !== "" ? message : qsTr("The server did not answer.")
            }
        })
    }
    function setOn(on: bool) {
        if (!on && root.sourceOn) root.lastKnownUrl = root.db.url
        root.save({ url: on ? (root.lastKnownUrl !== "" ? root.lastKnownUrl : root.defaultUrl) : "none" })
    }
    function refresh() {
        root.updating = true
        root.checkedBefore = root.db.last_check ?? ""
        api.call("POST", "/admin/cheat-db/refresh", undefined, function (ok, data, code, message) {
            if (!ok) {
                root.updating = false
                root.editError = message
                return
            }
            root.polls = 0
            poll.restart()
        })
    }
    // A save of the address starts a refresh on the server too: watch it like "Update now".
    function watchRefresh() {
        root.updating = true
        root.checkedBefore = root.db.last_check ?? ""
        root.polls = 0
        poll.restart()
    }
    Timer {
        id: poll
        interval: 1500
        onTriggered: {
            root.polls++
            api.call("GET", "/admin/cheat-db", undefined, function (ok, data) {
                if (ok && data && data.enabled !== undefined) {
                    root.db = data
                    if ((data.last_check ?? "") !== root.checkedBefore) { root.updating = false; return }
                }
                if (root.polls < 40) poll.restart()
                else root.updating = false
            })
        }
    }

    function openEdit(which: string) {
        root.editWhich = which
        root.editError = ""
        const cur = which === "url" ? (root.db.url ?? "") : (root.db.url_3ds ?? "")
        editField.text = cur === "none" ? "" : cur
        editDialog.open()
        editField.forceActiveFocus()
    }

    // ------------------------------------------------------------ read-only face
    UzLabel {
        objectName: "cheatDbReadOnly"
        visible: !root.editable
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.muted
        font.pixelSize: Theme.roles.input.size
        text: !root.player.cheatDbSupported
              ? qsTr("This server doesn't offer a cheat database yet. People can still enter and import their own cheats.")
              : root.player.isAdmin ? qsTr("The cheat database belongs to %1. You change it under Server › Settings › Cheat database.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("the server"))
              : qsTr("The cheat database is set by the admins of %1.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("the server"))
    }

    // ------------------------------------------------------------ admin face
    UzStateBlock {
        visible: root.editable && root.phase === "loading"
        Layout.fillWidth: true
        Layout.maximumWidth: 520
        kind: "loading"
        title: qsTr("Loading the cheat database")
    }
    UzStateBlock {
        objectName: "cheatDbError"
        visible: root.editable && root.phase === "error"
        Layout.fillWidth: true
        Layout.maximumWidth: 520
        kind: "error"
        title: qsTr("Can't read the cheat database setting")
        text: root.loadError !== "" ? root.loadError : qsTr("The server did not answer.")
        action: qsTr("Try again")
        onActionClicked: { root.phase = "loading"; root.load() }
    }
    ColumnLayout {
        visible: root.editable && root.phase === "ready"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 0
        UzSettingsRow {
            objectName: "cheatDbToggleRow"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "cheatdb"
            label: qsTr("Download the cheat database")
            description: qsTr("A list of known cheats per game, matched by the game file. The cheats themselves come from the Cheats plugin.")
            ctrl: "toggle"
            toggleName: "cheatDbToggle"
            controlEnabled: !root.editBusy && !root.updating
            checked: root.on
            onToggled: value => root.setOn(value)
        }
        UzSettingsRow {
            objectName: "cheatDbSourceRow"
            visible: root.on
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "cheatdbsource"
            label: qsTr("Source")
            descriptionName: "cheatDbSource"
            description: root.sourceOn ? root.db.url : qsTr("Off")
            ctrl: "buttons"
            buttons: [{ name: "cheatDbSourceChange", text: qsTr("Change…") }]
            onButtonClicked: root.openEdit("url")
        }
        UzSettingsRow {
            objectName: "cheatDb3dsRow"
            visible: root.on
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "cheatdb3ds"
            label: qsTr("3DS lists")
            descriptionName: "cheatDb3ds"
            description: (root.db.url_3ds ?? "none") !== "none" ? root.db.url_3ds : qsTr("Off. Gateway lists for the 3DS come from an address you give here.")
            ctrl: "buttons"
            buttons: [{ name: "cheatDb3dsChange", text: qsTr("Change…") }]
            onButtonClicked: root.openEdit("url_3ds")
        }
        UzSettingsRow {
            objectName: "cheatDbUpdatedRow"
            visible: root.on
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            resetMode: "none"
            idKey: "cheatdbupdated"
            label: qsTr("Last updated")
            descriptionName: "cheatDbUpdated"
            description: root.db.updated_at ? root.dateText(root.db.updated_at) : qsTr("Never")
            ctrl: "buttons"
            buttons: [{ name: "cheatDbUpdateNow", text: qsTr("Update now"), busy: root.updating, enabled: !root.updating }]
            onButtonClicked: root.refresh()
        }
        UzStatusIcon {
            objectName: "cheatDbStatus"
            Layout.topMargin: Theme.space15
            kind: root.failed ? "warning" : (root.updating || !root.on ? "info" : "done")
            small: true
            text: root.statusText
            maxWidth: 640
        }
        UzStatusIcon {
            objectName: "cheatDbProblem"
            visible: root.editError !== "" && !editDialog.shown
            Layout.topMargin: Theme.space05
            kind: "error"
            small: true
            text: root.editError
            maxWidth: 640
        }
        UzStatusIcon {
            visible: root.pcOnly
            Layout.topMargin: Theme.space05
            kind: "info"
            small: true
            text: qsTr("Only affects this PC.")
            maxWidth: 640
        }
    }

    UzDialog {
        id: editDialog
        objectName: "cheatDbEditDialog"
        parent: Window.window ? Window.window.contentItem : root
        label: qsTr("Cheat database")
        title: root.editWhich === "url" ? qsTr("Change the source") : qsTr("Change the 3DS source")
        text: root.editWhich === "url" ? qsTr("An https address of a libretro-database archive (zip).")
                                       : qsTr("An https address of a zip with 3DS Gateway lists (<TITLEID>.txt). Leave it empty to turn the 3DS lists off.")
        error: root.editError
        busy: root.editBusy
        UzField {
            id: editField
            objectName: "cheatDbEditField"
            Layout.fillWidth: true
            fill: Theme.ground
            mono: true
            placeholderText: "https://…"
            onAccepted: editSave.clicked()
        }
        actions: [
            UzButton {
                id: editSave
                objectName: "cheatDbEditSave"
                kind: "primary"
                text: qsTr("Save")
                onClicked: {
                    const v = editField.text.trim()
                    const body = {}
                    body[root.editWhich] = v === "" ? "none" : v
                    root.save(body)
                }
            },
            UzButton { objectName: "cheatDbEditCancel"; text: qsTr("Cancel"); onClicked: editDialog.close() }
        ]
    }
    // The dialog that sent a request needing "Confirm it's you" steps aside while the global dialog asks.
    Binding { target: editDialog; property: "visible"; value: false; when: root.admin.confirmNeeded }
}
