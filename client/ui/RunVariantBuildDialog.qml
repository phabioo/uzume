import QtQuick
import QtQuick.Layouts
import Uzume.App

// "Building your variant" (randomizer.md 10f-3): Play run of a randomized run builds the person's copy of the game first. The dialog follows
// `player.runBuild` (the pending run start): label RUN · NAME, the three steps (reading the game file, randomizing, writing the variant;
// the current one carries the progress and the estimate), the checkbox "Start <run> when it's ready" (on by default), "Run in background"
// (hides the dialog, the build goes on; Esc does the same) and Cancel (stops the build, nothing is kept). A failed build shows its reason and a Close button.
// Spoilers: the dialog shows no creature, item or trainer names and no settings.
// objectNames: runBuildDialog, runBuildStep_<n> (reading | randomizing | writing), runBuildProgress, runBuildStart (checkbox),
// runBuildBackground, runBuildCancel, runBuildClose, runBuildFailed.
UzDialog {
    id: root
    objectName: "runBuildDialog"
    required property PlayerController player
    readonly property var b: root.player.runBuild
    readonly property bool failed: root.b.state === "failed"
    readonly property bool building: root.b.state === "building"
    readonly property int stepIndex: root.b.step === "writing" ? 2 : root.b.step === "randomizing" ? 1 : 0
    readonly property int percent: root.b.percent ?? 0

    size: "lg"
    parent: Window.contentItem
    z: 180
    label: qsTr("Run · %1").arg((root.b.runName ?? "").toUpperCase())
    title: root.failed ? qsTr("Your variant couldn’t be built") : qsTr("Building your variant")
    text: root.failed ? qsTr("Nothing was kept. Main is not touched.")
                      : root.b.gameTitle ? qsTr("Before your first start, this PC builds your copy of %1. Spoilers stay hidden.").arg(root.b.gameTitle)
                                         : qsTr("Before your first start, this PC builds your copy of the game. Spoilers stay hidden.")
    closable: false

    // The dialog is open while a start waits for its variant and it was not sent to the background.
    readonly property bool wanted: (root.building && root.b.background !== true) || root.failed
    onWantedChanged: {
        if (root.wanted) root.open()
        else if (root.shown) root.shown = false
    }
    Component.onCompleted: if (root.wanted) root.open()
    // Esc closes the dialog like "Run in background": the build goes on (Cancel is its own button, nothing is lost by accident).
    onClosed: {
        if (root.building && root.b.background !== true) root.player.runBuildInBackground()
        else if (root.failed) root.player.dismissRunBuild()
    }

    component Step: Item {
        id: st
        property int index: 0
        property string name: ""
        property string title: ""
        property string detail: ""
        readonly property bool done: root.stepIndex > st.index
        readonly property bool current: root.stepIndex === st.index
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        implicitHeight: stCol.implicitHeight + Theme.space2
        objectName: "runBuildStep_" + st.name
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
        Item {
            id: marker
            width: 20
            height: 20
            y: Theme.space1
            UzGlyph { anchors.centerIn: parent; visible: st.done; name: "check"; size: 16; color: Theme.ink }
            UzEnso { anchors.centerIn: parent; visible: st.current; size: 18 }
            Rectangle {
                anchors.centerIn: parent
                visible: !st.done && !st.current
                width: 10
                height: 10
                color: "transparent"
                border.width: 1
                border.color: Theme.dim
            }
        }
        ColumnLayout {
            id: stCol
            x: 20 + Theme.space15
            y: Theme.space1
            width: parent.width - x
            spacing: Theme.space05
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space1
                Text {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: st.title
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.input.size
                    font.weight: st.current ? Font.Medium : Font.Normal
                    color: st.current || st.done ? Theme.ink : Theme.muted
                }
                UzMono {
                    visible: st.detail !== ""
                    text: st.detail
                    elide: Text.ElideRight
                    Layout.maximumWidth: stCol.width * 0.6
                }
            }
            UzProgress {
                objectName: st.current && st.name === "randomizing" ? "runBuildProgress" : ""
                visible: st.current && st.name === "randomizing"
                Layout.fillWidth: true
                value: root.b.fraction ?? 0
            }
        }
    }

    ColumnLayout {
        visible: root.building
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 0
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
        Step { index: 0; name: "reading"; title: qsTr("Reading your game file"); detail: root.stepIndex > 0 ? qsTr("checked") : "" }
        Step {
            index: 1
            name: "randomizing"
            title: qsTr("Randomizing your copy")
            detail: root.stepIndex === 1 ? (root.percent > 0 ? (root.b.etaText ? qsTr("%1 % · %2").arg(root.percent).arg(String(root.b.etaText).toUpperCase())
                                                                                : qsTr("%1 %").arg(root.percent)) : "") : ""
        }
        Step { index: 2; name: "writing"; title: qsTr("Writing your variant") }
    }
    Text {
        objectName: "runBuildFailed"
        visible: root.failed
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: root.b.error ? qsTr("Reason: %1. Details hold no spoilers.").arg(root.b.error) : ""
        font.family: Theme.mono
        font.pixelSize: Theme.roles.small.size
        color: Theme.muted
    }
    UzCheckbox {
        objectName: "runBuildStart"
        visible: root.building
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        text: qsTr("Start %1 when it’s ready").arg(root.b.runName ?? "")
        checked: root.b.startWhenReady !== false
        onToggled: root.player.setRunBuildStartWhenReady(checked)
    }

    actions: [
        UzButton {
            objectName: "runBuildBackground"
            focus: true
            visible: root.building
            kind: "primary"
            text: qsTr("Run in background")
            onClicked: root.player.runBuildInBackground()
        },
        UzButton {
            objectName: "runBuildCancel"
            visible: root.building
            text: qsTr("Cancel")
            onClicked: root.player.cancelRunBuild()
        },
        UzButton {
            objectName: "runBuildClose"
            visible: root.failed
            kind: "primary"
            text: qsTr("Close")
            onClicked: root.player.dismissRunBuild()
        }
    ]
    hints: [
        UzGamepadHint { k: "A"; label: qsTr("Run in background"); visible: root.building }
    ]
}
