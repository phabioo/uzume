import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// "Start a run" (runs-and-server.md 4j-app-3; mode "start": name, game, rules = None or an installed run plugin with its rule
// settings, people to invite, the Randomizer section of randomizer.md 10e) and "Invite players" into an open run (mode "invite"). A UzDialog lg (design-system.md "Dialog").
UzDialog {
    id: root
    objectName: "runStartDialog"
    required property RunsController runs
    property string mode: "start"      // start | invite
    property string inviteRunId: ""
    property bool inviteWasSent: false
    readonly property bool inviting: mode === "invite"
    property string serverName: ""      // shown in the Randomizer section when the server offers no engine
    property bool pcOnly: false         // the server of this PC only: nobody to invite (8j-4)

    size: "lg"
    label: root.inviting ? qsTr("Runs · Invite") : qsTr("Runs · New run")
    title: root.inviting ? qsTr("Invite players") : qsTr("Start a run")
    error: root.runs.startError

    function openStart() {
        root.mode = "start"
        root.inviteWasSent = false
        nameField.text = ""
        game.selectedId = ""
        randomizer.reset()
        root.runs.beginStart()
        root.open()
        nameField.forceActiveFocus()
    }
    // "Start a run" from a game page: the game is already chosen.
    function openStartFor(gameId: string) {
        root.openStart()
        game.selectedId = gameId
    }
    function openInvite(runId: string) {
        root.mode = "invite"
        root.inviteRunId = runId
        root.inviteWasSent = false
        root.runs.beginStart()
        root.open()
    }
    onClosed: root.runs.cancelStart()
    Connections {
        target: root.runs
        function onRunCreated() { root.close() }
        function onStartStateChanged() {
            if (root.inviting && root.visible && !root.runs.startBusy && root.runs.startError === "" && root.inviteWasSent) {
                root.close()
            }
        }
    }

    // A field label (role field-label: Plex Sans 500 12).
    component FieldLabel: Text {
        Layout.fillWidth: true
        textFormat: Text.PlainText
        elide: Text.ElideRight
        font.family: Theme.sans
        font.pixelSize: Theme.roles.fieldLabel.size
        font.weight: Font.Medium
        color: Theme.ink
    }
    // Help text under a control (role small, muted).
    component Hint: Text {
        Layout.fillWidth: true
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
        color: Theme.muted
    }

    Flickable {
        id: scroller
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredHeight: Math.min(body.implicitHeight, Math.max(160, root.height - 340))
        contentWidth: width
        contentHeight: body.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: body
            width: scroller.width - Theme.space15
            spacing: Theme.space2

            // Name
            ColumnLayout {
                visible: !root.inviting
                Layout.fillWidth: true
                spacing: Theme.space05
                FieldLabel { text: qsTr("Name") }
                UzField {
                    id: nameField
                    objectName: "runNameField"
                    Layout.fillWidth: true
                    fill: Theme.ground
                    placeholderText: qsTr("e.g. Summer Duo")
                    maximumLength: 64
                    font.family: Qt.application.font.family
                }
            }

            // Game: the library games that fit the run
            ColumnLayout {
                visible: !root.inviting
                Layout.fillWidth: true
                spacing: Theme.space05
                FieldLabel { text: qsTr("Game") }
                GamePickList {
                    id: game
                    objectName: "runStartGames"
                    Layout.fillWidth: true
                    rows: root.runs.games
                }
            }

            // Rules: None or an installed run plugin
            ColumnLayout {
                visible: !root.inviting
                Layout.fillWidth: true
                spacing: Theme.space05
                FieldLabel { objectName: "runRulesTitle"; text: qsTr("Rules") }
                Hint {
                    objectName: "runPluginsNone"
                    visible: root.runs.startRulesOptions.length === 0
                    text: qsTr("Your server offers no plugin for runs yet.")
                }
                UzSelect {
                    id: rulesSelect
                    objectName: "runRulesSelect"
                    visible: root.runs.startRulesOptions.length > 0
                    Layout.fillWidth: true
                    model: root.runs.startRulesOptions
                    current: root.runs.startRulesCurrent
                    displayText: rulesSelect.currentIndex >= 0 ? rulesSelect.currentText : qsTr("Pick a plugin…")
                    onPicked: value => root.runs.selectStartRules(value)
                }
                Hint { objectName: "runRulesHint"; text: qsTr("Only installed run plugins are listed.") }
            }

            // What the picked plugin is and may do.
            Repeater {
                model: root.inviting ? null : root.runs.startPlugins
                delegate: ColumnLayout {
                    id: plugRow
                    required property var model
                    objectName: "runPlugin_" + plugRow.model.id
                    visible: plugRow.model.checked === true
                    Layout.fillWidth: true
                    spacing: Theme.space05
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space1
                        Hint {
                            Layout.fillWidth: true
                            visible: plugRow.model.description !== ""
                            text: plugRow.model.description
                        }
                        PluginOriginPill { objectName: "runPluginOrigin"; origin: plugRow.model.origin; text: plugRow.model.originText }
                    }
                    Hint {
                        visible: plugRow.model.permissionsText !== ""
                        text: qsTr("It will be able to: %1").arg(plugRow.model.permissionsText)
                    }
                }
            }
            Repeater {
                model: root.inviting ? null : root.runs.startIncluded
                delegate: Hint {
                    id: incRow
                    required property var model
                    objectName: "runIncluded_" + incRow.model.id
                    text: incRow.model.text
                }
            }
            UnverifiedNote {
                objectName: "runStartUnverified"
                visible: !root.inviting && root.runs.startUnverified
                text: root.runs.unverifiedWarning
                Layout.fillWidth: true
            }

            // Rule settings (e.g. the mode): only while a plugin is picked (groups by plugin; one flat model, no nested repeaters).
            Repeater {
                model: root.inviting ? null : root.runs.startRules
                delegate: ColumnLayout {
                    id: ruleRow
                    required property var model
                    readonly property bool isHeader: ruleRow.model.kind === "header"
                    readonly property bool stacked: ruleRow.model.widget !== "toggle"
                    Layout.fillWidth: true
                    spacing: Theme.space05
                    UzSectionLabel {
                        visible: ruleRow.isHeader
                        objectName: "runRulesHeader_" + ruleRow.model.pluginId
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.space05
                        text: ruleRow.model.title
                    }
                    // Toggle: text at the left, the switch at the right.
                    RowLayout {
                        visible: !ruleRow.isHeader && !ruleRow.stacked
                        Layout.fillWidth: true
                        spacing: Theme.space15
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 0
                            FieldLabel { text: ruleRow.model.label; wrapMode: Text.Wrap; elide: Text.ElideNone }
                            Hint { visible: ruleRow.model.hint !== ""; text: ruleRow.model.hint }
                        }
                        UzToggle {
                            visible: ruleRow.model.widget === "toggle"
                            objectName: "runRule_" + ruleRow.model.pluginId + "_" + ruleRow.model.key
                            checked: ruleRow.model.value === true
                            onToggled: {
                                root.runs.setStartRule(ruleRow.model.pluginId, ruleRow.model.key, checked)
                                checked = Qt.binding(function() { return ruleRow.model.value === true })
                            }
                        }
                    }
                    // Segments and select: label above, control, hint below.
                    ColumnLayout {
                        visible: !ruleRow.isHeader && ruleRow.stacked
                        Layout.fillWidth: true
                        spacing: Theme.space05
                        FieldLabel { text: ruleRow.model.label; wrapMode: Text.Wrap; elide: Text.ElideNone }
                        UzSegmented {
                            visible: ruleRow.model.widget === "segments"
                            objectName: "runRule_" + ruleRow.model.pluginId + "_" + ruleRow.model.key
                            options: ruleRow.model.options
                            current: String(ruleRow.model.value)
                            onPicked: value => root.runs.setStartRule(ruleRow.model.pluginId, ruleRow.model.key, value)
                        }
                        UzSelect {
                            visible: ruleRow.model.widget === "select"
                            objectName: "runRule_" + ruleRow.model.pluginId + "_" + ruleRow.model.key
                            Layout.fillWidth: true
                            model: ruleRow.model.options
                            current: String(ruleRow.model.value)
                            onPicked: value => root.runs.setStartRule(ruleRow.model.pluginId, ruleRow.model.key, value)
                        }
                        Hint { visible: ruleRow.model.hint !== ""; text: ruleRow.model.hint }
                    }
                }
            }

            // People
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space1
                FieldLabel { text: qsTr("People") }
                Hint {
                    objectName: "runStartNeedsServer"
                    visible: root.pcOnly
                    text: qsTr("Needs a server · Share with others or join one")
                }
                Hint { visible: !root.pcOnly && root.runs.startUsersLoading; text: qsTr("Loading players…") }
                Hint {
                    visible: !root.pcOnly && !root.runs.startUsersLoading && root.runs.startUsers.count === 0
                    text: qsTr("There is nobody else to invite yet.")
                }
                GridLayout {
                    Layout.fillWidth: true
                    columns: Math.max(1, Math.min(4, Math.floor(body.width / 150)))
                    columnSpacing: Theme.space15
                    rowSpacing: Theme.space1
                    Repeater {
                        model: root.runs.startUsers
                        delegate: UzCheckbox {
                            id: userRow
                            required property var model
                            objectName: "runUser_" + userRow.model.id
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            enabled: !root.pcOnly
                            text: userRow.model.name
                            hint: userRow.model.online ? qsTr("Online") : ""
                            checked: userRow.model.checked
                            onToggled: {
                                root.runs.toggleStartUser(userRow.model.id)
                                checked = Qt.binding(function() { return userRow.model.checked })
                            }
                        }
                    }
                }
                Hint { text: qsTr("1–8 people. Each plays their own copy. You are always in your own run.") }
            }

            // Randomizer (10e): only when the Randomizer plugin registered games.
            RunRandomizerSection {
                id: randomizer
                hidden: root.inviting
                variants: root.runs.variants
                serverName: root.serverName
                gameId: game.selectedId
                gameTitle: {
                    const g = root.runs.games
                    for (let i = 0; i < g.count; ++i) if (g.get(i).id === game.selectedId) return g.get(i).title
                    return ""
                }
                runName: nameField.text.trim()
            }
        }
    }

    // The save slot every person gets, between two hairlines.
    ColumnLayout {
        visible: !root.inviting
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space05
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
        UzSectionLabel { Layout.fillWidth: true; Layout.topMargin: Theme.space05; text: qsTr("Save slot") }
        Text {
            objectName: "runStartSlotNote"
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.space05
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: randomizer.on ? (nameField.text.trim() !== "" ? qsTr("Each person gets Run: %1. Main and their own variants are not touched.").arg(nameField.text.trim())
                                                                 : qsTr("Each person gets their own save slot for this run. Main and their own variants are not touched."))
                                : (nameField.text.trim() !== "" ? qsTr("Each person gets Run: %1. Main is not touched.").arg(nameField.text.trim())
                                                                 : qsTr("Each person gets their own save slot for this run. Main is not touched."))
            font.family: Theme.sans
            font.pixelSize: Theme.roles.input.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.ink2
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
    }

    actions: [
        UzButton {
            objectName: "runStartCancel"
            text: qsTr("Cancel")
            onClicked: root.close()
        },
        UzButton {
            objectName: "runStartConfirm"
            kind: "primary"
            text: root.inviting ? qsTr("Invite") : qsTr("Start run")
            busy: root.runs.startBusy
            onClicked: {
                if (root.inviting) {
                    root.inviteWasSent = true
                    root.runs.inviteSelected(root.inviteRunId)
                } else {
                    root.runs.startRun(nameField.text, game.selectedId, randomizer.payload)
                }
            }
        }
    ]
}
