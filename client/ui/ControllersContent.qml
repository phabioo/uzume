import QtQuick
import QtQuick.Layouts
import Uzume.App

// Controls section of Settings (was the Controllers screen): the connected controllers as tabs, then the profile and the mapping
// table of the selected one, with an input test. Profiles stay local on this device. The hotkeys are their own section
// (HotkeysContent). The page header and the scrolling belong to SettingsScreen; key presses reach handleKeyPressed() from there
// (mapping capture and input test).
ColumnLayout {
    id: root
    objectName: "controllersContent"
    required property PlayerController player
    readonly property var ctl: root.player.controllers
    readonly property var plugins: root.player.plugins
    readonly property bool isMouse: root.ctl.device.kind === "mouse"
    // Inputs the viewed system has (the mapping rows): the input test draws only these cells (NDS: no ZL / ZR). Empty = no rows yet, draw all.
    readonly property var testInputIds: root.ctl.rows.map(function (r) { return r.input })
    property bool renaming: false
    property bool confirmDelete: false
    // The input test is a column at the right of the page at 1440 px and wider (SettingsScreen sets `inputTestColumn` and shows
    // `inputTestComponent` there); below that it is a disclosure under the controller tabs.
    property bool inputTestColumn: false
    property bool inputTestOpen: false
    readonly property Component inputTestComponent: inputTestComp
    // Narrow main column: tighter mapping table.
    readonly property bool compact: root.width < 520
    readonly property int mapWidth: compact ? 130 : 170
    readonly property int mapMinWidth: compact ? 72 : 100
    readonly property int targetWidth: compact ? 56 : 80
    readonly property int actionWidth: compact ? 48 : 60
    // Buttons table (grid 14 | flex | 240 | 72): the mapping field and the action cell give way on a narrow column.
    readonly property bool tight: root.width < 480
    readonly property int padActionWidth: tight ? 96 : 220
    spacing: Theme.space2

    // Tab labels are the controller names; a second pad with the same name gets a number.
    readonly property var deviceList: root.ctl.devices
    readonly property var tabLabels: {
        const seen = {}
        const total = {}
        for (const d of root.deviceList) total[d.name] = (total[d.name] || 0) + 1
        return root.deviceList.map(function (d) {
            seen[d.name] = (seen[d.name] || 0) + 1
            return total[d.name] > 1 ? d.name + " " + seen[d.name] : d.name
        })
    }
    readonly property string activeTab: {
        for (let i = 0; i < root.deviceList.length; ++i) {
            if (root.deviceList[i].key === root.ctl.selectedDevice) return root.tabLabels[i]
        }
        return ""
    }
    function pickTab(label: string) {
        const i = root.tabLabels.indexOf(label)
        if (i >= 0) root.ctl.selectDevice(root.deviceList[i].key)
    }

    // A key press: capture of a binding (mapping field), else it feeds the input test. True when it was taken.
    function handleKeyPressed(event): bool {
        if (root.renaming) return false
        if (root.ctl.captureKey(event.key)) return true
        if (!event.isAutoRepeat) root.ctl.testKey(event.key, true)
        return false
    }
    function handleKeyReleased(event) {
        if (!event.isAutoRepeat) root.ctl.testKey(event.key, false)
    }
    function cancelCaptures() {
        root.ctl.cancelCapture()
        root.ctl.cancelHotkeyCapture()
    }

    Connections {
        target: root.ctl
        function onSelectionChanged() { root.renaming = false; root.confirmDelete = false }
        function onProfilesChanged() { root.confirmDelete = false }
    }

    // Controller tabs (board 9a-1) with "Reset mapping" at the right while something differs from the defaults.
    RowLayout {
        id: tabRow
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space2
        UzTabs {
            objectName: "controllerTabs"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            size: "lg"
            items: root.tabLabels
            active: root.activeTab
            onPicked: label => root.pickTab(label)
        }
        UzButton {
            objectName: "resetChanged"
            visible: root.ctl.changedCount > 0 && !root.isMouse && !root.ctl.profileBuiltin
            kind: "ghost"
            size: "sm"
            text: qsTr("Reset mapping")
            onClicked: root.ctl.resetProfile()
        }
    }

    // The selected controller: slot, connection and the profile it uses (what the controller column showed).
    Repeater {
        model: root.deviceList
        delegate: RowLayout {
            id: dev
            required property var modelData
            objectName: "deviceInfo_" + modelData.key
            visible: modelData.key === root.ctl.selectedDevice
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.space2
            UzLabel {
                objectName: "deviceStatus_" + dev.modelData.key
                Layout.minimumWidth: 0
                text: (dev.modelData.connected ? "● " : "") + dev.modelData.status
                      + (dev.modelData.profile !== "" ? " · " + dev.modelData.profile : "")
                color: dev.modelData.connected ? Theme.ok : Theme.dim
                font.pixelSize: Theme.roles.small.size
                elide: Text.ElideRight
            }
            UzMono {
                objectName: "deviceSlot_" + dev.modelData.key
                text: dev.modelData.slot
                color: Theme.dim
                font.pixelSize: Theme.roles.small.size
            }
            Item { Layout.fillWidth: true }
        }
    }
    UzLabel {
        objectName: "profilesLocalFooter"
        Layout.fillWidth: true
        text: qsTr("Mapping is saved per controller on this PC.")
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
        wrapMode: Text.WordWrap
    }
    UzLabel {
        visible: !root.ctl.gamepadAvailable
        objectName: "gamepadNote"
        Layout.fillWidth: true
        text: root.ctl.gamepadNote
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
        wrapMode: Text.WordWrap
    }
    UzLabel {
        visible: root.ctl.gamepadAvailable && root.ctl.devices.length <= 2
        objectName: "noGamepadHint"
        Layout.fillWidth: true
        text: qsTr("No gamepad connected. Connect one and it appears here (the first gamepad is P1).")
        color: Theme.dim
        font.pixelSize: Theme.roles.small.size
        wrapMode: Text.WordWrap
    }

    // Input test below 1440 px: a disclosure under the controller tabs (the column takes over at 1440 px).
    ColumnLayout {
        objectName: "inputTestDisclosure"
        visible: !root.inputTestColumn
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space1
        UzDisclosure {
            objectName: "inputTestToggle"
            title: qsTr("Input test")
            onOpenChanged: root.inputTestOpen = open
            Component.onCompleted: open = root.inputTestOpen
        }
        Loader {
            objectName: "inputTestLoader"
            active: root.inputTestOpen && !root.inputTestColumn
            visible: active
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            sourceComponent: inputTestComp
        }
    }

    // Selects row (board 9a-1): Profile, System, and the button labels of this gamepad.
    Flow {
        id: selects
        objectName: "headerSelects"
        visible: !root.isMouse
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space15
        // Button labels: glyph set of this gamepad (Auto = detected type); saved per physical device.
        ColumnLayout {
            visible: root.ctl.device.kind === "gamepad"
            width: Math.min(180, selects.width)
            spacing: Theme.space05
            UzLabel { text: qsTr("Button labels"); color: Theme.muted; font.pixelSize: Theme.roles.fieldLabel.size; font.weight: Theme.roles.fieldLabel.weight }
            UzSelect {
                objectName: "labelSetSelect"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                model: root.ctl.labelChoices
                current: root.ctl.labelChoice
                onPicked: value => root.ctl.setLabelChoice(value)
            }
        }
        ColumnLayout {
            width: Math.min(200, selects.width)
            spacing: Theme.space05
            UzLabel { text: qsTr("Profile"); color: Theme.muted; font.pixelSize: Theme.roles.fieldLabel.size; font.weight: Theme.roles.fieldLabel.weight }
            UzSelect {
                objectName: "profileSelect"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                model: root.ctl.profiles
                current: root.ctl.profileId
                onPicked: value => root.ctl.selectProfile(value)
            }
        }
        // System selector (3f-4, P33): the mapping is listed per system, so the DS shows no ZL / ZR and GB / GBA are clear
        ColumnLayout {
            objectName: "systemSelector"
            visible: root.ctl.systems.length > 1
            width: Math.min(160, selects.width)
            spacing: Theme.space05
            UzLabel { text: qsTr("System"); color: Theme.muted; font.pixelSize: Theme.roles.fieldLabel.size; font.weight: Theme.roles.fieldLabel.weight }
            UzSelect {
                objectName: "systemSelect"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                model: root.ctl.systems.map(function (sys) { return { value: sys.id, label: sys.label } })
                current: root.ctl.viewSystem
                onPicked: value => root.ctl.selectSystem(value)
            }
        }
    }
    UzLabel {
        objectName: "labelSetHelp"
        visible: root.ctl.device.kind === "gamepad"
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.minimumWidth: 0
        Layout.topMargin: -Theme.space1
        text: qsTr("Auto uses the detected controller type. Saved per device.")
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
        wrapMode: Text.WordWrap
    }
    UzLabel {
        objectName: "systemNote"
        visible: !root.isMouse && root.ctl.systems.length > 1
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.minimumWidth: 0
        Layout.topMargin: -Theme.space1
        text: qsTr("Shows the mapping of this system. The game uses the mapping of its own system.")
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
        wrapMode: Text.WordWrap
    }

    // Mouse: touch input (fixed)
    UzLabel {
        visible: root.isMouse
        objectName: "mouseNote"
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.minimumWidth: 0
        text: qsTr("The mouse is the %1 input: move it over the touch screen, left mouse button = touch. This mapping is fixed and has no profile.").arg(root.ctl.touchLabel)
        color: Theme.muted
        wrapMode: Text.WordWrap
    }

    RowLayout {
        visible: !root.isMouse && root.ctl.profileBuiltin
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space15
        UzLabel {
            objectName: "builtinNote"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: qsTr("Built-in profiles are read-only. Duplicate the profile to change the mapping.")
            color: Theme.muted
            font.pixelSize: Theme.roles.small.size
            wrapMode: Text.WordWrap
        }
        UzButton {
            objectName: "duplicateToEditButton"
            size: "sm"
            text: qsTr("Duplicate to edit")
            onClicked: root.ctl.duplicateProfile()
        }
    }

    // Mapping table: grid 14 | flex | 240 | 72 (dot | DS input | mapping | Reset / Default)
    ColumnLayout {
        visible: !root.isMouse
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.bottomMargin: 6
            spacing: Theme.space15
            Item { Layout.preferredWidth: 14; Layout.minimumWidth: 14; Layout.maximumWidth: 14 }
            TableHeadCell {
                Layout.fillWidth: true; Layout.preferredWidth: 90; Layout.minimumWidth: 0
                text: qsTr("Game button")
            }
            TableHeadCell { Layout.fillWidth: true; Layout.preferredWidth: root.tight ? 190 : 240; Layout.minimumWidth: 120; Layout.maximumWidth: 240; text: root.ctl.device.kind === "keyboard" ? qsTr("Keyboard") : qsTr("Gamepad") }
            Item { Layout.preferredWidth: root.padActionWidth; Layout.minimumWidth: root.padActionWidth; Layout.maximumWidth: root.padActionWidth }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }

        Repeater {
            model: root.ctl.rows
            delegate: ColumnLayout {
                id: mrow
                required property var modelData
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 0
                Rectangle {
                    id: mbox
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: Math.max(40, mline.implicitHeight + Theme.space15)
                    color: mrow.modelData.listening ? Theme.selected : "transparent"
                    Rectangle { visible: mrow.modelData.listening; width: 2; height: parent.height; color: Theme.accent }
                    RowLayout {
                        id: mline
                        objectName: "mapRow_" + mrow.modelData.input
                        anchors.fill: parent
                        spacing: Theme.space15
                        Item {
                            Layout.preferredWidth: 14
                            Layout.minimumWidth: 14
                            Layout.maximumWidth: 14
                            Layout.preferredHeight: 14
                            Rectangle {
                                objectName: "changedDot_" + mrow.modelData.input
                                visible: mrow.modelData.changed
                                anchors.centerIn: parent
                                width: 6
                                height: 6
                                radius: 0
                                color: Theme.accent
                            }
                        }
                        UzLabel {
                            objectName: "mapLabel_" + mrow.modelData.input
                            Layout.fillWidth: true
                            Layout.preferredWidth: 90
                            Layout.minimumWidth: 0
                            text: mrow.modelData.label
                            elide: Text.ElideRight
                        }
                        UzCaptureField {
                            id: field
                            capturing: mrow.modelData.listening; accessibleName: qsTr("Binding for %1").arg(mrow.modelData.label !== undefined ? mrow.modelData.label : mrow.modelData.input)
                            objectName: "mapField_" + mrow.modelData.input
                            Layout.fillWidth: true
                            Layout.preferredWidth: root.tight ? 190 : 240
                            Layout.minimumWidth: 120
                            Layout.maximumWidth: 240
                            implicitHeight: 32
                            color: "transparent"
                            border.width: 1
                            border.color: mrow.modelData.listening ? Theme.accent : (field.activeFocus ? Theme.ink : Theme.rule2)
                            opacity: root.ctl.profileBuiltin ? 0.85 : 1
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 5
                                anchors.rightMargin: 6
                                spacing: 6
                                Row {
                                    objectName: "mapGlyphs_" + mrow.modelData.input
                                    visible: !mrow.modelData.listening && mrow.modelData.glyphs.length > 0
                                    Layout.minimumWidth: 0
                                    Layout.maximumWidth: field.width * 0.7
                                    clip: true
                                    spacing: 4
                                    Repeater {
                                        model: mrow.modelData.glyphs
                                        delegate: PadGlyph {
                                            required property string modelData
                                            size: 22
                                            g: modelData
                                        }
                                    }
                                }
                                BindingText {
                                    id: mapName
                                    objectName: "mapText_" + mrow.modelData.input
                                    // The key is shown once: a name that only repeats its single key frame ("[A] A") is left out.
                                    readonly property bool repeatsKey: !mrow.modelData.listening && mrow.modelData.glyphs.length === 1
                                                                       && String(mrow.modelData.glyphs[0]).toLowerCase() === String(mrow.modelData.bindingName).toLowerCase()
                                    visible: !repeatsKey
                                    caps: root.ctl.device.kind === "keyboard" && mrow.modelData.mapped && !mrow.modelData.listening
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.leftMargin: mrow.modelData.mapped || mrow.modelData.listening ? 0 : 5
                                    text: mrow.modelData.listening
                                          ? (root.ctl.device.kind === "keyboard" ? qsTr("Press a key…") : qsTr("Press a button…"))
                                          : mrow.modelData.bindingName
                                    color: mrow.modelData.listening ? Theme.accentText
                                           : (mrow.modelData.mapped ? Theme.ink2 : Theme.dim)
                                    font.pixelSize: Theme.roles.input.size
                                    elide: Text.ElideRight
                                }
                                Item { Layout.fillWidth: true; Layout.minimumWidth: 0; visible: !mapName.visible }   // keeps the clear button at the right edge
                                UzKbd {
                                    visible: mrow.modelData.listening && field.width >= 200
                                    keys: "Esc"
                                    label: qsTr("Cancel")
                                }
                                UzButton {
                                    visible: mrow.modelData.mapped && !root.ctl.profileBuiltin && !mrow.modelData.listening
                                    objectName: "mapClear_" + mrow.modelData.input
                                    kind: "link"
                                    text: "×"
                                    onClicked: root.ctl.clearBinding(mrow.modelData.input)
                                }
                            }
                                // Built-in profile: duplicate to a user profile first, then capture on the copy.
                            onActivated: root.ctl.profileBuiltin ? root.ctl.duplicateAndCapture(mrow.modelData.input)
                                : mrow.modelData.listening ? root.ctl.cancelCapture() : root.ctl.beginCapture(mrow.modelData.input)
                        }
                        RowLayout {
                            objectName: "mapAction_" + mrow.modelData.input
                            Layout.preferredWidth: root.padActionWidth
                            Layout.minimumWidth: root.padActionWidth
                            Layout.maximumWidth: root.padActionWidth
                            UzLabel {
                                objectName: "mapDefault_" + mrow.modelData.input
                                visible: mrow.modelData.changed
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                elide: Text.ElideRight
                                text: qsTr("Default: %1").arg(mrow.modelData.defaultName)
                                font.pixelSize: Theme.roles.small.size
                                color: Theme.muted
                            }
                            UzButton {
                                objectName: "mapReset_" + mrow.modelData.input
                                Layout.minimumWidth: 0
                                visible: mrow.modelData.changed
                                kind: "link"
                                font.underline: true
                                font.pixelSize: Theme.roles.small.size
                                text: qsTr("Reset")
                                onClicked: root.ctl.resetBinding(mrow.modelData.input)
                            }
                            Item { Layout.fillWidth: true; visible: !mrow.modelData.changed }
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }
            }
        }

        // Lid: only for input profiles that support it (none of the nds profiles does yet).
        Loader {
            active: root.ctl.supportsLid
            Layout.fillWidth: true
            sourceComponent: RowLayout {
                spacing: Theme.space15
                Item { Layout.preferredWidth: 14 }
                UzLabel { Layout.fillWidth: true; Layout.preferredWidth: 90; Layout.minimumWidth: 0; text: qsTr("Close lid") }
                UzLabel { Layout.preferredWidth: root.tight ? 190 : 240; Layout.minimumWidth: 120; Layout.maximumWidth: 240; Layout.fillWidth: true; text: qsTr("Unassigned"); color: Theme.dim }
                Item { Layout.preferredWidth: root.padActionWidth }
            }
        }
    }

    // Profile actions
    Flow {
        visible: !root.isMouse
        Layout.fillWidth: true
        spacing: 10
        UzButton {
            objectName: "resetProfileButton"
                                        text: qsTr("Reset profile to default")
            enabled: !root.ctl.profileBuiltin
            onClicked: root.ctl.resetProfile()
        }
        UzButton {
            objectName: "duplicateProfileButton"
                                        text: qsTr("Duplicate profile")
            onClicked: root.ctl.duplicateProfile()
        }
        UzButton {
            objectName: "renameProfileButton"
                                        text: qsTr("Rename")
            enabled: !root.ctl.profileBuiltin
            onClicked: { root.renaming = true; renameField.text = root.ctl.profileName; renameField.forceActiveFocus(); renameField.selectAll() }
        }
        UzButton {
            objectName: "deleteProfileButton"
                                        text: root.confirmDelete ? qsTr("Confirm delete") : qsTr("Delete")
            enabled: !root.ctl.profileBuiltin
            onClicked: {
                if (root.confirmDelete) { root.ctl.deleteProfile() } else { root.confirmDelete = true }
            }
        }
    }
    Flow {
        visible: root.renaming && !root.isMouse
        Layout.fillWidth: true
        spacing: 10
        UzField {
            id: renameField
            objectName: "renameField"
            width: 240
                                        onAccepted: { root.ctl.renameProfile(text); root.renaming = false; root.forceActiveFocus() }
            Keys.onEscapePressed: { root.renaming = false; root.forceActiveFocus() }
        }
        UzButton {
            objectName: "renameSaveButton"
            kind: "primary"
            text: qsTr("Save")
            onClicked: { root.ctl.renameProfile(renameField.text); root.renaming = false; root.forceActiveFocus() }
        }
        UzButton { text: qsTr("Cancel"); onClicked: { root.renaming = false; root.forceActiveFocus() } }
    }

    // The input test (one instance at a time: the column or the disclosure).
    Component {
        id: inputTestComp
        ColumnLayout {
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                TableHeadCell { Layout.fillWidth: true; text: qsTr("Input test") }
                UzLabel {
                    objectName: "testLabelsNote"
                    visible: !root.isMouse
                    text: qsTr("Labels: %1").arg(root.ctl.labelSetName)
                    color: Theme.muted
                    font.pixelSize: Theme.roles.small.size
                }
            }
            UzLabel {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: root.ctl.device.kind === "keyboard" ? qsTr("Press keys. Active inputs light up.")
                      : root.isMouse ? qsTr("The mouse drives the %1 input.").arg(root.ctl.touchLabel)
                      : qsTr("Press buttons on the controller. Active inputs light up.")
                color: Theme.muted
                font.pixelSize: Theme.roles.input.size
                wrapMode: Text.WordWrap
            }
            // Controller-shaped grid: 7 x 5 cells of 36, gap 6; triggers and shoulders on top, back/start in the
            // middle, D-pad cross left, face diamond right. Glyphs follow the Button labels set.
            Item {
                // Fixed-width slot (panel width minus margins): whatever the text elsewhere in the column
                // measures, the layout cannot hand the grid a wider area to center in.
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: Theme.inputTestWidth - 48
                Layout.preferredHeight: padGrid.height
            Item {
                id: padGrid
                objectName: "inputTestGrid"
                readonly property int cell: 36
                readonly property int gap: 6
                anchors.horizontalCenter: parent.horizontalCenter
                width: 7 * cell + 6 * gap
                height: 5 * cell + 4 * gap
                Repeater {
                    model: root.ctl.testCells
                    delegate: PadGlyph {
                        id: tile
                        required property var modelData
                        objectName: "testTile_" + modelData.id
                        x: modelData.col * (padGrid.cell + padGrid.gap)
                        y: modelData.row * (padGrid.cell + padGrid.gap)
                        width: modelData.span * padGrid.cell + (modelData.span - 1) * padGrid.gap
                        size: padGrid.cell
                        g: modelData.glyph
                        square: modelData.square
                        active: modelData.active
                        visible: root.testInputIds.length === 0 || modelData.id === "" || root.testInputIds.indexOf(modelData.id) >= 0
                    }
                }
            }
            }

            TableHeadCell { visible: root.ctl.hasTouch; text: root.ctl.touchLabel; Layout.topMargin: 10; Layout.minimumWidth: 0; Layout.maximumWidth: Theme.inputTestWidth - 48 }
            Rectangle {
                objectName: "touchBox"
                visible: root.ctl.hasTouch
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                implicitHeight: 150
                color: "transparent"
                border.width: 1
                border.color: Theme.rule
                UzLabel {
                    anchors.centerIn: parent
                    text: qsTr("Mouse on bottom screen")
                    color: Theme.muted
                    font.pixelSize: Theme.roles.input.size
                }
            }
            UzLabel {
                visible: root.ctl.hasTouch
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                wrapMode: Text.WordWrap
                text: qsTr("Left mouse button = stylus")
                color: Theme.dim
                font.pixelSize: Theme.roles.small.size
            }
            Item { Layout.fillHeight: true }
        }
    }
}
