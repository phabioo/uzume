import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Button of the design system (design-system.md "Button"): square, hairline `rule` frame, Yamabuki only for the one primary
// per view. kind: "primary" (Yamabuki plane) | "outline" (= secondary, rule frame) | "ghost" | "danger" (outlined, never filled)
// | "link" (text only, small). size: "md" 36 px | "sm" 32 px. `on` = toggled secondary (pressed plane).
// Feedback: hover and pressed states change at once (short color transition); `busy` shows a spinner and blocks
// repeated clicks while network work runs; `busyOnClick` shows it right on the click until the state arrives
// (or after at most 1.5 s), so a button that starts network work never looks dead.
Button {
    id: control
    property string kind: "outline"
    property string size: "md"
    property bool on: false
    property bool busy: false
    property int textFormat: Text.PlainText  // names and titles from peers, plugins and the Hub are never markup
    property bool busyOnClick: false
    readonly property bool working: busy || clickBusy
    property bool clickBusy: false

    implicitHeight: kind === "link" ? Theme.row28 : (size === "sm" ? Theme.row32 : Theme.row36)
    property real hPadding: kind === "link" ? 8 : (size === "sm" ? 24 : 32)   // total horizontal padding around the label (2 x space-1.5 / space-2)
    // Natural label width: from the label's implicitWidth, never from its assigned (possibly capped) width.
    readonly property real labelNaturalWidth: labelItem.implicitWidth + (control.working ? 20 : 0)
    implicitWidth: Math.max(kind === "link" ? 0 : 72, labelNaturalWidth + hPadding)
    padding: 0
    // In a Flow/Row-like container the button never grows past the container (the label elides); layouts size it themselves.
    Binding {
        target: control
        property: "width"
        when: control.parent && control.parent.flow !== undefined && control.parent.width > 0
        value: Math.min(control.implicitWidth, control.parent ? control.parent.width : 0)
    }
    Layout.minimumWidth: kind === "link" ? 0 : implicitWidth
    // A link button may shrink (its label elides) but never grows: fillWidth + maximumWidth = preferred. A non-fill layout item
    // keeps its preferred width and would push the row past a narrow column (very wide fonts).
    Layout.fillWidth: kind === "link"
    Layout.maximumWidth: kind === "link" ? implicitWidth : Number.POSITIVE_INFINITY
    hoverEnabled: true
    font.family: Theme.sans
    font.pixelSize: size === "sm" ? Theme.roles.buttonSm.size : Theme.roles.button.size
    // button: primary and danger 500, secondary and ghost 400 (link keeps 500 like its label always had)
    font.weight: (kind === "primary" || kind === "danger" || kind === "link") ? Font.Medium : Font.Normal
    opacity: 1

    onClicked: if (busyOnClick) { clickBusy = true; busyReset.restart() }
    onBusyChanged: if (!busy) clickBusy = false
    onVisibleChanged: if (!visible) clickBusy = false
    // While work runs, further clicks and key activations are swallowed (the look stays enabled).
    MouseArea { anchors.fill: parent; visible: control.working; z: 10; acceptedButtons: Qt.AllButtons; cursorShape: Qt.BusyCursor }
    Keys.onPressed: (event) => { if (control.working && (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) event.accepted = true }
    Keys.onReleased: (event) => { if (control.working && (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) event.accepted = true }
    Timer { id: busyReset; interval: 1500; onTriggered: control.clickBusy = false }

    contentItem: Item {
        implicitWidth: control.labelNaturalWidth
        implicitHeight: labelRow.implicitHeight
        Row {
            id: labelRow
            anchors.centerIn: parent
            spacing: 8
            UzSpinner {
                visible: control.working
                size: 12
                color: control.kind === "primary" ? Theme.inkOnAccent : (control.kind === "danger" ? Theme.danger : Theme.ink)
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: labelItem
                objectName: "fbButtonLabel"
                textFormat: control.textFormat
                text: control.text
                font: control.font
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                anchors.verticalCenter: parent.verticalCenter
                color: !control.enabled ? Theme.disabledInk
                       : control.kind === "primary" ? Theme.inkOnAccent
                       : control.kind === "danger" ? Theme.danger
                       : control.kind === "link" ? (control.hovered ? Theme.ink : Theme.muted)
                       : Theme.ink
                Behavior on color { ColorAnimation { duration: Theme.durFast } }
                // Elide the label when the button was capped to its Flow/Row-like container.
                Binding {
                    target: labelItem
                    property: "width"
                    when: (control.parent && control.parent.flow !== undefined && control.parent.width > 0) || (control.width > 0 && control.width < control.implicitWidth - 0.5)
                    value: Math.min(labelItem.implicitWidth, Math.max(0, control.width - control.hPadding - (control.working ? 20 : 0)))
                }
            }
        }
    }
    background: Item {
        Rectangle {
            id: plane
            visible: control.kind !== "link"
            anchors.fill: parent
            radius: 0
            readonly property bool primary: control.kind === "primary"
            readonly property bool danger: control.kind === "danger"
            color: !control.enabled ? (control.kind === "ghost" ? "transparent" : Theme.disabledGround)
                 : primary ? (control.down ? Theme.accentPressed : (control.hovered ? Theme.accentHover : Theme.accent))
                 : danger ? (control.down || control.hovered ? Theme.dangerTint : "transparent")
                 : (control.down || control.on ? Theme.pressed : (control.hovered ? Theme.hover : "transparent"))
            border.width: control.kind === "ghost" ? 0 : 1
            border.color: !control.enabled ? Theme.disabledRule
                        : primary ? plane.color
                        : danger ? Theme.danger
                        : (control.hovered || control.down || control.on ? Theme.ink : Theme.ruleControl)
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }
        UzFocusRing { visible: control.visualFocus }
    }
}
