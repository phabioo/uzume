import QtQuick
import QtQuick.Controls.Basic
import Uzume.App

// Window-wide notices of the variants (design 10c-2, 10c-3e, 10c-3f): the "<name> is ready" toast with Play (on any page, bottom left
// like the other toasts), a short error toast when a background build failed, and the confirm that asks before the window closes while a
// variant is building ("Quit and stop building <name>?", focus on Keep Uzume open). Main.qml calls allowClose() from onClosing.
// objectNames: variantToast, variantToastPlay (the action), variantQuitDialog, variantQuitConfirm, variantQuitKeep.
Item {
    id: root
    required property PlayerController player
    readonly property VariantsController vc: root.player.variants
    property bool quitConfirmed: false
    anchors.fill: parent
    z: 80

    function allowClose(): bool {
        if (!root.vc.building || root.quitConfirmed) return true
        quitDialog.open()
        return false
    }

    UzToast {
        id: toast
        objectName: "variantToast"
        property string variantId: ""
        property string message: ""
        property bool isError: false
        visible: message !== ""
        x: Theme.space3
        y: parent.height - height - Theme.space3 - (linkWaiting ? 56 : 0)
        readonly property bool linkWaiting: root.player.onboarding.linkWaiting
        kind: isError ? "error" : "done"
        text: message
        action: isError || variantId === "" ? "" : qsTr("Play")
        onActionTriggered: { const id = toast.variantId; toast.clear(); root.vc.play(id) }
        onCloseRequested: toast.clear()
        function clear() { message = ""; variantId = "" }
        function show(text, id, error) { message = text; variantId = id; isError = error; hideTimer.restart() }
        Timer { id: hideTimer; interval: 12000; onTriggered: toast.clear() }
    }
    Connections {
        target: root.vc
        function onToast(text: string, id: string) { toast.show(text, id, false) }
        function onBuildFinished(id: string, ok: bool, error: string) {
            if (ok || error === "cancelled") return
            const v = root.vc.variantMap(id)
            toast.show(qsTr("%1 couldn’t be built").arg(v.name ?? ""), id, true)
        }
    }

    UzDialog {
        id: quitDialog
        objectName: "variantQuitDialog"
        parent: Window.contentItem
        z: 140
        label: qsTr("Quit")
        title: qsTr("Quit and stop building %1?").arg(root.vc.current.name ?? "")
        text: qsTr("The build starts over next time.")
        actions: [
            UzButton { id: keepButton; objectName: "variantQuitKeep"; text: qsTr("Keep Uzume open"); onClicked: quitDialog.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "variantQuitConfirm"
                kind: "danger"
                destructive: true
                text: qsTr("Quit")
                onClicked: { quitDialog.shown = false; root.quitConfirmed = true; Window.window.close() }
            }
        ]
        onShownChanged: if (shown) Qt.callLater(() => keepButton.forceActiveFocus())
    }
}
