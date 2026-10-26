import QtQuick
import QtQuick.Layouts
import Uzume.App

// 3f: Controllers. Devices | profile and mapping table | input test. Profiles stay local on this device.
Rectangle {
    id: root
    required property PlayerController player
    readonly property var ctl: root.player.controllers
    readonly property var plugins: root.player.plugins
    readonly property bool isMouse: root.ctl.device.kind === "mouse"
    // Inputs the viewed system has (the mapping rows): the input test draws only these cells (NDS: no ZL / ZR). Empty = no rows yet, draw all.
    readonly property var testInputIds: root.ctl.rows.map(function (r) { return r.input })
    property bool renaming: false
    property bool confirmDelete: false
    property string tab: "buttons"  // "buttons" | "hotkeys" (hotkeys are global, independent of the selected device)
    readonly property bool hotkeysTab: root.tab === "hotkeys"
    // The input test is a fourth column only at 1440 px and wider (Theme.columnsCompactMax); below it is a disclosure.
    readonly property bool inputTestColumn: root.width >= Theme.columnsCompactMax
    property bool inputTestOpen: false
    // Narrow main column (input test open on a small window): tighter mapping table.
    readonly property bool compact: content.width < 520
    readonly property int mapWidth: compact ? 130 : 170
    readonly property int mapMinWidth: compact ? 72 : 100
    readonly property int targetWidth: compact ? 56 : 80
    readonly property int actionWidth: compact ? 48 : 60
    // Header: selects beside the title on a wide column, below it on a narrow one.
    readonly property bool wideHeader: content.width >= 640
    // Buttons table (grid 14 | flex | 240 | 72): the mapping field and the action cell give way on a narrow column.
    readonly property bool tight: content.width < 480
    readonly property int padActionWidth: tight ? 56 : 72
    // Hotkeys table: ACTION | KEY | GAMEPAD | Reset
    // Hotkey columns (3f-4): ACTION 1fr | KEY 150 | GAMEPAD 170 | state 70. KEY and GAMEPAD never shrink, so "Press a key…" and
    // "Select+RT" are shown in full; the action column gives way (it elides).
    readonly property int hotkeyKeyWidth: compact ? 124 : 150
    readonly property int hotkeyKeyMinWidth: hotkeyKeyWidth
    readonly property int hotkeyPadWidth: compact ? 140 : 170
    readonly property int hotkeyPadMinWidth: hotkeyPadWidth
    color: Theme.ground


    // One page tab (same look as UzTabs, with an objectName per tab): ink 500 with the 2 px Yamabuki underline when active.
    component PageTab: Item {
        id: pt
        property string text: ""
        property bool active: false
        signal clicked()
        width: ptLabel.implicitWidth
        height: 40
        activeFocusOnTab: true
        Accessible.role: Accessible.PageTab
        Accessible.name: pt.text
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                pt.clicked()
                event.accepted = true
            }
        }
        Text {
            id: ptLabel
            anchors.verticalCenter: parent.verticalCenter
            text: pt.text
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: 14
            font.weight: pt.active ? Font.Medium : Font.Normal
            color: pt.active || ptHover.hovered ? Theme.ink : Theme.muted
        }
        Rectangle { visible: pt.active; anchors.bottom: parent.bottom; width: parent.width; height: 2; color: Theme.accent }
        UzFocusRing { visible: pt.activeFocus }
        HoverHandler { id: ptHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: pt.clicked() }
    }

    // Table header cell (role table-header: Mono 500 10, +12 %, uppercase, `dim`).
    component Th: Text {
        Layout.minimumWidth: 0
        textFormat: Text.PlainText
        elide: Text.ElideRight
        font.family: Theme.mono
        font.pixelSize: Theme.roles.tableHeader.size
        font.weight: Font.Medium
        font.letterSpacing: Theme.roles.tableHeader.spacing
        font.capitalization: Font.AllUppercase
        color: Theme.dim
    }

    // Value cell of a binding: key caps for a set value (keyboard keys and gamepad combos like the Kbd of the boards), plain
    // text for "Press a key…", "not set" and names. `text` stays the readable value.
    component KeyText: Item {
        id: kt
        property string text: ""
        property bool caps: false
        property alias font: plain.font
        property alias color: plain.color
        property alias elide: plain.elide
        property alias verticalAlignment: plain.verticalAlignment
        implicitHeight: 24
        implicitWidth: kt.caps ? kbd.implicitWidth : plain.implicitWidth
        clip: true
        Text {
            id: plain
            visible: !kt.caps
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            text: kt.text
            textFormat: Text.PlainText
            font.family: Theme.sans
        }
        UzKbd {
            id: kbd
            visible: kt.caps
            anchors.verticalCenter: parent.verticalCenter
            keys: kt.text
        }
    }

    // B / Esc / Left inside the content: back to the column (the shell moves to the AppNav when this returns false).
    function handleBack(): bool { return devNav.backFromContent() }

    onVisibleChanged: if (visible) keyScope.forceActiveFocus()
    Connections {
        target: root.ctl
        function onHotkeysChanged() { if (root.ctl.hotkeyListening !== "" || root.ctl.padHotkeyListening !== "") root.plugins.cancelHotkeyCapture() }
        function onSelectionChanged() { root.renaming = false; root.confirmDelete = false }
        function onProfilesChanged() { root.confirmDelete = false }
    }

    // Keys: capture while "Press a button…" is shown on a keyboard profile, else feed the input test.
    FocusScope {
        id: keyScope
        anchors.fill: parent
        focus: root.visible
        Keys.onPressed: event => {
            if (root.renaming) { return }
            if (root.hotkeysTab && root.plugins.captureHotkey(event.key, event.modifiers)) {
                event.accepted = true
            } else if (root.ctl.captureKey(event.key)) {
                event.accepted = true
            } else if (!event.isAutoRepeat) {
                root.ctl.testKey(event.key, true)
            }
        }
        Keys.onReleased: event => {
            if (!event.isAutoRepeat) { root.ctl.testKey(event.key, false) }
        }
        onActiveFocusChanged: if (!activeFocus) root.ctl.clearTestKeys()

        UzPageHints {
            id: pageHints
            player: root.player
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
        }

        RowLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: pageHints.top
            spacing: 0

            UzAppNav {
                Layout.fillHeight: true
                Layout.preferredWidth: Theme.sidebar
                player: root.player
            }

            // Second column: connected controllers (board 9a-1)
            Item {
                Layout.fillHeight: true
                Layout.preferredWidth: Theme.sidebar
                Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.rule }
                Flickable {
                    id: devFlick
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: devNav.implicitHeight + Theme.space7
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    UzColumnNav {
                        id: devNav
                        objectName: "controllersColumnNav"
                        content: flick
                        flickable: devFlick
                        onContentRequested: devNav.focusFirstIn(flick)
                        x: Theme.space2
                        y: Theme.space5 + Theme.space1
                        width: parent.width - 2 * Theme.space2 - 1
                        spacing: 0

                        UzSectionLabel {
                            Layout.fillWidth: true
                            Layout.leftMargin: Theme.space15
                            text: qsTr("Controllers")
                        }
                        UzLabel {
                            objectName: "profilesLocalFooter"
                            Layout.fillWidth: true
                            Layout.leftMargin: Theme.space15
                            Layout.rightMargin: Theme.space15
                            Layout.topMargin: 14
                            Layout.bottomMargin: 14
                            text: qsTr("Mapping is saved per controller on this PC.")
                            color: Theme.muted
                            font.pixelSize: Theme.roles.small.size
                            lineHeight: Theme.roles.small.line
                            lineHeightMode: Text.FixedHeight
                            wrapMode: Text.WordWrap
                        }

                        Repeater {
                            model: root.ctl.devices
                            delegate: UzColumnNavRow {
                                id: dev
                                required property var modelData
                                objectName: "device_" + modelData.key
                                label: dev.modelData.name
                                meta: dev.modelData.slot
                                metaName: "deviceSlot_" + dev.modelData.key
                                selected: modelData.key === root.ctl.selectedDevice
                                onClicked: root.ctl.selectDevice(dev.modelData.key)
                                UzLabel {
                                    objectName: "deviceStatus_" + dev.modelData.key
                                    Layout.fillWidth: true
                                    text: (dev.modelData.connected ? "● " : "") + dev.modelData.status
                                          + (dev.modelData.profile !== "" ? " · " + dev.modelData.profile : "")
                                    color: dev.modelData.connected ? Theme.ok : Theme.dim
                                    font.pixelSize: Theme.roles.small.size
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        UzLabel {
                            visible: !root.ctl.gamepadAvailable
                            objectName: "gamepadNote"
                            Layout.fillWidth: true
                            Layout.leftMargin: Theme.space15
                            Layout.rightMargin: Theme.space15
                            Layout.topMargin: Theme.space15
                            text: root.ctl.gamepadNote
                            color: Theme.muted
                            font.pixelSize: Theme.roles.small.size
                            wrapMode: Text.WordWrap
                        }
                        UzLabel {
                            visible: root.ctl.gamepadAvailable && root.ctl.devices.length <= 2
                            objectName: "noGamepadHint"
                            Layout.fillWidth: true
                            Layout.leftMargin: Theme.space15
                            Layout.rightMargin: Theme.space15
                            Layout.topMargin: Theme.space15
                            text: qsTr("No gamepad connected. Connect one and it appears here (the first gamepad is P1).")
                            color: Theme.dim
                            font.pixelSize: Theme.roles.small.size
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            // Main: profile and mapping
            Flickable {
                id: flick
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: width
                contentHeight: content.implicitHeight + Theme.space7
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ColumnLayout {
                    id: content
                    x: Theme.space5 + Theme.space1
                    y: Theme.space5 + Theme.space1
                    width: Math.max(240, Math.min(760, flick.width - 2 * (Theme.space5 + Theme.space1)))  // never collapses to 0 on a hidden or unsized page (wrapping text would oscillate)
                    spacing: Theme.space2

                    // Header: section label (state of the device), title (Light 40), tabs; the selects row below.
                    ColumnLayout {
                        id: header
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: Theme.space2
                        UzPageHeader {
                            objectName: "controllersHeader"
                            titleName: "controllersTitle"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            number: 1
                            label: qsTr("Controllers")
                            labelMeta: root.hotkeysTab ? qsTr("This PC only")
                                       : root.ctl.device.name === undefined ? ""
                                       : root.ctl.device.connected === true
                                         ? (root.ctl.device.slot !== "—" ? qsTr("Connected · %1").arg(root.ctl.device.slot) : qsTr("Connected"))
                                         : qsTr("Not connected")
                            title: root.hotkeysTab ? qsTr("Hotkeys")
                                   : root.ctl.device.name !== undefined ? root.ctl.device.name : qsTr("Controllers")
                        }
                        // Tabs (board 9a-1) with "Reset mapping" / "Reset hotkeys" at the right while something differs from the defaults.
                        RowLayout {
                            id: tabRow
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: Theme.space2
                            Item {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                implicitHeight: 40
                                implicitWidth: tabs.implicitWidth
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                                Row {
                                    id: tabs
                                    spacing: 28
                                    PageTab { id: tabButtonsChip; objectName: "tabButtons"; text: qsTr("Mapping"); active: !root.hotkeysTab; onClicked: { root.ctl.cancelHotkeyCapture(); root.plugins.cancelHotkeyCapture(); root.tab = "buttons" } }
                                    PageTab { id: tabHotkeysChip; objectName: "tabHotkeys"; text: qsTr("Hotkeys"); active: root.hotkeysTab; onClicked: { root.ctl.cancelCapture(); root.ctl.cancelHotkeyCapture(); root.tab = "hotkeys" } }
                                }
                            }
                            UzButton {
                                id: resetChangedButton
                                objectName: "resetChanged"
                                readonly property int n: root.hotkeysTab ? root.ctl.hotkeysChangedCount : root.ctl.changedCount
                                visible: n > 0 && (root.hotkeysTab || (!root.isMouse && !root.ctl.profileBuiltin))
                                kind: "ghost"
                                size: "sm"
                                text: root.hotkeysTab ? qsTr("Reset hotkeys") : qsTr("Reset mapping")
                                onClicked: root.hotkeysTab ? root.ctl.resetHotkeys() : root.ctl.resetProfile()
                            }
                        }
                    }

                    // Input test below 1440 px: a disclosure under the device header (the column takes over at 1440 px).
                    ColumnLayout {
                        objectName: "inputTestDisclosure"
                        visible: !root.inputTestColumn && !root.hotkeysTab
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
                        visible: !root.isMouse && !root.hotkeysTab
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
                        visible: root.ctl.device.kind === "gamepad" && !root.hotkeysTab
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
                        visible: !root.isMouse && !root.hotkeysTab && root.ctl.systems.length > 1
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
                        visible: root.isMouse && !root.hotkeysTab
                        objectName: "mouseNote"
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
                        text: qsTr("The mouse is the %1 input: move it over the touch screen, left mouse button = touch. This mapping is fixed and has no profile.").arg(root.ctl.touchLabel)
                        color: Theme.muted
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        visible: !root.isMouse && !root.hotkeysTab && root.ctl.profileBuiltin
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

                    // Hotkeys table (global): ACTION | KEY | GAMEPAD | Reset / Default
                    ColumnLayout {
                        visible: root.hotkeysTab
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: content.width
                        spacing: 0

                        UzLabel {
                            objectName: "hotkeysInfo"
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.minimumWidth: 0
                            Layout.bottomMargin: Theme.space15
                            text: qsTr("Uzume hotkeys work on the keyboard and on a gamepad (hold the hotkey button, then press another one) and are never sent to the game.")
                            color: Theme.muted
                            font.pixelSize: Theme.roles.small.size
                            wrapMode: Text.WordWrap
                        }
                        // Hotkey button of the gamepad combos (default Select).
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.bottomMargin: Theme.space15
                            spacing: Theme.space2
                            UzLabel { Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.minimumWidth: 0; text: qsTr("Gamepad hotkey button"); elide: Text.ElideRight }
                            UzCaptureField {
                                capturing: root.ctl.padHotkeyListening === "modifier"; accessibleName: qsTr("Gamepad modifier")
                                objectName: "padModifierField"
                                Layout.fillWidth: true
                                Layout.preferredWidth: root.hotkeyPadWidth
                                Layout.minimumWidth: root.hotkeyPadMinWidth
                                Layout.maximumWidth: root.hotkeyPadWidth
                                implicitHeight: 30
                                color: "transparent"
                                readonly property bool listening: root.ctl.padHotkeyListening === "modifier"
                                border.width: 1
                                border.color: listening ? Theme.accent : Theme.rule2
                                KeyText {
                                    objectName: "padModifierText"
                                    caps: !parent.listening
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 4
                                    verticalAlignment: Text.AlignVCenter
                                    text: parent.listening ? qsTr("Press a button…") : root.ctl.padHotkeyModifier
                                    color: parent.listening ? Theme.accentText : Theme.ink
                                    font.pixelSize: Theme.roles.input.size
                                    elide: Text.ElideRight
                                }
                                onActivated: listening ? root.ctl.cancelPadHotkeyCapture() : root.ctl.beginPadHotkeyCapture("modifier")
                            }
                            RowLayout {
                                Layout.preferredWidth: root.actionWidth
                                Layout.minimumWidth: root.actionWidth
                                Layout.maximumWidth: root.actionWidth
                                UzButton {
                                    objectName: "padModifierReset"
                                    Layout.maximumWidth: root.actionWidth
                                    visible: root.ctl.padHotkeyModifierChanged
                                    kind: "link"
                                    font.underline: true
                                    font.pixelSize: Theme.roles.small.size
                                    text: qsTr("Reset")
                                    onClicked: root.ctl.resetPadHotkeyModifier()
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.bottomMargin: 6
                            spacing: Theme.space2
                            Th { Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.minimumWidth: 0; text: qsTr("Action") }
                            Th { Layout.fillWidth: true; Layout.preferredWidth: root.hotkeyKeyWidth; Layout.minimumWidth: root.hotkeyKeyMinWidth; Layout.maximumWidth: root.hotkeyKeyWidth; text: qsTr("Key") }
                            Th { Layout.fillWidth: true; Layout.preferredWidth: root.hotkeyPadWidth; Layout.minimumWidth: root.hotkeyPadMinWidth; Layout.maximumWidth: root.hotkeyPadWidth; text: qsTr("Gamepad") }
                            Item { Layout.preferredWidth: root.actionWidth; Layout.minimumWidth: root.actionWidth; Layout.maximumWidth: root.actionWidth }
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.topMargin: Theme.space1
                            Layout.bottomMargin: 6
                            spacing: Theme.space1
                            Th { objectName: "uzumeHotkeyGroup"; text: qsTr("Uzume"); Layout.fillWidth: false }
                            Item { Layout.fillWidth: true }
                            UzMono { text: qsTr("always win"); color: Theme.muted }
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }

                        Repeater {
                            model: root.ctl.hotkeyRows
                            delegate: ColumnLayout {
                                id: hrow
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 0
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 6
                                    Layout.bottomMargin: 6
                                    spacing: Theme.space2
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 1
                                        Layout.minimumWidth: 0
                                        spacing: Theme.space1
                                        Rectangle {
                                            objectName: "hotkeyChangedDot_" + hrow.modelData.action
                                            visible: hrow.modelData.changed || hrow.modelData.padChanged
                                            Layout.preferredWidth: 6
                                            Layout.preferredHeight: 6
                                            radius: 0
                                            color: Theme.accent
                                        }
                                        // Same structure as the Buttons table: one label directly in the cell.
                                        UzLabel { Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.minimumWidth: 0; text: hrow.modelData.label; elide: Text.ElideRight }
                                    }
                                    UzCaptureField {
                                        locked: hrow.modelData.fixed
                                        capturing: hrow.modelData.listening; accessibleName: qsTr("Key for %1").arg(hrow.modelData.label !== undefined ? hrow.modelData.label : hrow.modelData.action)
                                        objectName: "hotkeyField_" + hrow.modelData.action
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: root.hotkeyKeyWidth
                                        Layout.minimumWidth: root.hotkeyKeyMinWidth
                                        Layout.maximumWidth: root.hotkeyKeyWidth
                                        implicitHeight: 30
                                        color: "transparent"
                                        border.width: 1
                                        border.color: hrow.modelData.listening ? Theme.accent : Theme.rule2
                                        opacity: hrow.modelData.fixed ? 0.85 : 1
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 4
                                            KeyText {
                                                objectName: "hotkeyText_" + hrow.modelData.action
                                                caps: !hrow.modelData.listening && hrow.modelData.set
                                                Layout.fillWidth: true
                                                Layout.preferredWidth: 1
                                                Layout.minimumWidth: 0
                                                text: hrow.modelData.listening ? qsTr("Press a key…") : hrow.modelData.key
                                                color: hrow.modelData.listening ? Theme.accentText
                                                       : (hrow.modelData.set ? Theme.ink : Theme.dim)
                                                font.pixelSize: Theme.roles.input.size
                                                elide: Text.ElideRight
                                            }
                                            UzButton {
                                                visible: hrow.modelData.set && !hrow.modelData.fixed && !hrow.modelData.listening
                                                objectName: "hotkeyClear_" + hrow.modelData.action
                                                kind: "link"
                                                text: "×"
                                                onClicked: root.ctl.clearHotkey(hrow.modelData.action)
                                            }
                                        }
                                        onActivated: hrow.modelData.listening ? root.ctl.cancelHotkeyCapture() : root.ctl.beginHotkeyCapture(hrow.modelData.action)
                                    }
                                    UzCaptureField {
                                        locked: hrow.modelData.fixed
                                        capturing: hrow.modelData.padListening; accessibleName: qsTr("Gamepad button for %1").arg(hrow.modelData.label !== undefined ? hrow.modelData.label : hrow.modelData.action)
                                        objectName: "hotkeyPadField_" + hrow.modelData.action
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: root.hotkeyPadWidth
                                        Layout.minimumWidth: root.hotkeyPadMinWidth
                                        Layout.maximumWidth: root.hotkeyPadWidth
                                        implicitHeight: 30
                                        color: "transparent"
                                        border.width: 1
                                        border.color: hrow.modelData.padListening ? Theme.accent : Theme.rule2
                                        opacity: hrow.modelData.fixed ? 0.85 : 1
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 4
                                            KeyText {
                                                objectName: "hotkeyPadText_" + hrow.modelData.action
                                                caps: !hrow.modelData.padListening && hrow.modelData.padSet
                                                Layout.fillWidth: true
                                                Layout.preferredWidth: 1
                                                Layout.minimumWidth: 0
                                                text: hrow.modelData.padListening ? qsTr("Press a button…") : hrow.modelData.pad
                                                color: hrow.modelData.padListening ? Theme.accentText
                                                       : (hrow.modelData.padSet ? Theme.ink : Theme.dim)
                                                font.pixelSize: Theme.roles.input.size
                                                elide: Text.ElideRight
                                            }
                                            UzButton {
                                                visible: hrow.modelData.padSet && !hrow.modelData.padListening && !hrow.modelData.fixed
                                                objectName: "hotkeyPadClear_" + hrow.modelData.action
                                                kind: "link"
                                                text: "×"
                                                onClicked: root.ctl.clearPadHotkey(hrow.modelData.action)
                                            }
                                        }
                                        onActivated: hrow.modelData.padListening ? root.ctl.cancelPadHotkeyCapture() : root.ctl.beginPadHotkeyCapture(hrow.modelData.action)
                                    }
                                    RowLayout {
                                        Layout.preferredWidth: root.actionWidth
                                        Layout.minimumWidth: root.actionWidth
                                        Layout.maximumWidth: root.actionWidth
                                        UzButton {
                                            objectName: "hotkeyReset_" + hrow.modelData.action
                                            Layout.maximumWidth: root.actionWidth
                                            visible: hrow.modelData.changed || hrow.modelData.padChanged
                                            kind: "link"
                                            font.underline: true
                                            font.pixelSize: Theme.roles.small.size
                                            text: qsTr("Reset")
                                            onClicked: root.ctl.resetHotkey(hrow.modelData.action)
                                        }
                                        UzLabel {
                                            visible: !(hrow.modelData.changed || hrow.modelData.padChanged) && !hrow.modelData.fixed
                                            text: qsTr("Default")
                                            font.pixelSize: Theme.roles.small.size
                                            color: Theme.disabledInk
                                        }
                                        UzLabel {
                                            visible: hrow.modelData.fixed
                                            text: qsTr("Fixed")
                                            font.pixelSize: Theme.roles.small.size
                                            color: Theme.disabledInk
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                }
                                // Note under the KEY column (aligned with it, not with the action text, P35)
                                RowLayout {
                                    visible: hrow.modelData.conflictInput !== ""
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.bottomMargin: 6
                                    spacing: Theme.space2
                                    Item { Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.minimumWidth: 0 }
                                    UzLabel {
                                        objectName: "hotkeyConflict_" + hrow.modelData.action
                                        Layout.preferredWidth: root.hotkeyKeyWidth + root.hotkeyPadWidth + Theme.space2
                                        Layout.minimumWidth: root.hotkeyKeyWidth + root.hotkeyPadWidth + Theme.space2
                                        Layout.maximumWidth: root.hotkeyKeyWidth + root.hotkeyPadWidth + Theme.space2
                                        text: qsTr("Also mapped to %1 in the keyboard profile · the hotkey wins").arg(hrow.modelData.conflictInput)
                                        color: Theme.muted
                                        font.pixelSize: Theme.roles.small.size
                                        wrapMode: Text.WordWrap
                                    }
                                    Item { Layout.preferredWidth: root.actionWidth; Layout.minimumWidth: root.actionWidth; Layout.maximumWidth: root.actionWidth }
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }
                            }
                        }
                        // Fixed Uzume keys that are not Player actions of the table above: F4 and the panel tabs.
                        Repeater {
                            model: [
                                { id: "hudAll", label: qsTr("Show or hide all HUD widgets"), key: "F4", pad: root.ctl.fixedPad.hudAll },
                                { id: "panelTabs", label: qsTr("Panel tabs"), key: "Alt+1 … Alt+9", pad: root.ctl.fixedPad.panelTabs }
                            ]
                            delegate: ColumnLayout {
                                id: frow
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 0
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 6
                                    Layout.bottomMargin: 6
                                    spacing: Theme.space2
                                    UzLabel { Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.minimumWidth: 0; text: frow.modelData.label; elide: Text.ElideRight }
                                    Rectangle {
                                        objectName: "hotkeyField_" + frow.modelData.id
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: root.hotkeyKeyWidth
                                        Layout.minimumWidth: root.hotkeyKeyMinWidth
                                        Layout.maximumWidth: root.hotkeyKeyWidth
                                        implicitHeight: 30
                                        color: "transparent"
                                        border.width: 1
                                        border.color: "transparent"
                                        opacity: 0.85
                                        KeyText {
                                            objectName: "hotkeyText_" + frow.modelData.id
                                            caps: frow.modelData.id !== "panelTabs"
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 4
                                            verticalAlignment: Text.AlignVCenter
                                            text: frow.modelData.key
                                            color: Theme.ink
                                            font.pixelSize: Theme.roles.input.size
                                            elide: Text.ElideRight
                                        }
                                    }
                                    Rectangle {
                                        objectName: "hotkeyPadField_" + frow.modelData.id
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: root.hotkeyPadWidth
                                        Layout.minimumWidth: root.hotkeyPadMinWidth
                                        Layout.maximumWidth: root.hotkeyPadWidth
                                        implicitHeight: 30
                                        color: "transparent"
                                        border.width: 1
                                        border.color: "transparent"
                                        opacity: 0.85
                                        KeyText {
                                            objectName: "hotkeyPadText_" + frow.modelData.id
                                            caps: frow.modelData.pad !== ""
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 4
                                            verticalAlignment: Text.AlignVCenter
                                            text: frow.modelData.pad !== "" ? frow.modelData.pad : qsTr("not set")
                                            color: frow.modelData.pad !== "" ? Theme.ink : Theme.dim
                                            font.pixelSize: Theme.roles.input.size
                                            elide: Text.ElideRight
                                        }
                                    }
                                    UzLabel {
                                        Layout.preferredWidth: root.actionWidth
                                        Layout.minimumWidth: root.actionWidth
                                        Layout.maximumWidth: root.actionWidth
                                        text: qsTr("Fixed")
                                        font.pixelSize: Theme.roles.small.size
                                        color: Theme.disabledInk
                                    }
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }
                            }
                        }

                        // One group per plugin that declares hotkeys (keyboard only). Uzume keys always win; a plugin key in
                        // conflict is not active until the user chooses another one.
                        Repeater {
                            model: root.plugins.hotkeyGroups
                            delegate: ColumnLayout {
                                id: pgroup
                                required property var modelData
                                objectName: "pluginHotkeyGroup_" + pgroup.modelData.pluginId
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.topMargin: Theme.space2
                                spacing: 0
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.bottomMargin: 6
                                    spacing: Theme.space1
                                    Th { objectName: "pluginHotkeyName_" + pgroup.modelData.pluginId; text: pgroup.modelData.name; Layout.fillWidth: false; Layout.maximumWidth: 260 }
                                    UzTag {
                                        objectName: "pluginHotkeyOrigin_" + pgroup.modelData.pluginId
                                        visible: pgroup.modelData.marker !== ""
                                        small: true
                                        tone: "neutral"
                                        text: pgroup.modelData.marker
                                    }
                                    Item { Layout.fillWidth: true }
                                    UzMono {
                                        objectName: "pluginHotkeyCount_" + pgroup.modelData.pluginId
                                        text: pgroup.modelData.conflicts > 0
                                              ? qsTr("%1 · %2").arg(Theme.plural(pgroup.modelData.count, qsTr("key"), qsTr("keys"))).arg(Theme.plural(pgroup.modelData.conflicts, qsTr("conflict"), qsTr("conflicts")))
                                              : Theme.plural(pgroup.modelData.count, qsTr("key"), qsTr("keys"))
                                        color: pgroup.modelData.conflicts > 0 ? Theme.accentText : Theme.muted
                                    }
                                }
                                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }
                                Repeater {
                                    model: pgroup.modelData.rows
                                    delegate: ColumnLayout {
                                        id: prow
                                        required property var modelData
                                        readonly property string rid: prow.modelData.pluginId + "_" + prow.modelData.id
                                        readonly property bool conflict: prow.modelData.conflict
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        spacing: 0
                                        Rectangle {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            implicitHeight: pcol.implicitHeight + 12
                                            color: prow.conflict ? Theme.selected : "transparent"
                                            border.width: prow.conflict ? 1 : 0
                                            border.color: Theme.accent
                                            radius: 0
                                            ColumnLayout {
                                                id: pcol
                                                anchors.fill: parent
                                                anchors.leftMargin: prow.conflict ? 8 : 0
                                                anchors.rightMargin: prow.conflict ? 8 : 0
                                                anchors.topMargin: 6
                                                spacing: 0
                                                RowLayout {
                                                    id: prowLine
                                                    Layout.fillWidth: true
                                                    spacing: Theme.space2
                                                    RowLayout {
                                                        Layout.fillWidth: true
                                                        Layout.preferredWidth: 1
                                                        Layout.minimumWidth: 0
                                                        spacing: Theme.space1
                                                        Rectangle {
                                                            objectName: "pluginHotkeyChangedDot_" + prow.rid
                                                            visible: prow.modelData.changed
                                                            Layout.preferredWidth: 6
                                                            Layout.preferredHeight: 6
                                                            radius: 0
                                                            color: Theme.accent
                                                        }
                                                        UzLabel { Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.minimumWidth: 0; text: prow.modelData.label; elide: Text.ElideRight }
                                                    }
                                                    UzCaptureField {
                                                        capturing: prow.modelData.listening; accessibleName: qsTr("Key for %1").arg(prow.modelData.label !== undefined ? prow.modelData.label : prow.modelData.id)
                                                        objectName: "pluginHotkeyField_" + prow.rid
                                                        Layout.fillWidth: true
                                                        Layout.preferredWidth: root.hotkeyKeyWidth
                                                        Layout.minimumWidth: root.hotkeyKeyMinWidth
                                                        Layout.maximumWidth: root.hotkeyKeyWidth
                                                        implicitHeight: 30
                                                        color: "transparent"
                                                        border.width: 1
                                                        border.color: prow.modelData.listening ? Theme.accent : (prow.conflict ? Theme.accent : Theme.rule2)
                                                        RowLayout {
                                                            anchors.fill: parent
                                                            anchors.leftMargin: 10
                                                            anchors.rightMargin: 4
                                                            KeyText {
                                                                objectName: "pluginHotkeyText_" + prow.rid
                                                                caps: !prow.modelData.listening && prow.modelData.set
                                                                Layout.fillWidth: true
                                                                Layout.preferredWidth: 1
                                                                Layout.minimumWidth: 0
                                                                text: prow.modelData.listening ? qsTr("Press a key…") : prow.modelData.keyText
                                                                color: prow.modelData.listening ? Theme.accentText
                                                                       : (prow.conflict ? Theme.accentText : (prow.modelData.set ? Theme.ink : Theme.dim))
                                                                font.pixelSize: Theme.roles.input.size
                                                                elide: Text.ElideRight
                                                            }
                                                            UzButton {
                                                                visible: prow.modelData.set && !prow.modelData.listening
                                                                objectName: "pluginHotkeyClear_" + prow.rid
                                                                kind: "link"
                                                                text: "×"
                                                                onClicked: root.plugins.clearHotkey(prow.modelData.pluginId, prow.modelData.id)
                                                            }
                                                        }
                                                        onActivated: prow.modelData.listening ? root.plugins.cancelHotkeyCapture()
                                                            : root.plugins.beginHotkeyCapture(prow.modelData.pluginId, prow.modelData.id)
                                                    }
                                                    UzLabel {
                                                        objectName: "pluginHotkeyPad_" + prow.rid
                                                        Layout.fillWidth: true
                                                        Layout.preferredWidth: root.hotkeyPadWidth
                                                        Layout.minimumWidth: root.hotkeyPadMinWidth
                                                        Layout.maximumWidth: root.hotkeyPadWidth
                                                        text: "—"
                                                        color: Theme.disabledInk
                                                        font.pixelSize: Theme.roles.input.size
                                                    }
                                                    RowLayout {
                                                        Layout.preferredWidth: root.actionWidth
                                                        Layout.minimumWidth: root.actionWidth
                                                        Layout.maximumWidth: root.actionWidth
                                                        UzButton {
                                                            objectName: "pluginHotkeyReset_" + prow.rid
                                                            Layout.maximumWidth: root.actionWidth
                                                            visible: prow.modelData.changed
                                                            kind: "link"
                                                            font.underline: true
                                                            font.pixelSize: Theme.roles.small.size
                                                            text: qsTr("Reset")
                                                            onClicked: root.plugins.resetHotkey(prow.modelData.pluginId, prow.modelData.id)
                                                        }
                                                        UzLabel {
                                                            visible: !prow.modelData.changed
                                                            text: qsTr("Default")
                                                            font.pixelSize: Theme.roles.small.size
                                                            color: Theme.disabledInk
                                                        }
                                                        Item { Layout.fillWidth: true }
                                                    }
                                                }
                                                UzLabel {
                                                    id: prowNote
                                                    objectName: "pluginHotkeyNote_" + prow.rid
                                                    visible: prow.conflict
                                                    Layout.fillWidth: true
                                                    Layout.preferredWidth: 1
                                                    Layout.minimumWidth: 0
                                                    text: prow.modelData.note
                                                    color: Theme.ink2
                                                    font.pixelSize: Theme.roles.small.size
                                                    wrapMode: Text.WordWrap
                                                }
                                            }
                                        }
                                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }
                                    }
                                }
                            }
                        }
                        UzLabel {
                            objectName: "hotkeysFooter"
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.minimumWidth: 0
                            Layout.topMargin: Theme.space15
                            text: qsTr("Gamepad hotkeys: hold Select, then press the second button. They are never sent to the game. Plugin hotkeys have no gamepad button by default.")
                            color: Theme.dim
                            font.pixelSize: Theme.roles.small.size
                            wrapMode: Text.WordWrap
                        }
                        UzLabel {
                            objectName: "pluginHotkeyCaptureNote"
                            visible: root.plugins.hotkeyNote !== ""
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.minimumWidth: 0
                            Layout.topMargin: Theme.space1
                            text: root.plugins.hotkeyNote
                            color: Theme.accentText
                            font.pixelSize: Theme.roles.small.size
                            wrapMode: Text.WordWrap
                        }
                        UzLabel {
                            objectName: "hotkeyNote"
                            visible: root.ctl.hotkeyNote !== ""
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.minimumWidth: 0
                            Layout.topMargin: Theme.space1
                            text: root.ctl.hotkeyNote
                            color: Theme.accentText
                            font.pixelSize: Theme.roles.small.size
                            wrapMode: Text.WordWrap
                        }
                    }

                    // Mapping table: grid 14 | flex | 240 | 72 (dot | DS input | mapping | Reset / Default)
                    ColumnLayout {
                        visible: !root.isMouse && !root.hotkeysTab
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.bottomMargin: 6
                            spacing: Theme.space15
                            Item { Layout.preferredWidth: 14; Layout.minimumWidth: 14; Layout.maximumWidth: 14 }
                            Th {
                                Layout.fillWidth: true; Layout.preferredWidth: 90; Layout.minimumWidth: 0
                                text: qsTr("Game button")
                            }
                            Th { Layout.fillWidth: true; Layout.preferredWidth: root.tight ? 190 : 240; Layout.minimumWidth: 120; Layout.maximumWidth: 240; text: root.ctl.device.kind === "keyboard" ? qsTr("Keyboard") : qsTr("Gamepad") }
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
                                            border.color: mrow.modelData.listening ? Theme.accent : Theme.rule2
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
                                                KeyText {
                                                    objectName: "mapText_" + mrow.modelData.input
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
                                            UzLabel {
                                                visible: !mrow.modelData.changed
                                                text: qsTr("Default")
                                                font.pixelSize: Theme.roles.small.size
                                                color: Theme.disabledInk
                                            }
                                            Item { Layout.fillWidth: true }
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
                        visible: !root.isMouse && !root.hotkeysTab
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
                        visible: root.renaming && !root.isMouse && !root.hotkeysTab
                        Layout.fillWidth: true
                        spacing: 10
                        UzField {
                            id: renameField
                            objectName: "renameField"
                            width: 240
                                                        onAccepted: { root.ctl.renameProfile(text); root.renaming = false; keyScope.forceActiveFocus() }
                            Keys.onEscapePressed: { root.renaming = false; keyScope.forceActiveFocus() }
                        }
                        UzButton {
                            objectName: "renameSaveButton"
                            kind: "primary"
                            text: qsTr("Save")
                            onClicked: { root.ctl.renameProfile(renameField.text); root.renaming = false; keyScope.forceActiveFocus() }
                        }
                        UzButton { text: qsTr("Cancel"); onClicked: { root.renaming = false; keyScope.forceActiveFocus() } }
                    }
                }
            }

            // Input test: a column at 1440 px and wider; below that it is a disclosure under the device header (inputTestDisclosure).
            Rectangle {
                visible: root.inputTestColumn
                Layout.fillHeight: true
                Layout.preferredWidth: Theme.inputTestWidth
                Layout.minimumWidth: Theme.inputTestWidth  // never squeezed below the 288 px grid + margins by wide text
                Layout.maximumWidth: Theme.inputTestWidth
                color: Theme.ground
                Rectangle {
                    anchors.left: parent.left
                    width: 1
                    height: parent.height
                    color: Theme.rule
                }
                Loader {
                    anchors.fill: parent
                    anchors.margins: 24
                    anchors.topMargin: Theme.space4
                    active: root.inputTestColumn
                    sourceComponent: inputTestComp
                }
            }
        }
    }

    // The input test (one instance at a time: the column or the disclosure).
    Component {
        id: inputTestComp
        ColumnLayout {
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Th { Layout.fillWidth: true; text: qsTr("Input test") }
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

            Th { visible: root.ctl.hasTouch; text: root.ctl.touchLabel; Layout.topMargin: 10; Layout.minimumWidth: 0; Layout.maximumWidth: Theme.inputTestWidth - 48 }
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
