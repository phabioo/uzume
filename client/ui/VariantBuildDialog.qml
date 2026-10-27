import QtQuick
import QtQuick.Layouts
import Uzume.App

// Building a variant (design 10c-1, 10c-3b): step list (Reading game file, Randomizing with the progress bar and the estimate, Writing
// <name>), the save slot line, "Run in background" (primary, focused) and Cancel, which asks first ("Stop building <name>?", focus on
// Keep building). The build runs in VariantsController; closing the dialog never stops it. The dialog closes by itself when the build
// ended; a failed build asks the page for the problem dialog (`failed`). Small games never open it (the page checks smallGame()).
// objectNames: variantBuildDialog, variantBuildStep_<reading|randomizing|writing>, variantBuildProgress, variantBuildEta,
// variantBuildSlot, variantBuildBackground, variantBuildCancel, variantBuildCancelConfirm, variantBuildStop, variantBuildKeep.
UzDialog {
    id: root
    objectName: "variantBuildDialog"
    required property PlayerController player
    readonly property VariantsController vc: root.player.variants
    property string variantId: ""
    property string variantName: ""
    property string gameTitle: ""
    property string fileText: ""        // "1.8 GB · checked" once the file was read
    signal failed(string variantId, string error)

    parent: Window.contentItem
    z: 110
    size: "lg"
    closable: false
    label: qsTr("Randomizer · %1").arg(root.gameTitle)
    title: qsTr("Building %1").arg(root.variantName)
    readonly property var cur: root.vc.current.variantId === root.variantId ? root.vc.current : root.vc.variantMap(root.variantId)
    readonly property int stepIndex: ({ reading: 0, randomizing: 1, writing: 2 })[root.cur.step ?? "reading"] ?? 0
    readonly property bool longBuild: !(root.cur.etaMs > 0 && (root.cur.etaMs + (root.cur.elapsedMs ?? 0)) < 60000)
    text: root.longBuild ? qsTr("%1 is a large game, so this takes a few minutes. You can keep using Uzume meanwhile.").arg(root.gameTitle)
                         : qsTr("You can keep using Uzume meanwhile.")

    function watch(id: string, name: string, gameTitle: string, fileText: string) {
        root.variantId = id
        root.variantName = name
        root.gameTitle = gameTitle
        root.fileText = fileText
        root.open()
    }
    function askCancel() { stopConfirm.open() }
    Keys.onEscapePressed: (event) => { root.askCancel(); event.accepted = true }

    Connections {
        target: root.vc
        function onBuildFinished(id: string, ok: bool, error: string) {
            if (id !== root.variantId) return
            root.shown = false
            stopConfirm.shown = false
            if (!ok && error !== "cancelled") root.failed(id, error)
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        Repeater {
            model: [
                { id: "reading", label: qsTr("Reading game file") },
                { id: "randomizing", label: qsTr("Randomizing") },
                { id: "writing", label: qsTr("Writing %1").arg(root.variantName) }
            ]
            delegate: Item {
                id: step
                required property var modelData
                required property int index
                readonly property bool done: index < root.stepIndex
                readonly property bool active: index === root.stepIndex
                objectName: "variantBuildStep_" + modelData.id
                Layout.fillWidth: true
                implicitHeight: stepCol.implicitHeight + 2 * Theme.space15 - 2
                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule2 }
                Item {
                    id: marker
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.topMargin: Theme.space1
                    width: 20
                    height: 20
                    UzStatusIcon { visible: step.done; anchors.centerIn: parent; kind: "done" }
                    UzEnso { visible: step.active; anchors.centerIn: parent; size: 20 }
                    Rectangle {
                        visible: !step.done && !step.active
                        anchors.centerIn: parent
                        width: 10
                        height: 10
                        color: "transparent"
                        border.width: 1.5
                        border.color: Theme.disabledInk
                    }
                }
                ColumnLayout {
                    id: stepCol
                    anchors.left: marker.right
                    anchors.leftMargin: Theme.space15
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: Theme.space1
                    spacing: Theme.space05
                    UzLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.roles.input.size
                        color: step.done || step.active ? Theme.ink : Theme.dim
                        text: step.modelData.label
                    }
                    UzMono {
                        visible: step.done && step.index === 0 && root.fileText !== ""
                        Layout.fillWidth: true
                        font.pixelSize: Theme.roles.meta11.size - 1
                        color: Theme.dim
                        text: root.fileText
                    }
                    UzProgress {
                        objectName: step.index === 1 ? "variantBuildProgress" : ""
                        visible: step.active && step.index === 1
                        Layout.fillWidth: true
                        value: root.cur.fraction ?? 0
                    }
                    UzMono {
                        objectName: step.index === 1 ? "variantBuildEta" : ""
                        visible: step.active && step.index === 1
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.pixelSize: Theme.roles.meta11.size - 1
                        color: Theme.dim
                        font.capitalization: Font.AllUppercase
                        text: (root.cur.percent ?? 0) + " %" + ((root.cur.etaText ?? "") !== "" ? " · " + root.cur.etaText : "")
                    }
                }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space15
        UzMono { text: qsTr("Save slot"); color: Theme.dim; font.pixelSize: Theme.roles.sectionLabel.size; font.letterSpacing: Theme.roles.sectionLabel.spacing; font.capitalization: Font.AllUppercase }
        UzLabel {
            objectName: "variantBuildSlot"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 1
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.ink2
            text: qsTr("Plays in its own slot: %1. Main is not touched.").arg(root.variantName)
        }
    }
    actions: [
        UzButton {
            id: backgroundButton
            objectName: "variantBuildBackground"
            kind: "primary"
            text: qsTr("Run in background")
            onClicked: root.shown = false
        },
        UzButton {
            objectName: "variantBuildCancel"
            text: qsTr("Cancel")
            onClicked: root.askCancel()
        }
    ]
    onShownChanged: if (shown) Qt.callLater(() => backgroundButton.forceActiveFocus())

    // 10c-3b: Stop building? Focus starts on Keep building.
    UzDialog {
        id: stopConfirm
        objectName: "variantBuildCancelConfirm"
        parent: Window.contentItem
        z: 130
        label: qsTr("Randomizer · %1").arg(root.gameTitle)
        title: qsTr("Stop building %1?").arg(root.variantName)
        text: qsTr("Nothing is kept. Main is not touched.")
        actions: [
            UzButton { id: keepButton; objectName: "variantBuildKeep"; text: qsTr("Keep building"); onClicked: stopConfirm.close() }
        ]
        dangerActions: [
            UzButton {
                objectName: "variantBuildStop"
                kind: "danger"
                destructive: true
                text: qsTr("Stop building")
                onClicked: { stopConfirm.shown = false; root.vc.cancel(root.variantId) }
            }
        ]
        onShownChanged: if (shown) Qt.callLater(() => keepButton.forceActiveFocus())
    }
}
