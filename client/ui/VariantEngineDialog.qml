import QtQuick
import QtQuick.Layouts
import Uzume.App

// "Get the randomizer engine" (design 10d-1a before the download, 10d-2 while it downloads): the engine is a tool the server offers like
// cores. The first build asks before it downloads (Download and build); the download itself runs inside the build job
// (VariantsController), this dialog shows the facts and the progress and goes away when the engine is on this PC. Cancel stops the pending
// build (nothing is kept).
// objectNames: variantEngineDialog, variantEngineFacts, variantEngineProgress, variantEngineProgressText, variantEngineThen,
// variantEngineStart, variantEngineCancel.
UzDialog {
    id: root
    objectName: "variantEngineDialog"
    required property PlayerController player
    readonly property VariantsController vc: root.player.variants
    readonly property var eng: root.vc.engine
    property string variantName: ""     // the variant that waits for the engine
    property bool started: false        // "Download and build" was pressed
    // The person confirmed the download: the Randomize dialog creates the variant, whose build job downloads the engine.
    signal confirmed()
    signal cancelled()

    parent: Window.contentItem
    z: 120
    size: "lg"
    label: qsTr("Randomizer · engine")
    title: qsTr("Get the randomizer engine")
    text: qsTr("Building variants needs the randomizer engine. %1 offers it, like cores.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("The server"))
    readonly property bool downloading: root.started || root.eng.state === "downloading"
    readonly property string progressText: root.eng.total > 0
        ? qsTr("Downloading · %1 of %2").arg(root.eng.receivedText).arg(root.eng.totalText)
        : qsTr("Downloading…")

    function openFor(name: string) {
        root.variantName = name
        root.started = false
        root.open()
    }
    function finish() {
        root.started = false
        root.shown = false
    }
    // The engine arrived (or the build was stopped elsewhere): leave.
    Connections {
        target: root.vc
        function onEngineChanged() {
            if (!root.shown || !root.started) return
            if (root.eng.state === "ready") root.finish()
            else if (root.eng.state === "failed" || root.eng.state === "not_offered") root.started = false
        }
    }
    error: root.eng.state === "failed" ? qsTr("The engine could not be downloaded. Try again.") : ""

    ColumnLayout {
        objectName: "variantEngineFacts"
        Layout.fillWidth: true
        spacing: 0
        Repeater {
            model: [
                { k: qsTr("Version"), v: qsTr("%1 · the version %2 offers").arg(root.eng.version ?? "").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("the server")) },
                { k: qsTr("Size"), v: root.eng.sizeText !== "" ? qsTr("%1 · downloads once").arg(root.eng.sizeText) : qsTr("Downloads once") },
                { k: qsTr("From"), v: qsTr("%1 › Tools").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("Server")) },
                { k: qsTr("License"), v: (root.eng.license ?? "") !== "" ? qsTr("%1 · source").arg(root.eng.license) : qsTr("Open source") }
            ]
            delegate: Item {
                id: fact
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: Math.max(factValue.implicitHeight, 20) + 2 * Theme.space1
                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule2 }
                UzMono {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 88
                    elide: Text.ElideRight
                    text: fact.modelData.k
                    color: Theme.dim
                    font.pixelSize: Theme.roles.sectionLabel.size
                    font.letterSpacing: Theme.roles.sectionLabel.spacing
                    font.capitalization: Font.AllUppercase
                }
                UzLabel {
                    id: factValue
                    anchors.left: parent.left
                    anchors.leftMargin: 88 + Theme.space15
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.ink2
                    text: fact.modelData.v
                }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
    }
    ColumnLayout {
        visible: root.downloading
        Layout.fillWidth: true
        Layout.topMargin: Theme.space05
        spacing: Theme.space1
        UzProgress {
            objectName: "variantEngineProgress"
            Layout.fillWidth: true
            label: qsTr("Downloading the engine")
            value: (root.eng.percent ?? 0) / 100
        }
        UzMono {
            objectName: "variantEngineProgressText"
            Layout.fillWidth: true
            elide: Text.ElideRight
            font.pixelSize: Theme.roles.meta11.size - 1
            color: Theme.dim
            font.capitalization: Font.AllUppercase
            text: root.progressText
        }
        UzLabel {
            objectName: "variantEngineThen"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
            text: qsTr("Then %1 builds.").arg(root.variantName)
        }
    }
    actions: [
        UzButton {
            id: startButton
            objectName: "variantEngineStart"
            kind: "primary"
            text: root.downloading ? qsTr("Downloading…") : qsTr("Download and build")
            enabled: !root.downloading
            onClicked: { root.started = true; root.confirmed() }
        },
        UzButton {
            id: cancelButton
            objectName: "variantEngineCancel"
            text: qsTr("Cancel")
            onClicked: { root.started = false; root.shown = false; root.cancelled() }
        }
    ]
    onShownChanged: if (shown) Qt.callLater(() => (root.downloading ? cancelButton : startButton).forceActiveFocus())
}
