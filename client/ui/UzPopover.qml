import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Popover (design-system.md "Popover"): a small anchored surface: `surface` plane, 1 px `rule` frame, no pointer and no
// shadow. Optional Mono label and title with a close button, content (default slot), an action row and hint keys; closes on
// Esc, B (Back) and a click outside. Opens 4 px below (or above) its anchor; used for volume, the bell, the Multiview mixer and
// the map's route card. `closeRequested()` fires for every way of closing.
//   popover.openFor(volumeButton, "below", "end")
Popup {
    id: root
    property string label: ""
    property string title: ""
    property int popWidth: 280
    default property alias content: body.data
    property alias actions: actionRow.data
    property alias hints: hintRow.data
    signal closeRequested()

    function openFor(anchor: Item, placement: string, align: string) {
        parent = anchor
        const ap = placement === "above" ? "above" : "below"
        const gap = Theme.space05
        const w = popWidth
        x = align === "end" ? anchor.width - w : align === "center" ? (anchor.width - w) / 2 : 0
        y = ap === "above" ? -height - gap : anchor.height + gap
        open()
    }

    width: popWidth
    padding: 0
    margins: Theme.space1
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: root.closeRequested()

    background: Rectangle {
        radius: 0
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
    }
    contentItem: ColumnLayout {
        spacing: 0
        focus: true
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Back) { root.close(); event.accepted = true }
        }
        Item {   // head
            visible: root.title !== "" || root.label !== ""
            Layout.fillWidth: true
            implicitHeight: headCol.implicitHeight + Theme.space15 + Theme.space1
            Column {
                id: headCol
                x: Theme.space2
                y: Theme.space15
                width: parent.width - Theme.space2 - 36
                Text {
                    visible: root.label !== ""
                    width: parent.width
                    elide: Text.ElideRight
                    text: root.label
                    textFormat: Text.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta.size
                    font.weight: Font.Medium
                    font.letterSpacing: 1.4
                    font.capitalization: Font.AllUppercase
                    color: Theme.muted
                }
                Text {
                    objectName: "popoverTitle"
                    width: parent.width
                    elide: Text.ElideRight
                    text: root.title
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    lineHeight: 18
                    lineHeightMode: Text.FixedHeight
                    color: Theme.ink
                }
            }
            UzIconButton {
                objectName: "popoverClose"
                x: parent.width - width - Theme.space05
                y: Theme.space05 + 2
                width: Theme.targetMin
                height: Theme.targetMin
                glyph: "cross"
                glyphSize: 12
                Accessible.name: qsTr("Close")
                onClicked: root.close()
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
        }
        ColumnLayout {
            id: body
            Layout.fillWidth: true
            Layout.margins: Theme.space2
            Layout.topMargin: Theme.space15
            Layout.bottomMargin: Theme.space15
            spacing: Theme.space15
        }
        Item {
            visible: actionRow.children.length > 0
            Layout.fillWidth: true
            implicitHeight: actionRow.implicitHeight + 2 * Theme.space15 + 1
            Rectangle { width: parent.width; height: 1; color: Theme.rule2 }
            Row { id: actionRow; x: Theme.space2; y: Theme.space15 + 1; spacing: Theme.space1 }
        }
        Item {
            visible: hintRow.children.length > 0
            Layout.fillWidth: true
            implicitHeight: hintRow.implicitHeight + 2 * Theme.space1 + 1
            Rectangle { width: parent.width; height: 1; color: Theme.rule2 }
            Row { id: hintRow; x: Theme.space2; y: Theme.space1 + 1; spacing: Theme.space2 }
        }
    }
}
