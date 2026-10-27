import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// 8a-3 Play on this PC: "Where are your games?" The folder goes to the library-folder flow (progress in player.libraryScan, the
// result in player.libraryGameCount). States: nothing picked, scanning (Start playing waits), games found, nothing found, no
// access. Start playing and Add later end the step (OnboardingController::finishPlayOnThisPc).
StartPage {
    id: root
    required property PlayerController player
    readonly property var ob: player.onboarding
    readonly property var scan: player.libraryScan
    readonly property bool scanning: scan.active === true
    readonly property int games: player.libraryGameCount
    property bool added: false          // a folder was added on this page
    property bool noAccess: false
    readonly property string view: noAccess ? "noaccess" : !added ? "pick" : scanning ? "scanning" : games > 0 ? "found" : "empty"
    screenLabel: qsTr("Play on this PC")
    openLabel: qsTr("UZUME · PLAY ON THIS PC")
    initialFocus: folderField.field
    hints: [{ kbd: "Enter", pad: "A", label: qsTr("Choose") }, { kbd: "Esc", pad: "B", label: qsTr("Back") }]
    onBackRequested: ob.back()

    function normalized(p: string): string { return String(p).replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase() }
    function toPath(u: string): string { return String(u).replace(/^file:\/\/\/([A-Za-z]:)/, "$1").replace(/^file:\/\//, "") }
    function useFolder(source: string) {
        const path = root.toPath(source)
        folderField.text = path
        const before = root.player.libraryFolders.length
        root.ob.addGameFolder(source)
        const list = root.player.libraryFolders
        const known = list.some(f => root.normalized(f) === root.normalized(path))
        root.noAccess = !known && list.length === before
        root.added = known || list.length > before
    }

    Loader {
        id: dialogLoader
        active: false
        source: "StartFolderDialog.qml"
        onLoaded: item.folderPicked.connect(url => root.useFolder(url))
    }
    function browse() {
        dialogLoader.active = true
        if (dialogLoader.status === Loader.Ready && dialogLoader.item !== null) dialogLoader.item.open()
    }

    StartHead {
        step: root.screenLabel
        number: 1
        label: qsTr("Play on this PC")
        meta: qsTr("No account")
        title: qsTr("Where are your games?")
        text: qsTr("Pick the folder with your game files. Uzume looks inside sub-folders too and never changes the files.")
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space1
            StartField {
                id: folderField
                name: "playFolderField"
                label: qsTr("Game folder")
                mono: true
                error: root.noAccess ? qsTr("Uzume can't read this folder. Pick another one or check its permissions.") : ""
                placeholder: qsTr("Folder path, e.g. D:\\Games\\ROMs")
                Layout.fillWidth: true
                onAccepted: if (text.trim() !== "") root.useFolder(text.trim())
            }
            UzButton {
                objectName: "playBrowse"
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 24
                text: qsTr("Browse…")
                onClicked: root.browse()
            }
        }
        // Result line
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.space1
            spacing: Theme.space1
            visible: root.view === "scanning" || root.view === "found" || root.view === "empty"
            ColumnLayout {
                objectName: "playScanning"
                visible: root.view === "scanning"
                Layout.fillWidth: true
                spacing: Theme.space1
                UzProgress {
                    Layout.fillWidth: true
                    thin: true
                    value: root.scan.total > 0 ? root.scan.done / root.scan.total : 0
                    label: root.scan.done > 0 ? qsTr("Looking… %1 files checked").arg(Number(root.scan.done).toLocaleString(Qt.locale("en_US"), "f", 0))
                                              : qsTr("Looking…")
                }
            }
            UzStatusIcon {
                objectName: "playFound"
                visible: root.view === "found"
                kind: "done"
                text: qsTr("Found %1.").arg(Theme.plural(root.games, qsTr("game"), qsTr("games")))
            }
            Text {
                objectName: "playFirmwareNote"
                visible: root.view === "found"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("NDS and 3DS need firmware. You can add it later in Systems & cores.")
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
            RowLayout {
                objectName: "playEmpty"
                visible: root.view === "empty"
                Layout.fillWidth: true
                spacing: Theme.space2
                UzStatusIcon {
                    kind: "warning"
                    text: qsTr("No games in this folder. Uzume reads .gb, .gbc, .gba, .nds and .3ds.")
                    maxWidth: 380
                }
                UzButton {
                    objectName: "playPickAnother"
                    kind: "link"
                    text: qsTr("Pick another folder")
                    onClicked: { root.added = false; folderField.text = ""; root.browse() }
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        UzButton {
            objectName: "playStart"
            kind: "primary"
            text: qsTr("Start playing")
            enabled: root.view === "found"
            onClicked: root.ob.finishPlayOnThisPc()
        }
        UzButton {
            objectName: "playLater"
            text: qsTr("Add later")
            onClicked: root.ob.finishPlayOnThisPc()
        }
        Item { Layout.fillWidth: true }
    }
    Text {
        objectName: "playFootNote"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("Saves are kept in the user folder and backed up every day. Share with friends later in Settings › This PC › Servers › Share with others.")
        textFormat: Text.PlainText
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
        color: Theme.muted
    }
}
