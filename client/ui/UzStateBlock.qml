import QtQuick
import QtQuick.Layouts

// StateBlock (design-system.md "StateBlock"): empty / loading / error state in a quiet `rule-2` frame: the ensō (error: a
// 28 px danger square with "!"), a title (14/500), an explanation (12, muted, at most 280 px wide) and one action button.
// The action is the primary button for an empty state and a secondary small button otherwise; `actionClicked()`.
Rectangle {
    id: root
    property string kind: "empty"      // empty | loading | error
    property string title: ""
    property string text: ""
    property string action: ""
    // "done": a static 28 px square with a check instead of the ensō (an empty state that is good news, e.g. "No errors"; the open
    // circle reads as a spinner there). Default: the ensō.
    property string icon: ""
    signal actionClicked()

    implicitHeight: Math.max(220, col.implicitHeight + 2 * Theme.space3)
    implicitWidth: 320
    radius: 0
    color: "transparent"
    border.width: 1
    border.color: Theme.rule2
    Accessible.role: Accessible.StaticText
    Accessible.name: root.title

    ColumnLayout {
        id: col
        anchors.centerIn: parent
        width: Math.min(parent.width - 2 * Theme.space2, 320)
        spacing: 10
        UzEnso { visible: root.kind !== "error" && root.icon !== "done"; size: 48; Layout.alignment: Qt.AlignHCenter }
        Rectangle {
            visible: root.kind !== "error" && root.icon === "done"
            objectName: "stateBlockDone"
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            radius: 0
            color: "transparent"
            border.width: 1
            border.color: Theme.ink2
            UzGlyph { anchors.centerIn: parent; name: "check"; size: 16; strokeWidth: 1.8; color: Theme.ink }
        }
        Rectangle {
            visible: root.kind === "error"
            objectName: "stateBlockError"
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            radius: 0
            color: Theme.danger
            Text { anchors.centerIn: parent; text: "!"; font.family: Theme.sans; font.weight: Font.Bold; font.pixelSize: 15; color: Theme.ground }
        }
        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.space05
            Layout.maximumWidth: col.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: root.title
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.itemTitle.size
            font.weight: Font.Medium
            color: Theme.ink
        }
        Text {
            visible: root.text !== ""
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: Math.min(280, col.width)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: root.text
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            lineHeight: 18
            lineHeightMode: Text.FixedHeight
            color: Theme.muted
        }
        UzButton {
            objectName: "stateBlockAction"
            visible: root.action !== ""
            Layout.alignment: Qt.AlignHCenter
            kind: root.kind === "empty" ? "primary" : "outline"
            size: "sm"
            text: root.action
            onClicked: root.actionClicked()
        }
    }
}
