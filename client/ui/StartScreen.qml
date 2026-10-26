import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Start screen (screen name "start", 0.18 package 7, frame.md 8a, 8b, 8c): the first start and every way onto a server in one
// full window without a sidebar. The page follows OnboardingController::phase (and flow); the two steps before the controller is
// involved (the "Where should it run?" choice and the Hub on this PC) are local to this screen. Esc is Back on every page,
// Ctrl+V anywhere in the window opens an invite link (Main.qml). Sign-in of another person on this PC (accounts add / password)
// is SwitchUserScreen.qml.
Rectangle {
    id: root
    objectName: "startScreen"
    required property PlayerController player
    readonly property var ob: player.onboarding
    readonly property var local: player.localHub
    property string localView: ""          // "" | "setup_where" | "setup_local" (steps before the controller is involved)
    readonly property string page: {
        switch (root.ob.phase) {
        case "idle": return root.localView === "setup_local" ? "local" : root.localView === "setup_where" ? "setupWhere" : "welcome"
        case "play": return "play"
        case "address": return "address"
        case "checking": return "checking"
        case "security": return "security"
        case "link_choice": return "linkChoice"
        case "link_ready": return "linkReady"
        case "invite": return "invite"
        case "sign_in": return "signIn"
        case "approval_wait":
        case "approval_denied":
        case "approval_expired": return "approval"
        case "setup_account": return "setupAccount"
        case "security_mismatch":
        case "link_member":
        case "offline":
        case "server_too_old":
        case "app_too_old":
        case "disabled":
        case "certificate_changed":
        case "setup_exists": return "message"
        default: return "welcome"
        }
    }
    readonly property Item currentPage: loader.item
    color: Theme.ground

    // ---- the pages that are stops or dead ends: copy and ways out
    readonly property var messageSpec: {
        const ph = root.ob.phase
        const name = root.ob.serverName
        const setup = root.ob.flow === "setup"
        const msg = root.ob.errorMessage
        const frame = { screenLabel: setup ? qsTr("Set up a server") : (root.ob.flow === "link" ? qsTr("Invite link") : qsTr("Join a server")),
                        openLabel: setup ? qsTr("UZUME · SET UP A SERVER") : (root.ob.flow === "link" ? qsTr("UZUME · INVITE LINK") : qsTr("UZUME · JOIN A SERVER")) }
        let spec = {}
        switch (ph) {
        case "security_mismatch":
            spec = { kind: "error", label: qsTr("Security check"), title: qsTr("Stop: the codes don't match"),
                     text: setup ? qsTr("The device shows a different security code. Nothing was set up.")
                                 : qsTr("Someone may be in between. Nothing was sent to the server."),
                     details: qsTr("The security code of the server is not the one you expected. If you typed the address yourself, check it. If someone sent you the link, ask them to send it again, or ask them to read the code shown on the server."),
                     actions: [{ name: "dismiss", text: qsTr("Don't connect"), primary: true }, { name: "details", text: qsTr("Details") }] }
            break
        case "link_member":
            spec = { kind: "info", label: qsTr("Invite link"), title: qsTr("You're already on %1").arg(root.ob.linkInfo.name || root.ob.linkInfo.address || ""),
                     text: qsTr("This server is already on this PC. Open it to continue."),
                     actions: [{ name: "open", text: qsTr("Open %1").arg(root.ob.linkInfo.name || root.ob.linkInfo.address || ""), primary: true },
                               { name: "cancel", text: qsTr("Not now") }] }
            break
        case "offline":
            spec = { kind: "error", label: qsTr("Server offline"), title: qsTr("No answer from the server"),
                     text: (msg !== "" ? msg : qsTr("No answer from %1 after 10 seconds.").arg(root.ob.serverAddress)) + " " + qsTr("Games on this PC still run."),
                     actions: [{ name: "retry", text: qsTr("Try again"), primary: true, busy: true }, { name: "back", text: qsTr("Change address") }] }
            break
        case "server_too_old":
            spec = { kind: "error", label: qsTr("Server too old"), title: qsTr("%1 needs an update").arg(name),
                     text: msg !== "" ? msg : qsTr("%1 runs %2, which is too old for this version of Uzume. An admin updates it in Server › Settings › Updates or with `uzume-server update`.").arg(name).arg(root.ob.serverVersion),
                     actions: [{ name: "back", text: qsTr("Back"), primary: true }] }
            break
        case "app_too_old":
            spec = { kind: "error", label: qsTr("Uzume too old"), title: qsTr("Update Uzume to continue"),
                     text: msg !== "" ? msg : qsTr("This version of Uzume is too old for %1. Update Uzume to continue.").arg(name),
                     actions: [{ name: "back", text: qsTr("Back"), primary: true }] }
            break
        case "disabled":
            spec = { kind: "error", label: name, title: qsTr("This account is turned off"),
                     text: msg !== "" ? msg : qsTr("This account has been turned off on %1. Your saves are kept.").arg(name),
                     actions: [{ name: "back", text: qsTr("Back"), primary: true }] }
            break
        case "certificate_changed":
            spec = { kind: "error", label: qsTr("Security check"), title: qsTr("The security certificate has changed"),
                     text: qsTr("The certificate of %1 is not the one this PC remembers. Nothing was sent. Ask the admin whether the server was moved or reinstalled.").arg(name),
                     actions: [{ name: "back", text: qsTr("Don't connect"), primary: true }] }
            break
        case "setup_exists":
            spec = { kind: "info", label: qsTr("Set up a server"), title: qsTr("%1 is already set up").arg(name),
                     text: qsTr("To start over, run `uzume-server reset` on the device. Or join it as a member."),
                     actions: [{ name: "join", text: qsTr("Join %1").arg(name), primary: true }, { name: "back", text: qsTr("Back") }] }
            break
        default:
            break
        }
        return Object.assign({}, frame, spec)
    }
    function messageAction(name: string) {
        const ph = root.ob.phase
        switch (name) {
        case "dismiss": root.ob.dismissMismatch(); break
        case "open": root.ob.openLinkedServer(); break
        case "cancel": root.ob.cancel(); break
        case "retry": root.ob.tryAgain(); break
        case "join": root.ob.joinSetUpServer(); break
        default:   // back
            if (ph === "security_mismatch") root.ob.dismissMismatch()
            else if (ph === "link_member") root.ob.cancel()
            else root.ob.back()
        }
    }
    function chose(what: string) {
        if (what === "play") root.ob.beginPlay()
        else if (what === "join") root.ob.beginJoin()
        else root.localView = "setup_where"
    }
    function chooseWhere(where: string) {
        if (where === "pc") {
            root.localView = "setup_local"
            root.local.start()
        } else {
            root.ob.beginSetup()
        }
    }

    Connections {
        target: root.local
        function onChanged() {
            if (root.localView === "setup_local" && root.local.phase === "idle") root.localView = "setup_where"
        }
        function onSetupCompleted() {
            root.localView = ""
            root.ob.cancel()
        }
    }
    Connections {
        target: root.ob
        function onChanged() {
            if (root.ob.phase === "idle" && !root.ob.firstStart && root.localView === "setup_where") root.localView = ""
        }
    }

    StartFrame {
        id: frame
        anchors.fill: parent
        screenLabel: root.currentPage ? root.currentPage.screenLabel : ""
        openLabel: root.currentPage ? root.currentPage.openLabel : "UZUME"
        openLines: ["GB · GBC · GBA · NDS · 3DS", qsTr("Version %1").arg(root.player.playerVersion), root.player.deviceName]
        hints: root.currentPage ? root.currentPage.hints : []
        contentMaxWidth: root.currentPage ? root.currentPage.maxWidth : 520
        Loader {
            id: loader
            Layout.fillWidth: true
            sourceComponent: root.page === "welcome" ? welcomeC
                           : root.page === "setupWhere" ? setupWhereC
                           : root.page === "local" ? localC
                           : root.page === "play" ? playC
                           : root.page === "address" ? addressC
                           : root.page === "checking" ? checkingC
                           : root.page === "security" ? securityC
                           : root.page === "linkChoice" ? linkChoiceC
                           : root.page === "linkReady" ? linkReadyC
                           : root.page === "invite" ? inviteC
                           : root.page === "signIn" ? signInC
                           : root.page === "approval" ? approvalC
                           : root.page === "setupAccount" ? setupAccountC
                           : messageC
            onLoaded: Qt.callLater(root.focusPage)
        }
    }
    function focusPage() {
        const f = loader.item ? loader.item.initialFocus : null
        if (f) f.forceActiveFocus()
        else root.forceActiveFocus()
    }
    Keys.onEscapePressed: (event) => {
        if (loader.item) { loader.item.backRequested(); event.accepted = true }
    }

    Component { id: welcomeC; StartWelcomePage { player: root.player; onChose: what => root.chose(what) } }
    Component { id: setupWhereC; StartSetupWherePage { player: root.player; onChose: where => root.chooseWhere(where); onLeave: root.localView = "" } }
    Component { id: localC; StartLocalHubPage { player: root.player; onLeave: root.localView = "setup_where" } }
    Component { id: playC; StartPlayPage { player: root.player } }
    Component { id: addressC; StartAddressPage { player: root.player } }
    Component { id: checkingC; StartCheckingPage { player: root.player } }
    Component { id: securityC; StartSecurityPage { player: root.player } }
    Component { id: linkChoiceC; StartLinkChoicePage { player: root.player } }
    Component { id: linkReadyC; StartLinkReadyPage { player: root.player } }
    Component { id: inviteC; StartInvitePage { player: root.player } }
    Component { id: signInC; StartSignInPage { player: root.player; onForgotRequested: forgot.open() } }
    Component { id: approvalC; StartApprovalPage { player: root.player } }
    Component { id: setupAccountC; StartSetupAccountPage { player: root.player } }
    Component { id: messageC; StartMessagePage { spec: root.messageSpec; onAction: name => root.messageAction(name) } }

    ForgotPasswordDialog { id: forgot; objectName: "forgotDialog"; player: root.player }
}
