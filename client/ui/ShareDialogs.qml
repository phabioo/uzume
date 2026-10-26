import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Share with others / Stop sharing (runs-and-server.md 8i-1 to 8i-6). One dialog at a time, chosen from the state of
// `player.localHub`: sharePhase (progress while preparing / installing / stopping / resuming, error when failed), shareDone
// ("You're sharing"), sharingStopped (8i-6e) and shareDialog (intro 8i-1, setup 8i-2, stop 8i-5, opened by the buttons in
// Settings, the Users page and My account). The dialogs are created only while shown (Loader), so a closed flow costs nothing.
// objectNames: shareIntroContinue, shareServerName, shareYourName, shareYourPassword, shareJoinSelect, shareHostPort,
// shareSetupConfirm, shareSetupBack, shareProgressDialog, shareDoneDialog, shareDoneInvite, shareErrorDialog, shareErrorRetry,
// shareErrorPort, shareErrorClose, shareStoppedDialog, shareAgainButton, stopSharingDialog, stopSharingConfirm.
Item {
    id: root
    required property PlayerController player
    readonly property var lh: root.player.localHub
    readonly property string phase: root.lh.sharePhase
    readonly property string showing: (phase === "preparing" || phase === "installing" || phase === "stopping" || phase === "resuming") ? "progress"
                                      : phase === "failed" ? "error"
                                      : root.lh.shareDone ? "done"
                                      : root.lh.sharingStopped ? "stopped"
                                      : root.lh.shareDialog
    anchors.fill: parent
    z: 150

    // A line of a dialog: bold title, muted explanation.
    component Fact: ColumnLayout {
        property string title: ""
        property string text: ""
        Layout.fillWidth: true
        spacing: 2
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: parent.title
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.body.size
            font.weight: Font.Medium
            color: Theme.ink
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: parent.text
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.muted
        }
    }
    // A step of the progress list: square marker (done: filled, active: frame in accent) and the step's name.
    component Step: RowLayout {
        property string title: ""
        property string detail: ""
        property string state: "pending"   // pending | active | done
        Layout.fillWidth: true
        spacing: Theme.space15
        Rectangle {
            Layout.preferredWidth: 10
            Layout.preferredHeight: 10
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 5
            radius: 0
            color: parent.state === "done" ? Theme.accent : "transparent"
            border.width: parent.state === "done" ? 0 : 1
            border.color: parent.state === "active" ? Theme.accent : Theme.rule
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Text {
                Layout.fillWidth: true
                text: parent.parent.title
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.roles.body.size
                color: parent.parent.state === "pending" ? Theme.muted : Theme.ink
            }
            Text {
                visible: parent.parent.detail !== ""
                Layout.fillWidth: true
                text: parent.parent.detail
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
        }
    }

    Loader {
        id: loader
        anchors.fill: parent
        active: root.showing !== ""
        sourceComponent: {
            switch (root.showing) {
            case "intro": return introC
            case "setup": return setupC
            case "progress": return progressC
            case "done": return doneC
            case "error": return errorC
            case "stopped": return stoppedC
            case "stop": return stopC
            default: return null
            }
        }
    }

    // ---------------------------------------------------------------- 8i-1 what changes
    Component {
        id: introC
        UzDialog {
            objectName: "shareIntroDialog"
            shown: true
            size: "lg"
            label: qsTr("Share with others · 1 of 3")
            title: qsTr("Run a server on this PC")
            text: qsTr("Your library and saves stay. They move into the server, so friends can join you.")
            onClosed: root.lh.closeShareDialog()
            Fact { title: qsTr("Keeps running when Uzume is closed"); text: qsTr("As a Windows service. You can stop it in Server › Settings.") }
            Fact { title: qsTr("Friends join with an invite"); text: qsTr("At home or over the internet; you decide next.") }
            Fact { title: qsTr("Admin rights needed once"); text: qsTr("Windows asks while the service is installed.") }
            Fact { title: qsTr("You become the admin"); text: qsTr("With a password. Nothing is lost.") }
            actions: [
                UzButton { objectName: "shareIntroContinue"; kind: "primary"; text: qsTr("Continue"); onClicked: root.lh.openShareDialog("setup") },
                UzButton { objectName: "shareIntroCancel"; text: qsTr("Cancel"); onClicked: root.lh.closeShareDialog() }
            ]
        }
    }

    // ---------------------------------------------------------------- 8i-2 set up
    Component {
        id: setupC
        UzDialog {
            id: dlg
            objectName: "shareSetupDialog"
            shown: true
            label: qsTr("Share with others · 2 of 3")
            title: qsTr("Set up Home server")
            onClosed: root.lh.closeShareDialog()
            property string join: "home"
            readonly property int portValue: Number(portField.text)
            readonly property bool portOk: /^[0-9]{1,5}$/.test(portField.text) && portValue >= 1 && portValue <= 65535
            readonly property bool passwordOk: passwordField.text.length >= 12
            readonly property bool formOk: nameField.text.trim() !== "" && yourField.text.trim() !== "" && passwordOk && portOk
            property bool tried: false
            Component.onCompleted: {
                nameField.text = qsTr("Home server")
                yourField.text = root.player.userName
                portField.text = String(root.lh.shareSuggestedPort > 0 ? root.lh.shareSuggestedPort : 8443)
                nameField.field.selectAll()
                nameField.field.forceActiveFocus()
            }
            function submit() {
                dlg.tried = true
                if (dlg.formOk) root.lh.beginShare(nameField.text, yourField.text, passwordField.text, dlg.join, dlg.portValue)
            }
            StartField { id: nameField; fieldFill: Theme.ground; name: "shareServerName"; label: qsTr("Server name"); onAccepted: yourField.field.forceActiveFocus() }
            StartField { id: yourField; fieldFill: Theme.ground; name: "shareYourName"; label: qsTr("Your name"); onAccepted: passwordField.field.forceActiveFocus() }
            StartField {
                id: passwordField
                fieldFill: Theme.ground
                name: "shareYourPassword"
                label: qsTr("Password")
                secret: true
                hint: qsTr("At least 12 characters. You need it to sign in on other PCs.")
                error: dlg.tried && !dlg.passwordOk ? qsTr("Use at least 12 characters.") : ""
                onAccepted: dlg.submit()
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space1
                Text {
                    text: qsTr("Who can join")
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.fieldLabel.size
                    font.weight: Font.Medium
                    color: Theme.ink
                }
                UzSelect {
                    objectName: "shareJoinSelect"
                    Layout.fillWidth: true
                    model: [{ value: "home", label: qsTr("Home network only") }, { value: "internet", label: qsTr("Home network and internet") }]
                    current: dlg.join
                    onPicked: value => dlg.join = value
                }
            }
            StartField {
                id: portField
                fieldFill: Theme.ground
                name: "shareHostPort"
                label: qsTr("Port")
                mono: true
                error: dlg.tried && !dlg.portOk ? qsTr("Use a port between 1 and 65535.") : ""
                onAccepted: dlg.submit()
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Windows asks to let the Uzume server through the firewall. Allow it for private networks. For the internet, also forward 8443 TCP, 3478 UDP+TCP and 49160–49199 UDP.")
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                lineHeight: Theme.roles.small.line
                lineHeightMode: Text.FixedHeight
                color: Theme.muted
            }
            actions: [
                UzButton { objectName: "shareSetupConfirm"; destructive: true; kind: "primary"; text: qsTr("Share"); onClicked: dlg.submit() },
                UzButton { objectName: "shareSetupBack"; text: qsTr("Back"); onClicked: root.lh.openShareDialog("intro") }
            ]
        }
    }

    // ---------------------------------------------------------------- 8i-3 progress (and stopping / starting)
    Component {
        id: progressC
        UzDialog {
            objectName: "shareProgressDialog"
            shown: true
            closable: false
            readonly property bool sharing: root.phase === "preparing" || root.phase === "installing"
            label: sharing ? qsTr("Share with others · 3 of 3") : qsTr("Server")
            title: root.phase === "stopping" ? qsTr("Stopping the server…") : root.phase === "resuming" ? qsTr("Starting the server…") : qsTr("Setting up…")
            text: sharing ? qsTr("Windows asks for admin rights once. Uzume closes and opens again when the server is installed.")
                          : qsTr("Windows asks for admin rights.")
            Step { visible: sharing; title: qsTr("Create admin"); state: root.phase === "preparing" ? "active" : "done" }
            Step { visible: sharing; title: qsTr("Install the server service"); detail: qsTr("Admin rights needed"); state: root.phase === "installing" ? "active" : "pending" }
            Step { visible: sharing; title: qsTr("Move your library and saves"); detail: qsTr("Nothing is overwritten"); state: "pending" }
            Step { visible: sharing; title: qsTr("Start the server"); state: "pending" }
            UzProgress { Layout.fillWidth: true; thin: true; value: root.phase === "preparing" ? 0.15 : root.phase === "installing" ? 0.45 : 0.6 }
            actions: [UzButton { objectName: "shareProgressButton"; kind: "primary"; enabled: false; text: sharing ? qsTr("Sharing…") : qsTr("Working…") }]
        }
    }

    // ---------------------------------------------------------------- 8i-4 done
    Component {
        id: doneC
        UzDialog {
            objectName: "shareDoneDialog"
            shown: true
            label: qsTr("Share with others")
            title: qsTr("You're sharing")
            text: root.lh.shareJoin === "internet" ? qsTr("%1 is running on this PC.").arg(root.player.hubName)
                                                    : qsTr("%1 is running on this PC. Home network only.").arg(root.player.hubName)
            onClosed: root.lh.acknowledgeShareDone()
            Fact { title: qsTr("Server"); text: root.player.hubName + " · " + root.player.hubAddress.replace(/^https?:\/\//, "") + " · " + qsTr("on this PC") }
            Fact { title: qsTr("Join"); text: root.lh.shareJoin === "internet" ? qsTr("Home network and internet") : qsTr("Home network only") }
            Fact {
                objectName: "shareDoneInternetNote"
                visible: root.lh.shareJoin === "internet"
                title: qsTr("Internet sharing")
                text: qsTr("Set it up in Server › Settings › Network & sharing.")
            }
            actions: [
                UzButton { objectName: "shareDoneInvite"; kind: "primary"; text: qsTr("Invite a friend"); onClicked: { root.lh.acknowledgeShareDone(); root.player.showServerPage("users") } },
                UzButton { objectName: "shareDoneClose"; text: qsTr("Close"); onClicked: root.lh.acknowledgeShareDone() }
            ]
        }
    }

    // ---------------------------------------------------------------- 8i-6 states
    Component {
        id: errorC
        UzDialog {
            id: dlg
            objectName: "shareErrorDialog"
            shown: true
            readonly property string code: root.lh.shareErrorCode
            readonly property bool stopFlow: code === "stop_failed" || (code === "admin_rights_refused" && root.lh.mode === "service")
            readonly property bool startFlow: code === "start_failed" || (code === "admin_rights_refused" && root.lh.shareAgainAvailable && !root.lh.canRetryShare)
            readonly property bool installFlow: code === "install_failed" || (code === "admin_rights_refused" && !stopFlow && !startFlow)
            label: stopFlow ? qsTr("Stop sharing") : qsTr("Share with others")
            title: code === "admin_rights_refused" ? (stopFlow ? qsTr("The server wasn't stopped") : qsTr("The service wasn't installed"))
                   : code === "port_in_use" ? qsTr("Port %1 is busy").arg(root.lh.sharePort)
                   : code === "install_failed" ? qsTr("Sharing didn't finish")
                   : code === "stop_failed" ? qsTr("The server wasn't stopped")
                   : code === "start_failed" ? qsTr("The server didn't start")
                   : qsTr("Sharing didn't start")
            text: root.lh.shareError
            onClosed: root.lh.dismissShareError()
            Text {
                visible: dlg.code === "admin_rights_refused"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Ask someone with admin rights on this PC to approve it.")
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
            Text {
                visible: dlg.code === "install_failed"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Back to On this PC. Your library and saves are untouched.")
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
            actions: [
                UzButton {
                    objectName: "shareErrorPort"
                    destructive: true   // retries the sharing on another port
                    visible: dlg.code === "port_in_use" && root.lh.shareSuggestedPort > 0
                    kind: "primary"
                    text: qsTr("Use %1").arg(root.lh.shareSuggestedPort)
                    onClicked: {
                        const port = root.lh.shareSuggestedPort
                        if (root.lh.canRetryShare) root.lh.retryShare(port)
                        else { root.lh.dismissShareError(); root.lh.openShareDialog("setup") }   // the form is filled with the suggestion
                    }
                },
                UzButton {
                    objectName: "shareErrorRetry"
                    visible: dlg.code !== "port_in_use"
                    destructive: dlg.installFlow || dlg.startFlow   // starts or installs the sharing again
                    kind: "primary"
                    text: dlg.installFlow || dlg.stopFlow || dlg.startFlow ? qsTr("Try again") : qsTr("Back")
                    onClicked: {
                        if (dlg.stopFlow) { root.lh.dismissShareError(); root.lh.openShareDialog("stop") }
                        else if (dlg.startFlow) root.lh.shareAgain()
                        else if (dlg.installFlow && root.lh.canRetryShare) root.lh.retryShare()
                        else { root.lh.dismissShareError(); root.lh.openShareDialog("setup") }
                    }
                },
                UzButton { objectName: "shareErrorClose"; text: dlg.installFlow || dlg.stopFlow ? qsTr("Cancel") : qsTr("Close"); onClicked: root.lh.dismissShareError() }
            ]
        }
    }
    Component {
        id: stoppedC
        UzDialog {
            objectName: "shareStoppedDialog"
            shown: true
            label: qsTr("Stop sharing")
            title: qsTr("Sharing stopped")
            text: qsTr("On this PC. The accounts and saves of the others are kept.")
            onClosed: root.lh.acknowledgeStopped()
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("To delete their data for good: uzume-server reset (deletes everything, asks for the server name).")
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
            actions: [
                UzButton {
                    objectName: "shareAgainButton"
                    visible: root.lh.shareAgainAvailable
                    destructive: true
                    kind: "primary"
                    text: qsTr("Share again…")
                    onClicked: { root.lh.acknowledgeStopped(); root.lh.shareAgain() }
                },
                UzButton { objectName: "shareStoppedClose"; text: qsTr("Close"); onClicked: root.lh.acknowledgeStopped() }
            ]
        }
    }

    // ---------------------------------------------------------------- 8i-5 Stop sharing
    Component {
        id: stopC
        UzDialog {
            id: dlg
            objectName: "stopSharingDialog"
            shown: true
            property int waitingFor: -1
            label: qsTr("Server › Settings")
            title: qsTr("Stop sharing?")
            text: qsTr("Affects everyone on %1.").arg(root.player.hubName)
            onClosed: root.lh.closeShareDialog()
            Connections {
                target: root.player.admin
                function onConfirmationAnswered(id: int, ok: bool) {
                    if (id !== dlg.waitingFor) return
                    dlg.waitingFor = -1
                    if (ok) root.lh.stopSharing()
                }
            }
            Fact { title: qsTr("The others can't connect"); text: qsTr("Their apps show “Can't reach %1”.").arg(root.player.hubName) }
            Fact { title: qsTr("Their accounts and saves are kept"); text: qsTr("In C:\\ProgramData\\Uzume\\Server. They come back if you share again.") }
            Fact { title: qsTr("Your library and saves stay on this PC"); text: qsTr("Nothing to copy back.") }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Asks “Confirm it's you” and admin rights to remove the service.")
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: Theme.muted
            }
            actions: [UzButton { objectName: "stopSharingCancel"; text: qsTr("Cancel"); onClicked: root.lh.closeShareDialog() }]
            dangerActions: [
                UzButton {
                    objectName: "stopSharingConfirm"
                    kind: "danger"
                    text: qsTr("Stop sharing")
                    onClicked: dlg.waitingFor = root.player.admin.requireConfirmation()
                }
            ]
        }
    }
}
