import QtQuick
import QtQuick.Layouts
import Uzume.App

// The "Randomizer" section of "Start a run" (randomizer.md 10e-1, 10e-2 a to f). Shown when the Randomizer plugin registered games
// (`variants.pluginRegistered()`): Segmented Off / Each person gets their own / Same world for everyone, then Preset (the presets the plugin
// registered for the game, plus Custom with a settings string) and "Allow own variants", the line that pins the engine version (the save slot
// line is the dialog's). States: d Home server offers no engine (the row stays, turned off, with the reason), e the game is not supported.
// The dialog reads `payload` when it starts the run: {mode, preset_id, settings, tool_version, allow_own} (mode "off" = an ordinary run).
// objectNames: runRandomizer (the section), runRandomizerMode (Segmented, or runRandomizerModeSelect when it does not fit), runRandomizerReason,
// runRandomizerPreset, runRandomizerSettings, runRandomizerOwn, runRandomizerPin, runRandomizerNote.
ColumnLayout {
    id: root
    objectName: "runRandomizer"
    required property VariantsController variants
    property string gameId: ""
    property string gameTitle: ""
    property string runName: ""
    property int rev: 0                    // bumped when the dialog opens: the registry changes without a signal

    property string mode: "off"
    property string presetId: ""
    property string settingsText: ""
    property bool allowOwn: false

    readonly property bool registered: root.rev >= 0 && root.variants.pluginRegistered()   // `rev`: the registry changes without a signal
    readonly property bool gameKnown: root.gameId !== ""
    readonly property bool supported: root.rev >= 0 && root.gameKnown && root.variants.supported(root.gameId)
    readonly property var engine: root.variants.engine
    // Home server offers an engine: its version pins the run.
    readonly property string engineVersion: root.engine.offline === true || root.engine.state === "not_offered" || root.engine.state === "no_server" ? "" : (root.engine.version ?? "")
    readonly property bool usable: root.supported && root.engineVersion !== ""
    readonly property bool on: root.usable && root.mode !== "off"
    readonly property var presets: root.rev >= 0 && root.supported ? root.variants.presets(root.gameId) : []
    readonly property bool custom: root.presetId === "custom"
    readonly property string effectiveSettings: root.on ? (root.custom ? root.settingsText.trim() : root.variants.presetSettings(root.presetId)) : ""
    // Shown (and taking room) when the plugin is there at all (the dialog hides it while it only invites).
    property bool hidden: false
    visible: root.registered && !root.hidden
    Layout.fillWidth: true
    Layout.minimumWidth: 0
    spacing: Theme.space1

    readonly property var payload: ({
        mode: root.on ? root.mode : "off",
        preset_id: root.on ? root.presetId : "",
        settings: root.effectiveSettings,
        tool_version: root.on ? root.engineVersion : "",
        allow_own: root.on && root.allowOwn
    })

    function reset() {
        root.mode = "off"
        root.allowOwn = false
        root.settingsText = ""
        root.rev++
        root.presetId = root.presets.length > 0 ? root.presets[0].id : ""
    }
    onPresetsChanged: if (root.presets.length > 0 && !root.presets.some(p => p.id === root.presetId)) root.presetId = root.presets[0].id

    readonly property var modeOptions: [
        { value: "off", label: qsTr("Off"), name: "runRandomizerModeOff" },
        { value: "each", label: qsTr("Each person gets their own"), name: "runRandomizerModeEach", disabled: !root.usable },
        { value: "same", label: qsTr("Same world for everyone"), name: "runRandomizerModeSame", disabled: !root.usable }
    ]

    component FieldLabel: Text {
        Layout.fillWidth: true
        textFormat: Text.PlainText
        elide: Text.ElideRight
        font.family: Theme.sans
        font.pixelSize: Theme.roles.fieldLabel.size
        font.weight: Font.Medium
        color: Theme.ink
    }
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

    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
    UzSectionLabel { Layout.fillWidth: true; Layout.topMargin: Theme.space05; text: qsTr("Randomizer") }

    // Reason the row is off (10e-2d, 10e-2e).
    Hint {
        objectName: "runRandomizerReason"
        visible: root.gameKnown && (!root.supported || root.engineVersion === "")
        color: Theme.ink2
        text: !root.supported ? (root.gameTitle !== "" ? qsTr("%1 can’t be randomized.").arg(root.gameTitle) : qsTr("This game can’t be randomized.")) : qsTr("Home server offers no randomizer. Admins: Server › Tools.")
    }
    Hint {
        visible: !root.gameKnown
        text: qsTr("Pick a game to see whether it can be randomized.")
    }

    // The three-part Segmented when it fits, a Select otherwise (long translations, wide fonts).
    UzSegmented {
        id: probe
        visible: false
        options: root.modeOptions
    }
    UzSegmented {
        id: modeSeg
        objectName: "runRandomizerMode"
        visible: root.gameKnown && probe.implicitWidth <= root.width
        accessibleName: qsTr("Randomizer")
        options: root.modeOptions
        current: root.usable ? root.mode : "off"
        enabled: root.usable
        onPicked: value => root.mode = value
    }
    UzSelect {
        objectName: "runRandomizerModeSelect"
        Layout.fillWidth: true
        visible: root.gameKnown && !modeSeg.visible
        accessibleName: qsTr("Randomizer")
        enabled: root.usable
        model: root.modeOptions
        current: root.usable ? root.mode : "off"
        onPicked: value => root.mode = value
    }

    // Preset and Allow own variants (hidden while Off).
    GridLayout {
        visible: root.on
        Layout.fillWidth: true
        columns: root.width >= 440 ? 2 : 1
        columnSpacing: Theme.space15
        rowSpacing: Theme.space1
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space05
            FieldLabel { text: qsTr("Preset") }
            UzSelect {
                objectName: "runRandomizerPreset"
                Layout.fillWidth: true
                accessibleName: qsTr("Preset")
                model: root.presets.map(p => ({ value: p.id, label: p.label, hint: p.hint }))
                current: root.presetId
                onPicked: value => root.presetId = value
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space05
            UzCheckbox {
                objectName: "runRandomizerOwn"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.topMargin: root.width >= 440 ? Theme.space2 : 0
                text: qsTr("Allow own variants")
                hint: qsTr("Only with the run’s engine version.")
                checked: root.allowOwn
                onToggled: root.allowOwn = checked
            }
        }
    }
    UzField {
        objectName: "runRandomizerSettings"
        visible: root.on && root.custom
        Layout.fillWidth: true
        fill: Theme.ground
        placeholderText: qsTr("Paste a settings string")
        maximumLength: 4096
        mono: true
        text: root.settingsText
        onTextChanged: if (text !== root.settingsText) root.settingsText = text
    }
    UzMono {
        objectName: "runRandomizerPin"
        visible: root.on
        Layout.fillWidth: true
        elide: Text.ElideRight
        text: qsTr("PINS FVX %1 FOR THIS RUN").arg(root.engineVersion)
    }
    Hint {
        objectName: "runRandomizerNote"
        visible: root.on
        text: root.mode === "same" ? qsTr("One seed for everyone. Uzume picks it at the start; nobody sees it, the run creator neither.")
                                   : qsTr("Each person’s app builds their copy before the first start. Spoilers stay hidden.")
    }
}
