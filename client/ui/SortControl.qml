import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Sort select of the Library toolbar (3c-2): 32 high, "Sort: <value> ▾". The menu is 252 wide with grouped items,
// a "✓" on the selected one and a footer note. picked(key) on user choice; the owner stores it. `compact` drops the "Sort:"
// prefix of the label (narrow toolbars); `showReadyFirst` adds a "Ready first" row at the end of the menu (readyFirstToggled).
Rectangle {
    id: root
    property string current: "played"
    property bool compact: false
    property bool showReadyFirst: false
    property bool readyFirst: false
    property bool readyFirstEnabled: true
    signal picked(string key)
    signal readyFirstToggled(bool on)

    readonly property var sections: [
        { header: qsTr("Played"), items: [
            { value: "played", label: qsTr("Recently played"), short: qsTr("Last played") },
            { value: "playtime", label: qsTr("Most play time"), short: qsTr("Play time") } ] },
        { header: qsTr("Name"), items: [
            { value: "name_asc", label: qsTr("A–Z"), short: qsTr("Name A–Z") },
            { value: "name_desc", label: qsTr("Z–A"), short: qsTr("Name Z–A") } ] },
        { header: qsTr("Date added to server"), items: [
            { value: "added_desc", label: qsTr("Newest first"), short: qsTr("Date added, newest") },
            { value: "added_asc", label: qsTr("Oldest first"), short: qsTr("Date added, oldest") } ] },
        { header: qsTr("More"), items: [
            { value: "size_desc", label: qsTr("Size largest first"), short: qsTr("Size, largest") },
            { value: "size_asc", label: qsTr("Size smallest first"), short: qsTr("Size, smallest") },
            { value: "system", label: qsTr("System then name"), short: qsTr("System") } ] }
    ]
    readonly property string currentLabel: {
        for (const sec of sections) {
            for (const it of sec.items) {
                if (it.value === current) return it.short
            }
        }
        return sections[0].items[0].short
    }
    readonly property alias menuOpen: menu.visible

    objectName: "sortControl"
    implicitHeight: Theme.row36
    implicitWidth: label.implicitWidth + 2 * Theme.space15 + 14 + Theme.space1
    Layout.minimumWidth: 0
    color: menu.visible ? Theme.pressed : (Theme.dark ? "transparent" : Theme.surface)
    border.width: 1
    border.color: menu.visible ? Theme.ink : (hover.hovered ? Theme.ink2 : Theme.rule)
    activeFocusOnTab: true
    Accessible.role: Accessible.ComboBox
    Accessible.name: qsTr("Sort: %1").arg(currentLabel)

    Row {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Theme.space15
        anchors.rightMargin: Theme.space15
        spacing: Theme.space1
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(0, root.width - 2 * Theme.space15 - arrow.width - parent.spacing))
            elide: Text.ElideRight
            textFormat: Text.StyledText
            text: root.compact ? root.currentLabel : "<font color='" + Theme.muted + "'>" + qsTr("Sort:") + "</font> " + root.currentLabel
            font.family: Theme.sans
            font.pixelSize: Theme.roles.input.size
            color: Theme.ink
        }
        UzGlyph {
            id: arrow
            anchors.verticalCenter: parent.verticalCenter
            name: menu.visible ? "chevronUp" : "chevronDown"
            size: 14
            color: Theme.muted
        }
    }
    UzFocusRing { visible: root.activeFocus }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    MouseArea { anchors.fill: parent; onClicked: { root.forceActiveFocus(); menu.visible ? menu.close() : menu.open() } }
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space || event.key === Qt.Key_Down) {
            menu.open()
            event.accepted = true
        }
    }

    Popup {
        id: menu
        objectName: "sortMenu"
        parent: root
        x: root.width - width
        y: root.height + Theme.space05
        width: 252
        padding: 1
        topPadding: 1 + Theme.space05
        bottomPadding: 1 + Theme.space05
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: Theme.surface
            border.width: 1
            border.color: Theme.rule
        }
        contentItem: ColumnLayout {
            spacing: 0
            Repeater {
                model: root.sections
                delegate: ColumnLayout {
                    id: sec
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: 0
                    Eyebrow {
                        Layout.fillWidth: true
                        Layout.topMargin: sec.index === 0 ? Theme.space05 : Theme.space1
                        Layout.leftMargin: Theme.space15
                        Layout.bottomMargin: Theme.space05
                        text: sec.modelData.header
                    }
                    Repeater {
                        model: sec.modelData.items
                        delegate: ItemDelegate {
                            id: item
                            required property var modelData
                            readonly property bool selected: modelData.value === root.current
                            objectName: "sort_" + modelData.value
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            implicitHeight: Theme.row32
                            padding: 0
                            leftPadding: Theme.space15
                            rightPadding: Theme.space15
                            onClicked: { const key = item.modelData.value; menu.close(); root.picked(key) }
                            contentItem: Row {
                                spacing: Theme.space1
                                Item {
                                    width: 14
                                    height: 14
                                    anchors.verticalCenter: parent.verticalCenter
                                    UzGlyph { anchors.centerIn: parent; visible: item.selected; name: "check"; size: 14; color: Theme.accentText }
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.max(0, item.width - 2 * Theme.space15 - 14 - parent.spacing)
                                    text: item.modelData.label
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    font.family: Theme.sans
                                    font.pixelSize: Theme.roles.input.size
                                    font.weight: item.selected ? Font.Medium : Font.Normal
                                    color: Theme.ink
                                }
                            }
                            background: Rectangle {
                                color: item.highlighted || item.hovered ? Theme.hover : "transparent"
                                Rectangle { visible: item.selected; width: 2; height: parent.height; color: Theme.accent }
                                UzFocusRing { inset: true; visible: item.highlighted }
                            }
                        }
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; Layout.topMargin: Theme.space1; height: 1; color: Theme.rule2 }
            // "Ready first": ready games on top, a divider, then the rest (not applied while the Ready filter is on).
            ItemDelegate {
                id: readyRow
                objectName: "readyFirstToggle"
                visible: root.showReadyFirst
                enabled: root.readyFirstEnabled
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredHeight: visible ? Theme.row32 : 0
                padding: 0
                leftPadding: Theme.space15
                rightPadding: Theme.space15
                opacity: enabled ? 1 : 0.45
                Accessible.role: Accessible.CheckBox
                Accessible.checkable: true
                Accessible.checked: root.readyFirst
                onClicked: { const on = !root.readyFirst; menu.close(); root.readyFirstToggled(on) }
                contentItem: Row {
                    spacing: Theme.space1
                    Item {
                        width: 14
                        height: 14
                        anchors.verticalCenter: parent.verticalCenter
                        UzGlyph { anchors.centerIn: parent; visible: root.readyFirst; name: "check"; size: 14; color: Theme.accentText }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Ready first")
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.input.size
                        font.weight: root.readyFirst ? Font.Medium : Font.Normal
                        color: Theme.ink
                    }
                }
                background: Rectangle {
                    color: readyRow.highlighted || readyRow.hovered ? Theme.hover : "transparent"
                    UzFocusRing { inset: true; visible: readyRow.highlighted }
                }
            }
            Text {
                Layout.fillWidth: true
                Layout.topMargin: Theme.space1
                Layout.leftMargin: Theme.space15
                Layout.rightMargin: Theme.space15
                Layout.bottomMargin: Theme.space1
                text: qsTr("Never played games come last. Sort and Ready first are saved on this device.")
                wrapMode: Text.WordWrap
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                color: Theme.dim
            }
        }
    }
}
