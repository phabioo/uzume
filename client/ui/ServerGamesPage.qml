import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server › Games: everything an admin does to bring games onto the server, which used to sit in the Library and in General
// settings. "Rescan folders" (LibraryAdminBar: POST /admin/library/rescan with its progress and result line), "Upload ROM" (the
// same file dialog and path-field fallback as the Library's button), then the two server settings of the topic: Import folder and
// Members can add games (ServerSettingsGames, shown by a ServerSettingsPane with `forcePage: "games"`).
// objectNames: serverGamesPage, gamesUploadButton, gamesUploadPathPrompt, gamesUploadPathField, gamesUploadPathButton, gamesUploadStatus
// (plus libraryAdminBar and its children, settingsGames and its rows).
AdminPage {
    id: root
    objectName: "serverGamesPage"
    area: qsTr("Games")
    title: qsTr("Games")
    label: ""

    // "Upload ROM": native file dialog when QtQuick.Dialogs is installed, else a path field (see LibraryScreen).
    property bool pathPromptOpen: false
    Loader {
        id: fileDialogLoader
        active: false
        source: "UploadFileDialog.qml"
        onLoaded: item.player = root.player
    }
    function openUploadDialog() {
        fileDialogLoader.active = true
        if (fileDialogLoader.status === Loader.Ready && fileDialogLoader.item !== null) fileDialogLoader.item.open()
        else root.pathPromptOpen = !root.pathPromptOpen
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space2
    UzSectionLabel { Layout.fillWidth: true; text: qsTr("Add games") }
    LibraryAdminBar {
        Layout.fillWidth: true
        player: root.player
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space2
        visible: root.player.canUpload
        UzButton {
            objectName: "gamesUploadButton"
            size: "sm"
            enabled: !root.player.upload.active
            text: qsTr("Upload ROM")
            onClicked: root.openUploadDialog()
        }
        UzMono {
            Layout.fillWidth: true
            elide: Text.ElideRight
            text: qsTr("One game file from this PC")
        }
    }
    RowLayout {
        objectName: "gamesUploadPathPrompt"
        Layout.fillWidth: true
        visible: root.pathPromptOpen && root.player.canUpload
        spacing: Theme.space1
        UzField {
            id: uploadPath
            objectName: "gamesUploadPathField"
            Layout.fillWidth: true
            implicitHeight: Theme.row36
            placeholderText: qsTr("Path of the ROM file to upload")
            onAccepted: uploadPathButton.clicked()
        }
        UzButton {
            id: uploadPathButton
            objectName: "gamesUploadPathButton"
            implicitHeight: Theme.row36
            text: qsTr("Upload")
            enabled: uploadPath.text.trim() !== "" && !root.player.upload.active
            onClicked: { root.player.uploadRom(uploadPath.text.trim()); root.pathPromptOpen = false }
        }
    }
    RowLayout {
        objectName: "gamesUploadStatus"
        Layout.fillWidth: true
        visible: root.player.upload.active || root.player.upload.message !== ""
        spacing: Theme.space2
        UzMono {
            Layout.fillWidth: true
            visible: root.player.upload.active
            elide: Text.ElideMiddle
            text: qsTr("Uploading %1 · %2 %").arg(root.player.upload.fileName).arg(Math.round(root.player.upload.progress * 100))
        }
        UzStatusIcon {
            Layout.fillWidth: true
            visible: !root.player.upload.active
            kind: root.player.upload.isError ? "error" : "done"
            small: true
            text: root.player.upload.message
            maxWidth: 640
        }
        UzButton {
            visible: !root.player.upload.active
            kind: "link"
            text: qsTr("Dismiss")
            onClicked: root.player.dismissUploadMessage()
        }
    }

    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space2
        UzSectionLabel { Layout.fillWidth: true; text: qsTr("Settings") }
        ServerSettingsPane {
            forcePage: "games"
            player: root.player
        }
    }
}
