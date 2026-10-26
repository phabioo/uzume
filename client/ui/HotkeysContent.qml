import QtQuick
import QtQuick.Layouts
import Uzume.App

// Hotkeys section of Settings (was the Hotkeys tab of the Controllers screen): Uzume's own hotkeys (keyboard and gamepad) and the
// hotkeys of plugins. Hotkeys are global, independent of the selected controller, and saved on this PC. The page header and the
// scrolling belong to SettingsScreen; key presses reach handleKeyPressed() from there while a hotkey is being captured.
ColumnLayout {
    id: root
    objectName: "hotkeysContent"
    required property PlayerController player
    readonly property var ctl: root.player.controllers
    readonly property var plugins: root.player.plugins
    spacing: Theme.space2
    // Narrow main column: tighter table.
    readonly property bool compact: root.width < 520
    // Hotkey columns (3f-4): ACTION 1fr | KEY 150 | GAMEPAD 170 | state 70. KEY and GAMEPAD never shrink, so "Press a key…" and
    // "Select+RT" are shown in full; the action column gives way (it elides).
    readonly property int hotkeyKeyWidth: compact ? 124 : 150
    readonly property int hotkeyKeyMinWidth: hotkeyKeyWidth
    readonly property int hotkeyPadWidth: compact ? 140 : 170
    readonly property int hotkeyPadMinWidth: hotkeyPadWidth
    readonly property int actionWidth: compact ? 48 : 60

    // A key press while a hotkey is captured (plugin hotkey, then Uzume hotkey); true when it was taken.
    function handleKeyPressed(event): bool {
        if (root.plugins.captureHotkey(event.key, event.modifiers) || root.ctl.captureKey(event.key)) return true
        if (!event.isAutoRepeat) root.ctl.testKey(event.key, true)
        return false
    }
    function handleKeyReleased(event) {
        if (!event.isAutoRepeat) root.ctl.testKey(event.key, false)
    }
    function cancelCaptures() {
        root.ctl.cancelCapture()
        root.ctl.cancelHotkeyCapture()
        root.plugins.cancelHotkeyCapture()
    }

    Connections {
        target: root.ctl
        function onHotkeysChanged() { if (root.ctl.hotkeyListening !== "" || root.ctl.padHotkeyListening !== "") root.plugins.cancelHotkeyCapture() }
    }

    // "Reset hotkeys" at the right while something differs from the defaults.
    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: root.ctl.hotkeysChangedCount > 0
        Item { Layout.fillWidth: true }
        UzButton {
            objectName: "resetChanged"
            kind: "ghost"
            size: "sm"
            text: qsTr("Reset hotkeys")
            onClicked: root.ctl.resetHotkeys()
        }
    }

    // Hotkeys table (global): ACTION | KEY | GAMEPAD | Reset / Default
    ColumnLayout {
        visible: true
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.minimumWidth: 0
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
                BindingText {
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
            TableHeadCell { Layout.fillWidth: true; Layout.preferredWidth: 1; Layout.minimumWidth: 0; text: qsTr("Action") }
            TableHeadCell { Layout.fillWidth: true; Layout.preferredWidth: root.hotkeyKeyWidth; Layout.minimumWidth: root.hotkeyKeyMinWidth; Layout.maximumWidth: root.hotkeyKeyWidth; text: qsTr("Key") }
            TableHeadCell { Layout.fillWidth: true; Layout.preferredWidth: root.hotkeyPadWidth; Layout.minimumWidth: root.hotkeyPadMinWidth; Layout.maximumWidth: root.hotkeyPadWidth; text: qsTr("Gamepad") }
            Item { Layout.preferredWidth: root.actionWidth; Layout.minimumWidth: root.actionWidth; Layout.maximumWidth: root.actionWidth }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }

        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.topMargin: Theme.space1
            Layout.bottomMargin: 6
            spacing: Theme.space1
            TableHeadCell { objectName: "uzumeHotkeyGroup"; text: qsTr("Uzume"); Layout.fillWidth: false }
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
                            BindingText {
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
                            BindingText {
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
                        BindingText {
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
                        BindingText {
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
                    TableHeadCell { objectName: "pluginHotkeyName_" + pgroup.modelData.pluginId; text: pgroup.modelData.name; Layout.fillWidth: false; Layout.maximumWidth: 260 }
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
                                            BindingText {
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
}
