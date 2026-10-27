import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Settings row (design-system.md "SettingsRow"): label + help + control on the right, changed marker (Yamabuki square), "Reset",
// More / Less for long help; the one row of the Emulation and Settings pages. Grid 14 | flex | 280 | 72: the changed-dot slot (the label never shifts), the text column, the control column
// (right edge = the same x on every page) and the reset column (stays empty with resetMode "none").
// Core-agnostic: the row only knows label, description, values and flags; the control kind follows from the values
// (boolean pair -> toggle, at most 4 labels that fit their cell -> segment, otherwise select).
// Below Theme.settingsNarrowBelow the control column moves under the text so nothing widens past the page column.
Item {
    id: root

    // ---- content
    property string label: ""
    property string description: ""        // raw core text; "Option: explanation" lines become the option list
    property string meta: ""               // inline mono meta after the label ("Channel: beta")
    property string badge: ""              // e.g. "applies on next start"
    property string disabledReason: ""     // non-empty: the control is disabled and the reason is shown
    property string idKey: ""              // option key; derives the objectNames of dot, Reset, Default and badge
    property string controlName: ""        // objectName of the control (select, segment, value)
    property string toggleName: ""         // objectName of a toggle (defaults to controlName)
    property string descriptionName: ""    // objectName of the description text

    // ---- control
    property string ctrl: "auto"           // auto | toggle | segment | select | buttons | value
    property var values: []                // [{ value, label, name? }]
    property string current: ""            // selected value
    property bool checked: false           // toggle without a value list
    property var buttons: []               // [{ name, text, enabled?, visible?, busy?, busyOnClick? }]
    property string valueText: ""          // read-only value
    property bool controlEnabled: true

    // ---- state
    property bool changed: false
    property real separatorInset: 0        // the row sticks out past its column by the reset slot: keeps the hairline ending at the column
    property string resetMode: "reset"     // reset | none
    property bool expanded: false

    signal valuePicked(string value)
    signal toggled(bool checked)
    signal buttonClicked(string name)
    signal resetRequested()

    // ---- geometry (read by tests)
    readonly property bool narrow: width < Theme.settingsNarrowBelow
    readonly property real controlWidth: narrow ? Math.max(120, Math.min(Theme.settingsControlWidth, width - Theme.settingsDotSlot - Theme.settingsResetWidth))
                                               : Theme.settingsControlWidth
    readonly property real textWidth: Math.max(0, narrow ? width - Theme.settingsDotSlot
                                                         : width - Theme.settingsDotSlot - Theme.settingsControlWidth - Theme.settingsResetWidth - 32)
    readonly property alias controlColumn: ctlColumn
    readonly property alias resetColumn: resetCol
    readonly property alias labelItem: labelText
    readonly property alias textColumn: textCol
    readonly property alias descriptionItem: introText
    readonly property alias dotItem: dot
    readonly property alias controlLoader: ctlLoader

    // ---- resolved control kind
    readonly property bool disabled: root.disabledReason !== ""
    readonly property bool isBoolean: {
        if (root.values.length !== 2) {
            return false
        }
        const v = [root.values[0].value, root.values[1].value].join("|")
        return ["off|on", "disabled|enabled", "false|true", "0|1"].indexOf(v) >= 0
    }
    // Segment only when every label fits its cell (control width / n - 12px of padding), else select. Measured in
    // characters (6.5 px each), not font metrics, so the kind never flips with the platform font.
    readonly property bool segmentFits: {
        const n = root.values.length
        if (n < 2 || n > 4) {
            return false
        }
        const cell = root.controlWidth / n - 12
        for (let i = 0; i < n; ++i) {
            if (String(root.values[i].label || "").length > Math.floor(cell / 6.5)) {
                return false
            }
        }
        return true
    }
    readonly property string kind: {
        if (root.ctrl === "buttons" || root.ctrl === "value" || root.ctrl === "toggle" || root.ctrl === "select") {
            return root.ctrl
        }
        if (root.ctrl === "auto" && root.isBoolean) {
            return "toggle"
        }
        return root.segmentFits ? "segment" : "select"
    }
    readonly property bool toggleChecked: root.values.length === 2 ? root.current === root.values[1].value : root.checked

    // ---- description: intro + option list
    function parseDescription(text: string): var {
        const intro = []
        const list = []
        let seenFirst = false
        const lines = String(text).split(/\r?\n/)
        for (let i = 0; i < lines.length; ++i) {
            const ln = lines[i].trim()
            if (ln === "") {
                continue
            }
            const dash = /^[-•*]\s+/.test(ln)
            const m = /^([^:]{1,48}):\s+(.+)$/.exec(ln.replace(/^[-•*]\s+/, ""))
            if (m !== null && (dash || seenFirst)) {
                list.push({ k: m[1].trim(), v: m[2].trim() })
            } else {
                intro.push(ln)
            }
            seenFirst = true
        }
        return { intro: intro.join(" "), list: list }
    }
    readonly property var parsed: root.parseDescription(root.description)
    readonly property string intro: root.parsed.intro
    readonly property var optionList: root.parsed.list
    readonly property bool hasList: root.optionList.length > 0
    readonly property bool overflows: descProbe.lineCount > 2
    readonly property bool canExpand: root.hasList || root.overflows
    readonly property bool showFull: root.expanded && root.canExpand
    readonly property string moreLabel: root.showFull ? qsTr("Less")
                                        : (root.hasList ? qsTr("More · %1").arg(Theme.plural(root.optionList.length, qsTr("option"), qsTr("options")))
                                                        : qsTr("More"))

    // Buttons wrap onto further lines when the control column is too narrow for them (wide fonts), so the control may be taller.
    property real buttonsHeight: 0   // set by the buttons control (not bound: the Loader item would loop)
    readonly property real ctlHeight: root.kind === "buttons" ? Math.max(Theme.settingsControlHeight, root.buttonsHeight) : Theme.settingsControlHeight
    implicitHeight: (root.narrow ? Theme.settingsDotSlot + textCol.implicitHeight + 8 + root.ctlHeight
                                 : Theme.settingsDotSlot + Math.max(root.ctlHeight, textCol.implicitHeight))
                    + Theme.settingsDotSlot + 1


    // Hidden measurement of the full intro at the clamp width: more than 2 lines -> "More".
    Text {
        id: descProbe
        visible: false
        width: Math.min(560, root.textWidth)
        text: root.intro
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.pixelSize: Theme.roles.small.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
    }

    // Row surface (hover) and divider
    Rectangle {
        anchors.fill: parent
        color: hover.hovered ? Theme.hover : "transparent"
    }
    HoverHandler { id: hover }
    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.rightMargin: root.separatorInset; anchors.bottom: parent.bottom; height: 1; color: Theme.rule2 }

    // Dot slot (14): the label never shifts, the dot is centered on the first (32 high) label line.
    Item {
        x: 0
        y: Theme.settingsDotSlot
        width: Theme.settingsDotSlot
        height: Theme.settingsControlHeight
        Rectangle {   // changed marker: a 6 px Yamabuki square
            id: dot
            objectName: root.idKey !== "" ? "changedDot_" + root.idKey : ""
            visible: root.changed
            anchors.centerIn: parent
            width: 6
            height: 6
            radius: 0
            color: Theme.accent
        }
    }

    // Text column
    ColumnLayout {
        id: textCol
        // Not laid out while the page has no real width yet (first frame, width 0): wrapped text would break into
        // one character per line, and its stale height makes the layout polish loop with wide fonts.
        visible: root.textWidth > 24
        x: Theme.settingsDotSlot
        y: Theme.settingsDotSlot
        // Never narrower than 120 while hidden: at width 0 the wrapped texts break per character and flip their height on every font change (polish loop).
        width: Math.max(root.textWidth, 120)
        spacing: 2

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: root.textWidth   // known width: the wrapped label settles in one layout pass (no polish loop at TV)
            Layout.minimumWidth: 0
            Layout.minimumHeight: Theme.settingsControlHeight
            spacing: Theme.space1
            UzLabel {
                id: labelText
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: root.label
                font.weight: Font.Medium
                wrapMode: Text.Wrap
                lineHeight: Theme.roles.button.line
                lineHeightMode: Text.FixedHeight
                verticalAlignment: Text.AlignVCenter
                color: root.disabled ? Theme.muted : Theme.ink
            }
            // The wrapper takes its width from an unelided probe (an elided Text reports a clamped implicit width inside a layout).
            Item {
                visible: root.meta !== ""
                Layout.preferredWidth: Math.ceil(metaProbe.implicitWidth) + 1
                Layout.minimumWidth: 0
                implicitWidth: metaProbe.implicitWidth
                clip: true
                implicitHeight: metaProbe.implicitHeight
                Text {
                    id: metaProbe
                    visible: false
                    text: root.meta
                    textFormat: Text.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta11.size
                }
                Text {
                    id: metaVis
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.meta
                    textFormat: Text.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta11.size
                    color: Theme.muted
                }
            }
            Rectangle {
                objectName: root.idKey !== "" ? "restartBadge_" + root.idKey : ""
                visible: root.badge !== ""
                Layout.preferredWidth: Math.ceil(badgeProbe.implicitWidth) + 13
                Layout.minimumWidth: 0
                implicitWidth: badgeProbe.implicitWidth + 12
                implicitHeight: 18
                radius: 0
                color: "transparent"
                border.width: 1
                border.color: Theme.rule
                Text {
                    id: badgeProbe
                    visible: false
                    text: root.badge
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.roles.meta11.size
                }
                clip: true
                Text {
                    id: badgeText
                    x: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.badge
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.roles.meta11.size
                    color: Theme.muted
                }
            }
        }

        // Description: clamped to 2 lines with "More"; expanded: full intro and the two-column option list.
        Text {
            id: introText
            objectName: root.descriptionName
            visible: root.intro !== ""
            Layout.fillWidth: true
            Layout.preferredWidth: Math.min(560, root.textWidth)
            Layout.minimumWidth: 0
            Layout.maximumWidth: 560
            text: root.intro
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: root.showFull ? 100000 : 2
            elide: root.showFull ? Text.ElideNone : Text.ElideRight
            font.pixelSize: Theme.roles.small.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.muted
        }
        Rectangle {
            id: optionBox
            objectName: root.idKey !== "" ? "optionList_" + root.idKey : ""
            visible: root.showFull && root.hasList
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.maximumWidth: 560
            Layout.topMargin: 6
            implicitHeight: optionGrid.implicitHeight + 16
            radius: 0
            color: "transparent"
            border.width: 1
            border.color: Theme.rule2
            GridLayout {
                id: optionGrid
                x: 12
                y: 8
                width: parent.width - 24
                columns: 2
                columnSpacing: 12
                rowSpacing: 4
                Repeater {
                    model: root.optionList.length * 2
                    delegate: Text {
                        required property int index
                        readonly property var entry: root.optionList[Math.floor(index / 2)]
                        readonly property bool isKey: index % 2 === 0
                        Layout.fillWidth: !isKey
                        Layout.minimumWidth: 0
                        Layout.maximumWidth: isKey ? 160 : Number.POSITIVE_INFINITY
                        Layout.alignment: Qt.AlignTop
                        text: isKey ? entry.k : entry.v
                        textFormat: Text.PlainText
                        wrapMode: isKey ? Text.NoWrap : Text.Wrap
                        elide: isKey ? Text.ElideRight : Text.ElideNone
                        font.pixelSize: Theme.roles.small.size
                        font.weight: isKey ? Font.Medium : Font.Normal
                        lineHeight: Theme.roles.small.line
                        lineHeightMode: Text.FixedHeight
                        color: isKey ? Theme.ink2 : Theme.muted
                    }
                }
            }
        }
        Item {
            id: moreLink
            objectName: root.idKey !== "" ? "descMore_" + root.idKey : ""
            visible: root.intro !== "" && (root.canExpand || root.expanded)
            Layout.preferredWidth: Math.min(moreText.implicitWidth, root.textWidth)
            Layout.minimumWidth: 0
            Layout.preferredHeight: 18
            Layout.topMargin: 2
            activeFocusOnTab: true
            Keys.onReturnPressed: root.expanded = !root.expanded
            Keys.onEnterPressed: root.expanded = !root.expanded
            Keys.onSpacePressed: root.expanded = !root.expanded
            Text {
                id: moreText
                width: parent.width
                elide: Text.ElideRight
                text: root.moreLabel
                textFormat: Text.PlainText
                font.pixelSize: Theme.roles.small.size
                font.underline: true
                color: moreLink.activeFocus ? Theme.ink : Theme.muted
            }
            TapHandler { onTapped: root.expanded = !root.expanded }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
        Text {
            objectName: root.idKey !== "" ? "disabledReason_" + root.idKey : ""
            visible: root.disabled
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.topMargin: 2
            text: "⊘ " + root.disabledReason
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font.pixelSize: Theme.roles.small.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.ink2
        }
    }

    // Control column (right edge = row width - 72 on wide rows)
    Item {
        id: ctlColumn
        objectName: root.idKey !== "" ? "rowControl_" + root.idKey : ""
        x: root.narrow ? Theme.settingsDotSlot : root.width - Theme.settingsResetWidth - Theme.settingsControlWidth
        y: root.narrow ? Theme.settingsDotSlot + textCol.implicitHeight + 8 : Theme.settingsDotSlot
        width: root.controlWidth
        height: root.ctlHeight
        Loader {
            id: ctlLoader
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            enabled: root.controlEnabled && !root.disabled
            opacity: root.disabled ? 0.4 : 1
            sourceComponent: root.kind === "toggle" ? toggleComp
                           : root.kind === "segment" ? segmentComp
                           : root.kind === "select" ? selectComp
                           : root.kind === "buttons" ? buttonsComp
                           : valueComp
        }
    }

    // Reset column (72): "Reset" when changed, "Default" otherwise, empty with resetMode "none".
    Item {
        id: resetCol
        x: root.width - Theme.settingsResetWidth
        y: ctlColumn.y
        width: Theme.settingsResetWidth
        height: Theme.settingsControlHeight
        visible: root.resetMode !== "none"
        Text {
            objectName: root.idKey !== "" ? "optionOrigin_" + root.idKey : ""
            visible: !root.changed
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Default")
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
        Item {
            id: resetLink
            objectName: root.idKey !== "" ? "optionReset_" + root.idKey : ""
            visible: root.changed
            anchors.fill: parent
            activeFocusOnTab: true
            Keys.onReturnPressed: root.resetRequested()
            Keys.onEnterPressed: root.resetRequested()
            Keys.onSpacePressed: root.resetRequested()
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Reset")
                font.pixelSize: Theme.roles.small.size
                font.underline: true
                color: Theme.ink
            }
            TapHandler { onTapped: root.resetRequested() }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
    }

    // ---- controls
    Component {
        id: toggleComp
        Item {
            width: 40
            height: Theme.settingsControlHeight
            UzToggle {
                id: tg
                objectName: root.toggleName !== "" ? root.toggleName : root.controlName
                Accessible.name: root.label
                anchors.verticalCenter: parent.verticalCenter
                width: 40
                height: 22
                checked: root.toggleChecked
                onToggled: {
                    root.toggled(tg.checked)
                    if (root.values.length === 2) {
                        root.valuePicked(root.values[tg.checked ? 1 : 0].value)
                    }
                }
            }
        }
    }

    Component {
        id: segmentComp
        Item {
            width: root.controlWidth
            height: Theme.settingsControlHeight
            UzSegmented {
                id: seg
                objectName: root.controlName
                accessibleName: root.label
                anchors.fill: parent
                stretch: true
                segmentHeight: Theme.settingsControlHeight - 2
                options: root.values.map(function (v) { return { value: v.value, label: v.label, name: v.name || "" } })
                current: root.current
                // Tab stop, arrow keys and focus ring come from UzSegmented itself (R-P4).
                onPicked: value => root.valuePicked(value)
            }
        }
    }

    Component {
        id: selectComp
        Item {
            width: root.controlWidth
            height: Theme.settingsControlHeight
            UzSelect {
                id: sel
                objectName: root.controlName
                accessibleName: root.label
                anchors.fill: parent
                model: root.values
                current: root.current
                font.pixelSize: Theme.roles.input.size
                onPicked: value => root.valuePicked(value)
                Keys.onReturnPressed: event => { if (!sel.popup.visible) { sel.popup.open() } else { event.accepted = false } }
            }
        }
    }

    Component {
        id: buttonsComp
        Flow {
            // Never wider than the column: wide rows right-align the buttons, narrow rows left-align and wrap.
            width: root.controlWidth
            spacing: Theme.space1
            layoutDirection: root.narrow ? Qt.LeftToRight : Qt.RightToLeft
            onChildrenRectChanged: root.buttonsHeight = childrenRect.height
            Repeater {
                // Count model, delegates by index: a rebuilt JS array model recreates the buttons on every change and loops the layout.
                model: root.buttons.length
                delegate: UzButton {
                    id: btn
                    required property int index
                    readonly property var modelData: root.buttons[root.narrow ? btn.index : root.buttons.length - 1 - btn.index] || ({})
                    objectName: modelData.name || ""
                    visible: modelData.visible !== false
                    enabled: modelData.enabled !== false
                    busy: modelData.busy === true
                    busyOnClick: modelData.busyOnClick === true
                    kind: modelData.kind || "outline"
                    implicitHeight: Theme.settingsControlHeight
                    hPadding: 28
                    implicitWidth: Math.min(labelNaturalWidth + hPadding, root.controlWidth)
                    font.pixelSize: Theme.roles.input.size
                    text: modelData.text
                    onClicked: root.buttonClicked(modelData.name || "")
                }
            }
        }
    }

    Component {
        id: valueComp
        Text {
            objectName: root.controlName
            width: root.controlWidth
            height: Theme.settingsControlHeight
            text: root.valueText
            textFormat: Text.PlainText
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            font.family: Theme.mono
            font.pixelSize: Theme.roles.input.size
            color: Theme.ink2
        }
    }
}
