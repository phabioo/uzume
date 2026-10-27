import QtQuick
import QtQuick.Layouts
import Uzume.App

// Library, admin part 2 (accounts-and-server.md 3j-app-1, 3j-app-2): the details section for the selected game, in the Manage tab of the game page:
// Uploaded by, File (the API carries the file name, not a path on the server's disk), SHA-256 with Copy, Saves (people, slots from
// GET /admin/saves?game_id=). Actions: Rename... (display name only: PATCH /admin/games/{id}; the file keeps its name) and
// Delete game... (danger: DELETE /admin/games/{id}; the file goes for everyone, the saves stay; the server asks "Confirm it's you"
// itself). The Library list is read again after each change. The game page shows it for admins in its Manage tab.
// objectNames: libraryAdminDetails, libraryAdminUploader, libraryAdminFile, libraryAdminSha, libraryAdminCopy, libraryAdminSaves,
// libraryAdminRename, libraryAdminDelete, libraryAdminNote.
Item {
    id: root
    implicitHeight: col.implicitHeight
    objectName: "libraryAdminDetails"
    required property PlayerController player
    readonly property AdminController admin: root.player.admin
    readonly property var game: root.player.selectedGame
    readonly property string gameId: root.game.id === undefined ? "" : root.game.id
    property string savesText: qsTr("Reading…")
    property string note: ""
    property string noteKind: "done"
    property string copied: ""
    signal gameRemoved(string note)   // the game was deleted: the game page it sits in closes and the Library says so

    AdminCall { id: api; admin: root.admin }
    AdminPeople { id: people; admin: root.admin }
    AdminAskDialog { id: ask }

    function savesSummary(list: var): string {
        if (list.length === 0) return qsTr("No saves yet")
        const who = {}
        for (const s of list) who[s.user_id] = true
        const n = Object.keys(who).length
        return qsTr("%1 in %2").arg(Theme.plural(list.length, qsTr("slot"), qsTr("slots"))).arg(Theme.plural(n, qsTr("person"), qsTr("people")))
    }
    function load() {
        root.note = ""
        root.copied = ""
        root.savesText = qsTr("Reading…")
        if (root.gameId === "" || !root.visible || !root.admin.available) return
        if (!people.loaded) people.load()
        const id = root.gameId
        api.call("GET", "/admin/saves?game_id=" + encodeURIComponent(id), undefined, function (ok, data, code, message) {
            if (id !== root.gameId) return
            root.savesText = ok && data && data.saves !== undefined ? root.savesSummary(data.saves) : qsTr("Could not read the saves")
        })
    }
    onGameIdChanged: root.load()
    onVisibleChanged: if (visible) root.load()
    Component.onCompleted: root.load()

    function rename() {
        const id = root.gameId
        ask.ask({
            label: qsTr("Library"),
            title: qsTr("Rename game"),
            text: qsTr("Changes the name shown in the Library. The file keeps its name."),
            action: qsTr("Rename"),
            input: true,
            inputLabel: qsTr("Name"),
            inputText: root.game.title ?? "",
            maxLength: 100,
            run: (value, finish) => api.call("PATCH", "/admin/games/" + id, { title: value }, function (ok, data, code, message) {
                if (ok) {
                    root.noteKind = "done"
                    root.note = qsTr("Renamed to %1").arg(value)
                    root.player.reloadLibrary()
                }
                finish(ok, ok ? "" : api.failText(qsTr("Could not rename"), message))
            })
        })
    }
    function remove() {
        const id = root.gameId
        const title = root.game.title ?? ""
        ask.ask({
            label: qsTr("Library"),
            title: qsTr("Delete %1?").arg(title),
            text: qsTr("The game file is removed for everyone. All saves are kept. You can undo this by adding the file again. The server may ask you to confirm it's you first."),
            action: qsTr("Delete game"),
            danger: true,
            run: (value, finish) => api.call("DELETE", "/admin/games/" + id, undefined, function (ok, data, code, message) {
                if (ok) {
                    root.noteKind = "done"
                    root.note = qsTr("Deleted %1. Its saves are kept.").arg(title)
                    root.player.reloadLibrary()
                    root.gameRemoved(root.note)
                }
                finish(ok, ok ? "" : code === "confirmation_cancelled" ? qsTr("Not confirmed. Nothing was deleted.")
                                      : api.failText(qsTr("Could not delete"), message))
            })
        })
    }

    ColumnLayout {
        id: col
        width: root.width
        spacing: Theme.space1
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("For admins · %1").arg(root.game.title ?? "") }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.space3
            rowSpacing: Theme.space05
            UzMono { text: qsTr("Uploaded by").toUpperCase() }
            Text {
                objectName: "libraryAdminUploader"
                Layout.fillWidth: true
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.input.size
                color: Theme.ink
                text: people.nameOf(root.game.uploadedBy ?? "", root.game.uploadedBy ?? "")
            }
            UzMono { text: qsTr("File").toUpperCase() }
            UzMono { objectName: "libraryAdminFile"; Layout.fillWidth: true; elide: Text.ElideMiddle; color: Theme.ink; text: root.game.romFilename ?? "" }
            UzMono { text: qsTr("SHA-256").toUpperCase() }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space2
                UzMono { objectName: "libraryAdminSha"; Layout.fillWidth: true; elide: Text.ElideMiddle; color: Theme.ink; text: root.game.sha ?? "" }
                UzButton {
                    objectName: "libraryAdminCopy"
                    kind: "link"
                    text: root.copied === root.gameId ? qsTr("Copied") : qsTr("Copy")
                    onClicked: { ask.copy(root.game.sha ?? ""); root.copied = root.gameId }
                }
            }
            UzMono { text: qsTr("Saves").toUpperCase() }
            Text {
                objectName: "libraryAdminSaves"
                Layout.fillWidth: true
                elide: Text.ElideRight
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.input.size
                color: Theme.ink
                text: root.savesText
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.space1
            spacing: Theme.space2
            UzButton { objectName: "libraryAdminRename"; size: "sm"; text: qsTr("Rename…"); onClicked: root.rename() }
            UzStatusIcon {
                objectName: "libraryAdminNote"
                Layout.fillWidth: true
                visible: root.note !== ""
                kind: root.noteKind
                small: true
                text: root.note
                maxWidth: 520
            }
            Item { Layout.fillWidth: true; visible: root.note === "" }
            UzButton { objectName: "libraryAdminDelete"; size: "sm"; kind: "danger"; text: qsTr("Delete game…"); onClicked: root.remove() }
        }
    }
}
