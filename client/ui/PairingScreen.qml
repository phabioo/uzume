import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// 3b: Add hub (hub found, confirm certificate, approve device).
Rectangle {
    id: root
    required property PlayerController player
    readonly property var info: player.pairing
    readonly property string phase: info.phase
    property bool inviteMode: false
    // A Hub this device knew (it connected before) that no longer has this device's pairing.
    readonly property bool pairAgain: info.pairAgain === true
    // Approval step is ready for input (request approval or redeem an invite)
    // "Set up a server on this PC": the Hub on this PC pairs the Player with an admin sign-in instead of an approval.
    readonly property var local: player.localHub
    readonly property bool localForm: local.phase === "needsSetup" || local.phase === "needsSignIn" || local.phase === "working"
    readonly property bool canChoose: !root.localForm && (root.phase === "needsPairing" || root.phase === "denied" || root.phase === "expired")
    color: Theme.ground

    // Esc leaves the step like the Cancel / Back button of the phase (R-P4).
    Shortcut {
        sequence: "Esc"
        enabled: root.visible
        onActivated: {
            if (root.phase === "awaiting") root.player.cancelPairing()
            else if (root.phase === "trust") root.player.rejectTrust()
            else root.player.leavePairing()
        }
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + 64
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        ColumnLayout {
            id: column
            width: Math.min(640, flick.width - 32)
            x: (flick.width - width) / 2
            y: Math.max(32, (flick.height - implicitHeight) / 2)
            spacing: 14

            ColumnLayout {
                spacing: 6
                Layout.bottomMargin: 8
                Eyebrow {
                    objectName: "pairingEyebrow"
                    visible: root.pairAgain
                    text: qsTr("%1 · pairing lost").arg((root.info.hubName || "").toUpperCase())
                }
                UzLabel {
                    objectName: "pairingTitle"
                    // A known Hub that lost its pairing says "Pair again" (P2); a new address is "Add hub".
                    text: root.localForm ? qsTr("Server on this PC") : (root.pairAgain ? qsTr("Pair again") : qsTr("Add server"))
                    font.pixelSize: Theme.roles.title.size
                    font.weight: Font.DemiBold
                    font.letterSpacing: -0.6
                }
                UzLabel {
                    objectName: "pairingBody"
                    visible: root.pairAgain && !root.localForm
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.muted
                    font.pixelSize: Theme.roles.body.size
                    lineHeight: 1.4
                    text: qsTr("%1 no longer knows this device. Its admin may have revoked it. Pair again to get a new approval; your local saves are kept.")
                              .arg(root.info.hubName || qsTr("The server"))
                }
                UzMono { text: root.info.address; color: Theme.muted; font.pixelSize: 13 }
            }

            // Step 1: hub found
            StepCard {
                Layout.fillWidth: true
                objectName: "stepHub"
                title: root.info.hubKnown ? qsTr("Server detected") : qsTr("Searching for server…")
                stage: root.info.hubKnown ? "done" : "active"
                UzMono {
                    visible: root.info.hubKnown
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.ink2
                    text: qsTr("%1 · Uzume server %2 · Protocol v%3 · compatible")
                              .arg(root.info.hubName).arg(root.player.updates.shown(root.info.hubVersion)).arg(root.info.protocol)
                }
            }

            // Step 2: certificate (TOFU, explicit confirmation)
            StepCard {
                id: certStep
                Layout.fillWidth: true
                objectName: "stepCertificate"
                title: root.phase === "trust" ? qsTr("Check certificate")
                       : (certStep.stage === "done" ? (root.info.tls ? qsTr("Certificate trusted") : qsTr("Unencrypted connection")) : qsTr("Certificate"))
                stage: !root.info.hubKnown ? "pending" : (root.phase === "trust" ? "active" : "done")

                UzMono {
                    visible: root.info.tls && root.info.hubKnown
                    objectName: "fingerprintText"
                    Layout.fillWidth: true
                    text: root.info.fingerprint
                    color: Theme.ink
                    font.pixelSize: Theme.roles.small.size
                    lineHeight: 1.35
                }
                UzLabel {
                    visible: root.phase === "trust"
                    Layout.fillWidth: true
                    text: qsTr("SHA-256 fingerprint of the server certificate. Compare it with the one shown in the server and trust it only if both match. If the certificate changes later, the connection is blocked.")
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    visible: root.phase === "trust"
                    Layout.topMargin: 4
                    spacing: 10
                    UzButton {
                        objectName: "trustButton"
                        kind: "primary"
                        busyOnClick: true
                        text: qsTr("Trust fingerprint")
                        onClicked: root.player.confirmTrust()
                    }
                    UzButton {
                        objectName: "rejectButton"
                        text: qsTr("Cancel")
                        onClicked: root.player.rejectTrust()
                    }
                }
                UzLabel {
                    visible: root.phase !== "trust" && root.info.hubKnown
                    Layout.fillWidth: true
                    text: root.info.tls ? qsTr("Saved. If the certificate changes later, the connection is blocked.")
                                        : qsTr("Development only: this server is contacted without TLS, so there is no fingerprint.")
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                    wrapMode: Text.WordWrap
                }
            }

            // Step 3: approve device
            StepCard {
                id: approvalStep
                Layout.fillWidth: true
                objectName: "stepApproval"
                title: root.localForm ? qsTr("Sign in") : qsTr("Approve device")
                stage: (root.phase === "trust" || !root.info.hubKnown) ? "pending" : "active"

                UzSegmented {
                    objectName: "pairingModeSegment"
                    visible: root.canChoose
                    options: [
                        { value: "request", label: qsTr("Request approval"), name: "requestSegment" },
                        { value: "invite", label: qsTr("I have an invite code"), name: "inviteSegment" }
                    ]
                    current: root.inviteMode ? "invite" : "request"
                    onPicked: value => root.inviteMode = (value === "invite")
                }

                LocalHubForm {
                    objectName: "localHubForm"
                    visible: root.localForm
                    Layout.fillWidth: true
                    player: root.player
                }

                ColumnLayout {
                    objectName: "inviteForm"
                    visible: root.canChoose && root.inviteMode
                    Layout.fillWidth: true
                    spacing: 6
                    Eyebrow { text: qsTr("Invite code") }
                    UzField {
                        id: inviteCodeField
                        objectName: "inviteCodeField"
                        Layout.fillWidth: true
                        placeholderText: qsTr("UZ-XXXX-XXXX")
                        font.capitalization: Font.AllUppercase
                        inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhUppercaseOnly
                        enabled: !root.info.inviteBusy
                        onAccepted: inviteNameField.forceActiveFocus()
                    }
                    Eyebrow { text: qsTr("Display name"); Layout.topMargin: 4 }
                    UzField {
                        id: inviteNameField
                        objectName: "inviteNameField"
                        Layout.fillWidth: true
                        maximumLength: 32
                        font.family: Qt.application.font.family
                        placeholderText: qsTr("How others see you on this server")
                        enabled: !root.info.inviteBusy
                        onAccepted: redeemButton.clicked()
                    }
                    UzLabel {
                        Layout.fillWidth: true
                        text: qsTr("The invite code comes from the server admin and works once. Your user is created on this server with the name above.")
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                        wrapMode: Text.WordWrap
                    }
                }

                RowLayout {
                    visible: root.phase === "awaiting" || root.phase === "authenticating"
                    spacing: 12
                    BusyIndicator {
                        running: parent.visible
                        implicitWidth: 22
                        implicitHeight: 22
                        palette.dark: Theme.accent
                    }
                    ColumnLayout {
                        spacing: 2
                        UzLabel {
                            text: root.phase === "authenticating" ? qsTr("Signing in…") : qsTr("Waiting for admin approval")
                            font.pixelSize: Theme.roles.body.size
                            font.weight: Font.Medium
                        }
                        UzLabel {
                            visible: root.phase === "awaiting"
                            text: qsTr("The request appears in the server under Clients.")
                            color: Theme.muted
                            font.pixelSize: Theme.roles.small.size
                        }
                    }
                }

                UzLabel {
                    visible: root.phase === "needsPairing" && !root.inviteMode && !root.localForm
                    Layout.fillWidth: true
                    text: qsTr("This device is not yet approved on the server. The server admin must confirm the request.")
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                    wrapMode: Text.WordWrap
                }
                Rectangle {
                    visible: root.phase === "denied" || root.phase === "expired"
                    objectName: "pairingProblem"
                    Layout.fillWidth: true
                    implicitHeight: problemText.implicitHeight + 24
                    radius: 0
                    color: root.phase === "denied" ? Theme.dangerTint : Theme.ground
                    UzLabel {
                        id: problemText
                        anchors.fill: parent
                        anchors.margins: 12
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.roles.input.size
                        color: root.phase === "denied" ? Theme.danger : Theme.ink2
                        text: root.phase === "denied" ? qsTr("The request was denied by the admin.")
                                                      : qsTr("The request has expired. Please request again.")
                    }
                }
                UzLabel {
                    visible: root.info.error !== "" && approvalStep.stage === "active"
                    Layout.fillWidth: true
                    text: root.info.error
                    color: Theme.danger
                    font.pixelSize: Theme.roles.small.size
                    wrapMode: Text.WordWrap
                }

                GridLayout {
                    visible: approvalStep.stage === "active" && !root.localForm
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 6
                    Layout.topMargin: 4
                    Eyebrow { text: qsTr("Device name"); Layout.preferredWidth: 130 }
                    UzMono { text: root.info.deviceName; color: Theme.ink; Layout.fillWidth: true; elide: Text.ElideRight }
                    Eyebrow { text: qsTr("Platform"); Layout.preferredWidth: 130 }
                    UzMono { text: root.info.platform; color: Theme.ink; Layout.fillWidth: true }
                    Eyebrow { text: qsTr("Uzume version"); Layout.preferredWidth: 130 }
                    UzMono { text: root.player.updates.shown(root.info.playerVersion); color: Theme.ink; Layout.fillWidth: true }
                }

                RowLayout {
                    visible: approvalStep.stage === "active"
                    Layout.topMargin: 4
                    spacing: 10
                    UzButton {
                        objectName: "requestButton"
                        visible: root.canChoose && !root.inviteMode
                        kind: "primary"
                        busyOnClick: true
                        text: root.pairAgain && root.phase === "needsPairing" ? qsTr("Pair again") : (root.phase === "needsPairing" ? qsTr("Request approval") : qsTr("Request again"))
                        onClicked: root.player.requestPairing()
                    }
                    UzButton {
                        id: redeemButton
                        objectName: "redeemButton"
                        visible: root.canChoose && root.inviteMode
                        kind: "primary"
                        enabled: !root.info.inviteBusy
                        busy: root.info.inviteBusy
                        text: root.info.inviteBusy ? qsTr("Redeeming…") : qsTr("Redeem invite")
                        onClicked: root.player.redeemInvite(inviteCodeField.text, inviteNameField.text)
                    }
                    UzButton {
                        objectName: "cancelRequestButton"
                        visible: root.phase === "awaiting"
                        text: qsTr("Cancel request")
                        onClicked: root.player.cancelPairing()
                    }
                    UzButton {
                        objectName: "backButton"
                        visible: root.phase !== "awaiting"
                        text: qsTr("Back")
                        onClicked: root.player.leavePairing()
                    }
                }
            }
        }
    }
}
