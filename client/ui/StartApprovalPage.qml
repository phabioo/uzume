import QtQuick
import QtQuick.Layouts
import Uzume.App

// 8b-5h waiting for approval, 8b-5i declined (and expired): the ensō, "Cancel request", "Ask again".
StartPage {
    id: root
    required property PlayerController player
    readonly property var ob: player.onboarding
    screenLabel: qsTr("Step 3 of 3 · %1").arg(ob.serverName)
    openLabel: qsTr("UZUME · SIGN IN · %1").arg(ob.serverName)
    hints: [{ kbd: "Esc", pad: "B", label: root.ob.phase === "approval_wait" ? qsTr("Cancel request") : qsTr("Back") }]
    onBackRequested: root.ob.phase === "approval_wait" ? ob.cancelApproval() : ob.back()

    StartHead {
        step: root.screenLabel
        number: 1
        label: root.ob.serverName
        meta: qsTr("3 of 3")
        title: root.ob.phase === "approval_wait" ? qsTr("Waiting for approval")
               : root.ob.phase === "approval_denied" ? qsTr("Request declined") : qsTr("Request expired")
        text: root.ob.phase === "approval_wait" ? qsTr("Ask an admin to approve this PC once.") : ""
    }
    StartApprovalBlock {
        prefix: "approval"
        mode: root.ob.phase === "approval_wait" ? "wait" : root.ob.phase === "approval_denied" ? "denied" : "expired"
        deviceName: root.player.deviceName
        platformText: root.player.platformText.split(" ")[0]
        serverName: root.ob.serverName
        onAsk: root.ob.requestApproval()
        onCancel: root.ob.cancelApproval()
        onBack: root.ob.back()
    }
}
