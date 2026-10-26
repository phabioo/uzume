import QtQuick

// Section label (design-system.md "SectionLabel"): Plex Mono 500 10/16, +14 %, uppercase, `dim`: a two-digit number, an em
// dash and the name ("01 — LIBRARY"); `meta` sits at the far right (+6 %). Without a number only the name. Below 1440 px
// (Theme.columnsCompactMax) the number is dropped unless `keepNumber` is set (first-start, empty and waiting states, panels).
Item {
    id: root
    property int number: -1
    property string text: ""
    property string meta: ""
    property bool keepNumber: false
    readonly property bool numbered: number >= 0 && (keepNumber || Window.width >= Theme.columnsCompactMax)
    readonly property string composed: (numbered ? String(number).padStart(2, "0") + " — " : "") + text
    implicitHeight: 16
    implicitWidth: label.implicitWidth + (metaText.visible ? metaText.implicitWidth + Theme.space2 : 0)
    Accessible.role: Accessible.StaticText
    Accessible.name: root.composed

    Text {
        id: label
        objectName: "sectionLabelText"
        anchors.left: parent.left
        anchors.right: metaText.visible ? metaText.left : parent.right
        anchors.rightMargin: metaText.visible ? Theme.space2 : 0
        elide: Text.ElideRight
        text: root.composed
        textFormat: Text.PlainText
        font.family: Theme.mono
        font.pixelSize: Theme.roles.sectionLabel.size
        font.weight: Font.Medium
        font.letterSpacing: Theme.roles.sectionLabel.spacing
        font.capitalization: Font.AllUppercase
        color: Theme.muted
    }
    Text {
        id: metaText
        visible: root.meta !== ""
        anchors.right: parent.right
        text: root.meta
        textFormat: Text.PlainText
        font.family: Theme.mono
        font.pixelSize: Theme.roles.sectionLabel.size
        font.weight: Font.Medium
        font.letterSpacing: Theme.roles.meta.spacing
        font.capitalization: Font.AllUppercase
        color: Theme.muted
    }
}
