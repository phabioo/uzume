import QtQuick
import Uzume.App

// Global toast queue (ADR 0033 D2): the toasts of the NotificationCenter, at most three at a time, each for six seconds. In the main
// window it sits bottom left; the game view puts one into its own toast stack. Only the host of the current screen is visible.
Column {
    id: root
    required property PlayerController player
    property real maxToastWidth: 420
    objectName: "toastHost"
    spacing: Theme.space1

    // "WATCH" is the bell's label; a toast reads it as a link: "Watch".
    function linkText(label: string): string {
        return label === "" ? "" : label.charAt(0).toUpperCase() + label.slice(1).toLowerCase()
    }

    Repeater {
        model: root.player.notifications.toasts
        delegate: UzToast {
            id: toast
            required property var modelData
            objectName: "notificationToast"
            width: Math.min(implicitWidth, root.maxToastWidth)
            kind: modelData.kind === "server_problem" ? "warning" : "done"
            maxLines: 2
            text: modelData.text
            action: root.linkText(modelData.actionLabel)
            onActionTriggered: root.player.notifications.actToast(modelData.id)
            onCloseRequested: root.player.notifications.dismissToast(modelData.id)
        }
    }
}
