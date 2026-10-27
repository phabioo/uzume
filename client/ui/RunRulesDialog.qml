import QtQuick
import QtQuick.Layouts
import Uzume.App

// "Rules…" of a run (design 11f, creator or admin only): the four core rules in one dialog, saved as the whole object with
// PUT /runs/{id}/rules. Turning cheats off or allowing them is explained by one line under the rows instead of a second dialog.
// objectNames: runRulesDialog, runRulesWarning, runRulesSave, runRulesCancel (plus the rows of RunRulesFields).
UzDialog {
    id: root
    objectName: "runRulesDialog"
    required property RunsController runs
    property var saved: ({})            // rules of the run when the dialog opened
    property var draft: ({})
    property string runId: ""
    readonly property bool changed: root.draft.states !== root.saved.states || root.draft.rewind !== root.saved.rewind
                                    || root.draft.cheats !== root.saved.cheats || root.draft.memoryWrite !== root.saved.memoryWrite
    readonly property string warning: root.changed ? root.runs.rulesDraftWarning(root.draft) : ""

    size: "lg"
    label: qsTr("Runs · Rules")
    title: qsTr("Rules for %1").arg(root.runs.detail.name || "")
    error: root.runs.rulesError
    busy: root.runs.rulesBusy

    function openFor(runId: string, rules: var) {
        root.runId = runId
        root.saved = rules
        root.draft = Object.assign({}, rules)
        root.runs.clearRulesError()
        root.open()
    }
    Connections {
        target: root.runs
        function onRulesSaved(id) { if (id === root.runId) root.close() }
    }

    RunRulesFields {
        rules: root.draft
        onRuleChanged: (key, value) => {
            const d = Object.assign({}, root.draft)
            d[key] = value
            root.draft = d
        }
    }
    UzStatusIcon {
        objectName: "runRulesWarning"
        visible: root.warning !== ""
        kind: root.draft.cheats === "off" || root.draft.memoryWrite === "off" ? "warning" : "done"
        small: true
        text: root.warning
        maxWidth: 480
    }

    actions: [
        UzButton {
            objectName: "runRulesSave"
            kind: "primary"
            text: qsTr("Save rules")
            enabled: root.changed
            busy: root.runs.rulesBusy
            onClicked: root.runs.saveRunRules(root.runId, root.draft)
        },
        UzButton { objectName: "runRulesCancel"; text: qsTr("Cancel"); onClicked: root.close() }
    ]
}
