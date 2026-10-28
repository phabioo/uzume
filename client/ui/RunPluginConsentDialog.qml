import QtQuick
import QtQuick.Layouts
import Uzume.App

// New permissions after a plugin update in a run (run_plugin_updates_v1): "Play run" asks before the run's plugins start for this person.
// Agreeing is remembered per run + plugin + version; declining (or closing) keeps the game from starting.
// objectNames: runPluginConsentDialog, runPluginConsentItem, runPluginConsentAgree, runPluginConsentDecline.
UzDialog {
    id: root
    objectName: "runPluginConsentDialog"
    required property RunsController runs
    readonly property var items: root.runs.pluginConsent.items || []

    size: "lg"
    label: qsTr("Runs · Plugins")
    title: qsTr("Updated plugins ask for more")
    text: qsTr("The plugins of this run were updated. Agree to start them with the new permissions.")

    Connections {
        target: root.runs
        function onPluginConsentChanged() {
            if ((root.runs.pluginConsent.runId || "") !== "") root.open()
            else root.close()
        }
    }
    Component.onCompleted: if ((root.runs.pluginConsent.runId || "") !== "") root.open()  // raised while another screen showed
    onClosed: if ((root.runs.pluginConsent.runId || "") !== "") root.runs.answerPluginConsent(false)

    Repeater {
        model: root.items
        UzLabel {
            required property var modelData
            objectName: "runPluginConsentItem"
            Layout.fillWidth: true
            text: modelData.text
            wrapMode: Text.WordWrap
        }
    }

    actions: [
        UzButton { objectName: "runPluginConsentDecline"; text: qsTr("Decline"); onClicked: root.runs.answerPluginConsent(false) },
        UzButton { objectName: "runPluginConsentAgree"; kind: "primary"; text: qsTr("Agree and play"); onClicked: root.runs.answerPluginConsent(true) }
    ]
}
