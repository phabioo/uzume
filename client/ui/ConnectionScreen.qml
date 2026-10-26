import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// 3a: Connect to a server, in the frame of the full-window screens (frame.md 8b/8k). Saved servers, the address field, the
// server on this PC (starting / failed states of "Play on this PC") and the automatic connection.
Rectangle {
    id: root
    required property PlayerController player
    readonly property var local: player.localHub
    // The server of "Play on this PC" is starting or did not start: the page says so instead of asking for an address.
    readonly property bool childStarting: local.mode === "child" && (local.childState === "starting" || local.phase === "starting")
    readonly property bool childFailed: local.mode === "child" && !childStarting && local.childState === "failed"
    readonly property bool childBlock: childStarting || childFailed
    color: Theme.ground

    StartFrame {
        anchors.fill: parent
        screenLabel: qsTr("Server")
        openLabel: "UZUME · SERVER"
        openLines: [qsTr("Your games and saves stay on this PC."), qsTr("Servers are for playing with others.")]
        contentSpacing: Theme.space3

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space2
                UzSectionLabel {
                    objectName: "connectEyebrow"
                    Layout.fillWidth: true
                    number: 1
                    keepNumber: true
                    text: root.player.hubs.length === 0 ? qsTr("First start") : qsTr("Connect")
                }
                Text {
                    objectName: "connectTitle"
                    text: root.childBlock ? qsTr("Your server on this PC") : qsTr("Welcome to Uzume")
                    font.pixelSize: Theme.roles.title.size
                    font.weight: Font.DemiBold
                    font.letterSpacing: -0.6
                }
                Text {
                    objectName: "connectBody"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.muted
                    font.pixelSize: Theme.roles.body.size
                    lineHeight: 1.4
                    text: root.childBlock ? qsTr("Games and saves stay on this PC. The server runs while Uzume is open.")
                                          : qsTr("Enter the address of your Uzume server. You find it in the server under Settings → General.")
                }
            }

            // Play on this PC: the server is a child process of the app.
            UzStateBlock {
                objectName: "connectChildState"
                visible: root.childBlock
                Layout.fillWidth: true
                kind: root.childFailed ? "error" : "loading"
                title: root.childFailed ? qsTr("The server on this PC didn't start") : qsTr("Starting the server on this PC…")
                text: root.childFailed ? root.local.childError : qsTr("This takes a few seconds.")
                action: root.childFailed ? qsTr("Start again") : ""
                onActionClicked: root.local.restartChild()
            }

            // A positioner with explicit widths, not a layout: a ColumnLayout filled by a Repeater while this screen was not shown
            // was never re-run and left the saved-Hub card at 0 px (R-P2, the card lost its name and background).
            Column {
                id: hubList
                objectName: "hubList"
                Layout.fillWidth: true
                spacing: 10
                Repeater {
                    model: root.player.hubs
                    delegate: HubCard {
                        required property var modelData
                        width: hubList.width
                        hub: modelData
                        onConnectRequested: hubId => root.player.connectProfile(hubId)
                        onRetryRequested: root.player.retryConnection()
                        onRemoveRequested: hubId => {
                            // Discarding an attempt also empties the address field it came from (R-P20).
                            if (!modelData.saved) addressField.text = ""
                            root.player.removeHub(hubId)
                        }
                        onTrustCertificateRequested: fingerprint => root.player.trustChangedCertificate(fingerprint)
                        onCancelCertificateRequested: root.player.cancelCertificateChange()
                    }
                }
                UzLabel {
                    visible: root.player.hubs.length === 0
                    text: qsTr("No server saved yet.")
                    color: Theme.muted
                    font.pixelSize: Theme.roles.input.size
                    wrapMode: Text.WordWrap
                    width: hubList.width
                }
            }

            ColumnLayout {
                visible: !root.childBlock
                Layout.fillWidth: true
                spacing: 8
                UzLabel { text: qsTr("Server address"); font.pixelSize: Theme.roles.input.size; font.weight: Font.Medium }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    UzField {
                        id: addressField
                        objectName: "addressField"
                        Layout.fillWidth: true
                        placeholderText: qsTr("hub.example.com:8443")
                        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                        onAccepted: addButton.clicked()
                    }
                    UzButton {
                        id: addButton
                        objectName: "addButton"
                        implicitHeight: 44
                        busyOnClick: true
                        kind: "primary"
                        text: qsTr("Connect")
                        onClicked: root.player.addHub(addressField.text)
                    }
                }
                UzLabel {
                    objectName: "connectHint"
                    visible: root.player.connectionNotice === ""
                    text: qsTr("host:port, without https://")
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                }
                UzLabel {
                    visible: root.player.connectionNotice !== ""
                    Layout.fillWidth: true
                    text: root.player.connectionNotice
                    color: Theme.danger
                    font.pixelSize: Theme.roles.input.size
                    wrapMode: Text.WordWrap
                }
            }

            // Hub on this PC (0.9): install it or connect to it, create the admin, done.
            Rectangle {
                id: localCard
                objectName: "localHubCard"
                readonly property var local: root.player.localHub
                readonly property bool installing: local.phase === "installing"
                visible: local.available && !root.childBlock && local.mode === ""
                Layout.fillWidth: true
                implicitHeight: localCol.implicitHeight + 28
                radius: Theme.radiusNone
                color: Theme.surface
                border.width: 1
                border.color: Theme.rule
                ColumnLayout {
                    id: localCol
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: Theme.space1
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space15
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            UzLabel { text: qsTr("Set up a server on this PC"); font.pixelSize: Theme.roles.itemTitle.size; font.weight: Font.DemiBold }
                            UzLabel {
                                Layout.fillWidth: true
                                text: localCard.local.present ? qsTr("Connect to the server on this PC and sign in as its admin.")
                                                              : qsTr("Install the Uzume server on this PC to keep your games and saves here.")
                                color: Theme.muted
                                font.pixelSize: Theme.roles.input.size
                                wrapMode: Text.WordWrap
                            }
                        }
                        UzButton {
                            objectName: "setupLocalHubButton"
                            kind: "primary"
                            busy: localCard.installing || localCard.local.phase === "connecting"
                            enabled: !busy
                            text: localCard.local.present ? qsTr("Connect") : qsTr("Set up")
                            onClicked: localCard.local.start()
                        }
                    }
                    UzLabel {
                        objectName: "localHubInstallText"
                        visible: localCard.installing && localCard.local.installText !== ""
                        text: localCard.local.installText
                        color: Theme.ink2
                        font.pixelSize: Theme.roles.input.size
                    }
                    ProgressBar {
                        visible: localCard.installing && value > 0 && value < 1
                        Layout.fillWidth: true
                        value: localCard.local.installProgress
                    }
                    UzLabel {
                        objectName: "localHubError"
                        visible: localCard.local.phase === "error" && localCard.local.error !== ""
                        Layout.fillWidth: true
                        text: localCard.local.error
                        color: Theme.danger
                        font.pixelSize: Theme.roles.input.size
                        wrapMode: Text.WordWrap
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 14
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.rule }
                UzToggle {
                    id: autoToggle
                    objectName: "autoConnectToggle"
                    Layout.fillWidth: true
                    text: qsTr("Connect automatically on startup")
                    checked: root.player.autoConnect
                    onToggled: root.player.autoConnect = checked
                }
                UzMono {
                    Layout.fillWidth: true
                    text: root.player.deviceFooter
                    wrapMode: Text.WordWrap
                }
            }
    }
}
