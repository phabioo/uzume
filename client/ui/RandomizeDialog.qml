import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Randomize dialog (design 10b-1, 10b-2, 10b-3, 10d-1b/c): name, preset (the presets the Randomizer plugin registered plus Custom with a
// settings string), a seed, the spoiler log switch, the save slot line and the engine line. "Build variant" creates the recipe on the
// server and builds the variant on this PC (VariantsController.build); the dialog stays open (busy) until the server accepted the
// recipe, then `created(variantId, small)` tells the page to show the build dialog (10c) or, for a small game, only the button state.
// States: name taken (10b-3c), string from a newer engine (10b-3b), game file not on this PC (10d-1c "Download and build"), engine not on
// this PC (the page asks first, 10d-1a, `engineNeeded`), server offers no engine (10d-1b).
// objectNames: randomizeDialog, randomizeName, randomizeNameError, randomizePreset, randomizePresetHint, randomizeSettings, randomizePaste,
// randomizeSettingsState, randomizeUseSeed, randomizeSeed, randomizeSpoiler, randomizeSlotLine, randomizeFileNote,
// randomizeEngineLine, randomizeEngineSource, randomizeNoEngine, randomizeOpenTools, randomizeBuild, randomizeCancel.
UzDialog {
    id: root
    objectName: "randomizeDialog"
    required property PlayerController player
    readonly property VariantsController vc: root.player.variants
    property string gameId: ""
    property string gameTitle: ""
    property string gameSystem: ""
    property bool awaiting: false        // the recipe is on its way to the server
    property string serverError: ""
    property int requestId: -1
    // The page shows the engine dialog (10d-1a) before the first build when the engine is not on this PC.
    signal engineNeeded()
    signal created(string variantId, bool small)

    size: "lg"
    parent: Window.contentItem
    z: 110
    label: qsTr("Randomizer · %1").arg(root.gameSystem.toUpperCase())
    title: qsTr("Randomize %1").arg(root.gameTitle)
    busy: root.awaiting

    readonly property var eng: root.vc.engine
    // registryRevision is part of the result on purpose (see GamePage.qml: an unused bare read is dropped by compiled bindings).
    readonly property var presets: root.vc.registryRevision < 0 || root.gameId === "" ? [] : root.vc.presets(root.gameId)
    readonly property bool custom: root.presetId === "custom"
    property string presetId: ""
    readonly property var presetOptions: root.presets.map(p => ({ value: p.id, label: p.custom ? qsTr("Custom") : p.label }))
    readonly property var presetNow: {
        for (const p of root.presets) if (p.id === root.presetId) return p
        return null
    }
    readonly property string nameText: nameField.text.trim()
    readonly property bool nameTaken: root.vc.variants === undefined ? false : root.nameText !== "" && root.vc.nameTaken(root.gameId, root.nameText)
    readonly property string nameError: root.nameTaken ? qsTr("You already have a variant called %1.").arg(root.nameText)
                                       : root.nameText.length > 60 ? qsTr("Names can have up to 60 characters.") : ""
    readonly property var check: root.custom ? root.vc.checkSettings(root.gameId, settingsField.text) : ({ state: "ok", valid: true, prefix: "" })
    readonly property bool seedOk: !seedBox.checked || /^[0-9]{1,18}$/.test(seedField.text)
    readonly property bool engineBlocked: root.eng.state === "not_offered" || root.eng.state === "no_server"
    readonly property bool engineMissing: root.eng.state === "not_on_pc" || root.eng.state === "failed"
    readonly property var file: root.vc.variants === undefined || root.gameId === "" ? ({ onPc: true, sizeText: "" }) : root.vc.gameFile(root.gameId)
    readonly property bool canBuild: root.nameText !== "" && root.nameError === "" && root.check.valid && root.seedOk && !root.engineBlocked && root.presetId !== ""

    function openFor(id: string, gameTitle: string, system: string) {
        root.gameId = id
        root.gameTitle = gameTitle
        root.gameSystem = system
        root.awaiting = false
        root.serverError = ""
        root.vc.setGame(id)
        nameField.text = root.vc.defaultName(id)
        root.presetId = root.presets.length > 0 ? root.presets[0].id : "custom"
        settingsField.text = ""
        seedBox.checked = false
        seedField.text = ""
        spoilerBox.checked = false
        root.open()
        nameField.forceActiveFocus()
    }
    function startBuild() {
        if (!root.canBuild || root.awaiting) return
        root.serverError = ""
        const r = root.vc.build(root.gameId, root.nameText, root.presetId, root.custom ? settingsField.text.trim() : "", seedBox.checked ? seedField.text : "",
                                seedBox.checked, spoilerBox.checked)
        if (!r.ok) {
            root.serverError = root.errorText(r.error)
            return
        }
        root.requestId = Math.round(r.requestId)
        root.awaiting = true
    }
    function submit() {
        if (!root.canBuild || root.awaiting) return
        if (root.engineMissing) root.engineNeeded()
        else root.startBuild()
    }
    function errorText(code: string): string {
        switch (code) {
        case "name_taken": return qsTr("You already have a variant called %1.").arg(root.nameText)
        case "invalid_name": return qsTr("Enter a name of up to 60 characters.")
        case "invalid_settings": return qsTr("The settings string is not valid.")
        case "invalid_seed": return qsTr("The seed is a whole number.")
        case "unsupported": return qsTr("%1 can't be randomized.").arg(root.gameTitle)
        case "engine_not_offered": return qsTr("The server does not offer the randomizer engine.")
        case "server_unreachable": return qsTr("The server can't be reached. Try again when it is back.")
        default: return qsTr("The variant could not be created.")
        }
    }
    Connections {
        target: root.vc
        function onVariantCreated(req, variantId) {
            if (!root.awaiting || Math.round(req) !== root.requestId) return
            root.awaiting = false
            const small = root.vc.smallGame(root.gameId)
            root.shown = false
            root.created(variantId, small)
        }
        function onCreateFailed(req, error) {
            if (!root.awaiting || Math.round(req) !== root.requestId) return
            root.awaiting = false
            root.serverError = root.errorText(error)
        }
    }
    function pasteFromClipboard() {
        clipProbe.text = ""
        clipProbe.paste()
        settingsField.text = clipProbe.text.trim()
        settingsField.forceActiveFocus()
    }
    TextEdit { id: clipProbe; visible: false }

    error: root.serverError
    Keys.onReturnPressed: (event) => { root.submit(); event.accepted = true }
    Keys.onEnterPressed: (event) => { root.submit(); event.accepted = true }

    // Name and Preset share a row.
    GridLayout {
        Layout.fillWidth: true
        columns: root.width >= 520 ? 2 : 1
        columnSpacing: Theme.space15
        rowSpacing: Theme.space15
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space05
            UzMono { text: qsTr("Name"); color: Theme.dim; font.pixelSize: Theme.roles.sectionLabel.size; font.letterSpacing: Theme.roles.sectionLabel.spacing; font.capitalization: Font.AllUppercase }
            UzField {
                id: nameField
                objectName: "randomizeName"
                Layout.fillWidth: true
                fill: Theme.ground
                maximumLength: 80
                invalid: root.nameError !== ""
                accessibleName: qsTr("Name")
                KeyNavigation.tab: presetSelect
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space05
            UzMono { text: qsTr("Preset"); color: Theme.dim; font.pixelSize: Theme.roles.sectionLabel.size; font.letterSpacing: Theme.roles.sectionLabel.spacing; font.capitalization: Font.AllUppercase }
            UzSelect {
                id: presetSelect
                objectName: "randomizePreset"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                implicitWidth: 100
                accessibleName: qsTr("Preset")
                model: root.presetOptions
                current: root.presetId
                onPicked: (value) => root.presetId = value
            }
        }
    }
    UzStatusIcon {
        objectName: "randomizeNameError"
        visible: root.nameError !== ""
        small: true
        kind: "error"
        text: root.nameError
        maxWidth: root.frame.width - 56
    }
    UzLabel {
        objectName: "randomizePresetHint"
        visible: !root.custom && root.presetNow !== null && root.presetNow.hint !== ""
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.roles.small.size
        color: Theme.muted
        text: root.presetNow !== null ? root.presetNow.hint : ""
    }
    UzLabel {
        visible: root.custom
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.roles.small.size
        color: Theme.muted
        text: qsTr("Paste a settings string")
    }

    // Custom: settings string, Paste and the validation line.
    ColumnLayout {
        objectName: "randomizeCustom"
        visible: root.custom
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space05
        UzMono { text: qsTr("Settings string"); color: Theme.dim; font.pixelSize: Theme.roles.sectionLabel.size; font.letterSpacing: Theme.roles.sectionLabel.spacing; font.capitalization: Font.AllUppercase }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space1
            UzField {
                id: settingsField
                objectName: "randomizeSettings"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                fill: Theme.ground
                mono: true
                maximumLength: 4096
                invalid: root.check.state === "invalid" || root.check.state === "other_version"
                accessibleName: qsTr("Settings string")
            }
            UzButton {
                objectName: "randomizePaste"
                text: qsTr("Paste")
                onClicked: root.pasteFromClipboard()
            }
        }
        UzStatusIcon {
            objectName: "randomizeSettingsState"
            visible: root.check.state !== "empty"
            small: true
            kind: root.check.valid ? "done" : "error"
            maxWidth: root.frame.width - 56
            text: root.check.state === "ok" ? qsTr("Valid · settings version %1").arg(root.check.prefix)
                  : root.check.state === "other_version" ? qsTr("This string is from a newer engine than %1 has.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("the server"))
                  : qsTr("This is not a settings string.")
        }
        UzMono {
            objectName: "randomizeSettingsVersion"
            visible: root.check.state === "ok" || root.check.state === "other_version"
            Layout.fillWidth: true
            elide: Text.ElideRight
            font.pixelSize: Theme.roles.meta11.size
            color: Theme.dim
            text: root.check.state === "ok" ? qsTr("Made with settings %1").arg(root.check.prefix)
                  : qsTr("String settings %1 · Server engine %2").arg(root.check.prefix).arg(root.eng.version ?? "")
        }
    }

    // Use a seed and Seed share a row.
    GridLayout {
        Layout.fillWidth: true
        columns: root.width >= 520 ? 2 : 1
        columnSpacing: Theme.space15
        rowSpacing: Theme.space1
        UzCheckbox {
            id: seedBox
            objectName: "randomizeUseSeed"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            text: qsTr("Use a seed")
            hint: seedBox.checked ? qsTr("The same settings string and seed build the same world on every PC.") : qsTr("Off: Uzume picks a new random seed.")
        }
        ColumnLayout {
            visible: seedBox.checked
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 1
            spacing: Theme.space05
            UzMono { text: qsTr("Seed"); color: Theme.dim; font.pixelSize: Theme.roles.sectionLabel.size; font.letterSpacing: Theme.roles.sectionLabel.spacing; font.capitalization: Font.AllUppercase }
            UzField {
                id: seedField
                objectName: "randomizeSeed"
                Layout.fillWidth: true
                fill: Theme.ground
                mono: true
                maximumLength: 18
                inputMethodHints: Qt.ImhDigitsOnly
                invalid: seedField.text !== "" && !root.seedOk
                accessibleName: qsTr("Seed")
            }
        }
    }
    UzCheckbox {
        id: spoilerBox
        objectName: "randomizeSpoiler"
        Layout.fillWidth: true
        text: qsTr("Keep a spoiler log")
        hint: qsTr("Off by default. Open it later from the variant’s menu; never shown in game.")
    }

    // SAVE SLOT line between two hairlines.
    ColumnLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 0
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.space1
            Layout.bottomMargin: Theme.space1
            spacing: Theme.space15
            UzMono { text: qsTr("Save slot"); color: Theme.dim; font.pixelSize: Theme.roles.sectionLabel.size; font.letterSpacing: Theme.roles.sectionLabel.spacing; font.capitalization: Font.AllUppercase }
            UzLabel {
                objectName: "randomizeSlotLine"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: 1
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.roles.small.size
                color: Theme.ink2
                text: qsTr("Plays in its own slot: %1. Main is not touched.").arg(root.nameText)
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
    }
    UzLabel {
        objectName: "randomizeFileNote"
        visible: !root.file.onPc
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.roles.small.size
        color: Theme.ink2
        text: root.file.sizeText !== "" ? qsTr("%1 isn’t on this PC. Uzume downloads it from %2 first · %3.").arg(root.gameTitle).arg(root.player.hubName !== "" ? root.player.hubName : qsTr("the server")).arg(root.file.sizeText)
                                         : qsTr("%1 isn’t on this PC. Uzume downloads it from %2 first.").arg(root.gameTitle).arg(root.player.hubName !== "" ? root.player.hubName : qsTr("the server"))
    }
    // The server offers no engine (10d-1b): admins get Open Tools.
    ColumnLayout {
        objectName: "randomizeNoEngine"
        visible: root.engineBlocked
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space05
        UzStatusIcon {
            small: true
            kind: "warning"
            maxWidth: root.frame.width - 56
            text: root.eng.state === "no_server" ? qsTr("%1 is offline and the engine isn’t on this PC.").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("The server"))
                                                 : qsTr("%1 doesn’t offer a randomizer").arg(root.player.hubName !== "" ? root.player.hubName : qsTr("The server"))
        }
        UzLabel {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
            text: root.eng.state === "no_server" ? qsTr("Try again when the server is back.") : qsTr("Ask an admin to turn on Offer to apps in Server › Tools.")
        }
        UzButton {
            objectName: "randomizeOpenTools"
            visible: root.player.isAdmin && root.eng.state === "not_offered"
            Layout.fillWidth: false
            size: "sm"
            text: qsTr("Open Tools")
            onClicked: { root.close(); root.player.showServerPage("tools") }
        }
    }
    // The engine line: 10 px Mono, the only place the engine's name appears.
    Flow {
        Layout.fillWidth: true
        visible: (root.eng.version ?? "") !== ""
        spacing: 0
        UzMono {
            objectName: "randomizeEngineLine"
            font.pixelSize: Theme.roles.meta11.size - 1
            color: Theme.dim
            text: "FVX " + (root.eng.version ?? "") + ((root.eng.license ?? "") !== "" ? " · " + root.eng.license : "") + ((root.eng.sourceUrl ?? "") !== "" ? " · " : "")
        }
        UzMono {
            objectName: "randomizeEngineSource"
            font.pixelSize: Theme.roles.meta11.size - 1
            color: sourceHover.hovered ? Theme.ink : Theme.muted
            font.underline: true
            text: qsTr("SOURCE")
            visible: (root.eng.sourceUrl ?? "") !== ""
            Accessible.role: Accessible.Link
            Accessible.name: qsTr("Engine source")
            HoverHandler { id: sourceHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: Qt.openUrlExternally(root.eng.sourceUrl) }
        }
    }

    actions: [
        UzButton {
            id: buildButton
            objectName: "randomizeBuild"
            kind: "primary"
            text: root.engineMissing || !root.file.onPc ? qsTr("Download and build") : qsTr("Build variant")
            enabled: root.canBuild && !root.awaiting
            busy: root.awaiting
            onClicked: root.submit()
        },
        UzButton {
            objectName: "randomizeCancel"
            text: qsTr("Cancel")
            onClicked: root.close()
        }
    ]
}
