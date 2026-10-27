import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// 3a: Connect to a server (usability pass 6, board "Start and connect"): the centred 880 px grid of the start screens with a help
// column. "Where are your games?": the saved servers as 56 px rows with one Connect each, the automatic connection, and
// "Another server" with one field and "Add server" as the only way to add one (no second Connect). The server on this PC
// (starting / failed states of "Play on this PC") keeps its blocks.
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
        grid: true
        contentSpacing: Theme.space3
        helpItems: root.childBlock ? [] : [
            { title: qsTr("Next time it is one click less"), text: qsTr("With automatic connect on, Uzume opens straight into your library. Turn it off in Settings › Servers.") },
            { title: qsTr("An invite works here too"), text: qsTr("Paste an invite link into the field below, or with Ctrl+V anywhere in the window. It opens the invite for that server.") }
        ]
        helpLines: [qsTr("Uzume %1").arg(root.player.playerVersion)]

        StartHead {
            step: root.childBlock ? qsTr("Your server on this PC") : qsTr("Step 1 of 2 · Server")
            title: root.childBlock ? qsTr("Your server on this PC") : qsTr("Where are your games?")
            text: root.childBlock ? qsTr("Games and saves stay on this PC. The server runs while Uzume is open.") : ""
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
            visible: !root.childBlock
            Layout.fillWidth: true
            Rectangle { visible: root.player.hubs.length > 0; width: hubList.width; height: 1; color: Theme.rule }
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
        }

        UzToggle {
            id: autoToggle
            objectName: "autoConnectToggle"
            visible: root.player.hubs.length > 0 && !root.childBlock
            Layout.fillWidth: true
            text: qsTr("Connect automatically when Uzume starts")
            checked: root.player.autoConnect
            onToggled: root.player.autoConnect = checked
        }

        ColumnLayout {
            visible: !root.childBlock
            Layout.fillWidth: true
            spacing: 8
            UzSectionLabel { Layout.fillWidth: true; text: root.player.hubs.length > 0 ? qsTr("Another server") : qsTr("Add a server") }
            UzField {
                id: addressField
                objectName: "addressField"
                Layout.fillWidth: true
                placeholderText: qsTr("server.example.com:8443 or an invite link")
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                onAccepted: addButton.clicked()
            }
            UzLabel {
                visible: root.player.connectionNotice !== ""
                Layout.fillWidth: true
                text: root.player.connectionNotice
                color: Theme.danger
                font.pixelSize: Theme.roles.input.size
                wrapMode: Text.WordWrap
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space1
                UzButton {
                    id: addButton
                    objectName: "addButton"
                    busyOnClick: true
                    text: qsTr("Add server")
                    onClicked: {
                        const text = addressField.text.trim()
                        // An invite link opens the invite flow; anything else is a server address.
                        if (text.indexOf("uzume://") === 0) root.player.onboarding.openLink(text)
                        else root.player.addHub(addressField.text)
                    }
                }
                UzButton {
                    objectName: "playOnThisPcButton"
                    kind: "ghost"
                    text: qsTr("Play on this PC without a server")
                    onClicked: root.player.onboarding.beginPlay()
                }
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
    }
}
