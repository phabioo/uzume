import QtQuick
import QtQuick.Controls.Basic
import Uzume.App

// Select field (design-system.md "Select"): 36 px, `rule` frame, chevron; the list is a menu on `surface` with a `rule` frame
// (selected item: check + 2 px Yamabuki marker). options [{ value, label, hint?, disabled? }], current = selected value; picked(value) on user
// choice. hint is a muted second part of the line ("Nuzlocke friendly · same engine"); a disabled option is dimmed and cannot be picked.
ComboBox {
    id: control
    property string accessibleName: ""   // spoken name of the field (the label next to it)
    Accessible.name: control.accessibleName !== "" ? control.accessibleName : control.displayText
    property string current: ""
    signal picked(string value)

    implicitHeight: Theme.row36
    implicitWidth: Theme.menu
    textRole: "label"
    valueRole: "value"
    currentIndex: {
        for (let i = 0; i < control.count; ++i) {
            if (control.model[i] && control.model[i].value === control.current) {
                return i
            }
        }
        return -1
    }
    onActivated: index => control.picked(control.model[index].value)

    font.family: Theme.sans
    font.pixelSize: Theme.roles.input.size
    leftPadding: Theme.space15
    rightPadding: 34

    contentItem: Text {
        text: control.displayText
        textFormat: Text.PlainText
        font: control.font
        color: control.enabled ? Theme.ink : Theme.disabledInk
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
    indicator: UzGlyph {
        x: control.width - width - Theme.space15
        y: (control.height - height) / 2
        name: control.popup.visible ? "chevronUp" : "chevronDown"
        size: 14
        color: control.enabled ? Theme.muted : Theme.disabledInk
    }
    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: 0
            color: !control.enabled ? Theme.disabledGround : (control.popup.visible ? Theme.pressed : (Theme.dark ? "transparent" : Theme.surface))
            border.width: 1
            border.color: !control.enabled ? Theme.disabledRule : (control.popup.visible ? Theme.ink : (control.hovered ? Theme.ink2 : Theme.ruleControl))
            Behavior on border.color { ColorAnimation { duration: Theme.durFast } }
        }
        UzFocusRing { visible: control.visualFocus }
    }
    delegate: ItemDelegate {
        id: item
        required property var modelData
        required property int index
        readonly property bool selected: item.modelData.value === control.current
        width: control.width - 2
        height: Theme.row32
        enabled: item.modelData.disabled !== true
        padding: 0
        leftPadding: Theme.space15
        rightPadding: Theme.space15
        highlighted: control.highlightedIndex === item.index
        contentItem: Row {
            spacing: Theme.space1
            Item {
                width: 14; height: 14
                anchors.verticalCenter: parent.verticalCenter
                UzGlyph { anchors.centerIn: parent; visible: item.selected; name: "check"; size: 14; color: Theme.ink }
            }
            Text {
                width: parent.width - 14 - parent.spacing
                anchors.verticalCenter: parent.verticalCenter
                text: item.modelData.hint ? item.modelData.label + "  ·  " + item.modelData.hint : item.modelData.label
                textFormat: Text.PlainText
                font.family: control.font.family
                font.pixelSize: control.font.pixelSize
                font.weight: item.selected ? Font.Medium : Font.Normal
                color: item.enabled ? Theme.ink : Theme.disabledInk
                elide: Text.ElideRight
            }
        }
        background: Rectangle {
            color: item.highlighted || item.hovered ? Theme.hover : (item.selected ? Theme.selected : "transparent")
            Rectangle { visible: item.selected; width: 2; height: parent.height; color: Theme.ink }
            UzFocusRing { inset: true; visible: item.highlighted }
        }
    }
    popup: Popup {
        y: control.height + Theme.space05
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 2 + 2 * Theme.space05, 8 * Theme.row32 + 2 + 2 * Theme.space05)
        padding: 1
        topPadding: 1 + Theme.space05
        bottomPadding: 1 + Theme.space05
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator { }
        }
        background: Rectangle {
            radius: 0
            color: Theme.surface
            border.width: 1
            border.color: Theme.rule
        }
    }
}
