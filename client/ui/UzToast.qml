import QtQuick
import QtQuick.Layouts

// Toast (design-system.md "Toast"): a 44 px notice on `surface` in a 1 px `rule` frame (no shadow): StatusIcon (done / warning
// / error), message 13, an optional action (ink, underlined, 500), an optional Mono shortcut and a close mark. Bottom left of the
// stage in game. `action` empty hides the action link; `closable: false` hides the close mark. `maxLines` above 1 wraps a long
// message onto that many lines (the toast grows past 44 px); the owner limits the width.
Rectangle {
    id: root
    property string kind: "done"        // done | warning | error
    property string text: ""
    property string action: ""
    property string shortcut: ""
    property bool closable: true
    property int maxLines: 1
    signal actionTriggered()
    signal closeRequested()

    implicitHeight: Math.max(Theme.row44, toastText.implicitHeight + 2 * Theme.space15)
    implicitWidth: Math.max(240, content.implicitWidth + Theme.space15 + 14)
    radius: 0
    color: Theme.surface
    border.width: 1
    border.color: Theme.rule
    Accessible.role: Accessible.AlertMessage
    Accessible.name: root.text

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.leftMargin: Theme.space15
        anchors.rightMargin: 14
        spacing: Theme.space15
        UzStatusIcon { kind: root.kind; Layout.alignment: root.maxLines > 1 ? Qt.AlignTop : Qt.AlignVCenter; Layout.topMargin: root.maxLines > 1 ? Theme.space15 : 0 }
        Text {
            id: toastText
            objectName: "toastText"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            elide: Text.ElideRight
            wrapMode: root.maxLines > 1 ? Text.Wrap : Text.NoWrap
            maximumLineCount: root.maxLines
            text: root.text
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: Theme.ink
        }
        Item {
            id: actionBox
            objectName: "toastAction"
            visible: root.action !== ""
            Layout.preferredWidth: actionText.implicitWidth + Theme.space3
            Layout.preferredHeight: parent.height
            activeFocusOnTab: true
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) { root.actionTriggered(); event.accepted = true }
            }
            Text {
                id: actionText
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.action
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                font.weight: Font.Medium
                font.underline: true
                color: Theme.ink
            }
            TapHandler { onTapped: root.actionTriggered() }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            UzFocusRing { visible: actionBox.activeFocus; anchors.fill: undefined; x: actionBox.width - actionText.width - 2; y: 8; width: actionText.width + 4; height: actionBox.height - 16 }
        }
        Text {
            visible: root.shortcut !== ""
            text: root.shortcut
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            color: Theme.muted
        }
        UzIconButton {
            id: closeBox
            objectName: "toastClose"
            visible: root.closable
            Layout.preferredWidth: Theme.targetMin
            Layout.preferredHeight: Theme.targetMin
            glyph: "cross"
            glyphSize: 12
            text: qsTr("Dismiss")
            onClicked: root.closeRequested()
        }
    }
}
