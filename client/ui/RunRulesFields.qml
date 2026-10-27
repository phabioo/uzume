import QtQuick
import QtQuick.Layouts
import Uzume.App

// The core rules of a run as four compact rows (design 11e / 11f): Save states and Rewind (toggles), Cheats and Memory changes
// (Off | Allowed, logged). Used by the Start dialog and the Rules dialog; it only shows `rules` and reports the change, the owner
// keeps the draft. rules = {states, rewind, cheats, memoryWrite} (RunsController.startCoreRules / detail.rules).
// objectNames: runRule_states, runRule_rewind, runRule_cheats, runRule_memoryWrite (the segments: runRuleCheatsOff / runRuleCheatsLogged,
// runRuleMemoryOff / runRuleMemoryLogged).
ColumnLayout {
    id: root
    property var rules: ({})
    property bool editable: true
    signal ruleChanged(string key, var value)
    spacing: Theme.space15
    Layout.fillWidth: true
    Layout.minimumWidth: 0

    function levelOptions(prefix: string): var {
        return [
            { value: "off", label: qsTr("Off"), name: prefix + "Off" },
            { value: "logged", label: qsTr("Allowed, logged"), name: prefix + "Logged" }
        ]
    }

    component RuleText: ColumnLayout {
        property string title
        property string hint
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 0
        Text {
            Layout.fillWidth: true
            text: parent.title
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.roles.fieldLabel.size
            font.weight: Font.Medium
            color: Theme.ink
        }
        Text {
            Layout.fillWidth: true
            text: parent.hint
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.muted
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space15
        RuleText { title: qsTr("Save states"); hint: qsTr("Players can keep and load save states while they play.") }
        UzToggle {
            objectName: "runRule_states"
            enabled: root.editable
            checked: root.rules.states === true
            Accessible.name: qsTr("Save states")
            onToggled: { root.ruleChanged("states", checked); checked = Qt.binding(function() { return root.rules.states === true }) }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space15
        RuleText { title: qsTr("Rewind"); hint: qsTr("Players can take back the last few seconds.") }
        UzToggle {
            objectName: "runRule_rewind"
            enabled: root.editable
            checked: root.rules.rewind === true
            Accessible.name: qsTr("Rewind")
            onToggled: { root.ruleChanged("rewind", checked); checked = Qt.binding(function() { return root.rules.rewind === true }) }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space15
        RuleText { title: qsTr("Cheats"); hint: qsTr("Allowed cheats show in the run log for everyone.") }
        UzSegmented {
            objectName: "runRule_cheats"
            enabled: root.editable
            accessibleName: qsTr("Cheats")
            options: root.levelOptions("runRuleCheats")
            current: root.rules.cheats ?? "off"
            onPicked: value => root.ruleChanged("cheats", value)
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space15
        RuleText { title: qsTr("Memory changes"); hint: qsTr("Plugins may change the game's memory. Every change is logged.") }
        UzSegmented {
            objectName: "runRule_memoryWrite"
            enabled: root.editable
            accessibleName: qsTr("Memory changes")
            options: root.levelOptions("runRuleMemory")
            current: root.rules.memoryWrite ?? "off"
            onPicked: value => root.ruleChanged("memoryWrite", value)
        }
    }
}
