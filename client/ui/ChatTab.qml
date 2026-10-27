import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Chat tab of the game panel (board 3g-7, ADR 0033 D4): the header "RUN CHAT · <RUN> · N PEOPLE" / "SESSION CHAT · N PEOPLE", the messages with
// day separators (newest at the bottom), the run's events as dimmed lines, the quick replies (Down!, Gym next, Wait; Alt+1 / 2 / 3 while the
// composer has the focus) and the one-line composer. Enter sends, Esc gives the keys back to the game. The text stays as typed (emote
// shortcodes too); only the display replaces :gg: :wave: :lol: :hype: :sad: :ok:.
ColumnLayout {
    id: root
    required property PlayerController player
    readonly property ChatController chat: player.chat
    property bool tabOpen: false          // this tab is the open one and the panel is shown
    readonly property bool composerFocused: composer.activeFocus
    signal focusGameRequested()
    spacing: 0

    function focusComposer() { composer.forceActiveFocus() }
    function sendNow() {
        if (root.chat.send(composer.text)) composer.text = ""
    }

    Binding { target: root.chat; property: "panelVisible"; value: root.tabOpen && root.visible }

    UzSectionLabel {
        objectName: "chatHeader"
        Layout.fillWidth: true
        Layout.topMargin: Theme.space2
        Layout.leftMargin: Theme.space3
        Layout.rightMargin: Theme.space3
        visible: root.chat.available
        text: root.chat.header
    }
    Rectangle { Layout.fillWidth: true; Layout.topMargin: Theme.space15; visible: root.chat.available; implicitHeight: 1; color: Theme.rule2 }

    Text {
        objectName: "chatNoScope"
        Layout.fillWidth: true
        Layout.margins: Theme.space3
        visible: !root.chat.available
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        text: root.player.gameRun.id ? qsTr("Chat opens when the run is active.") : qsTr("Chat opens when you play a run, share this game or watch a Session.")
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
        color: Theme.muted
    }
    Item { Layout.fillHeight: true; visible: !root.chat.available }

    ListView {
        id: list
        objectName: "chatList"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 0
        visible: root.chat.available
        clip: true
        verticalLayoutDirection: ListView.BottomToTop   // the rows come newest first: the list sticks to the newest message
        boundsBehavior: Flickable.StopAtBounds
        model: root.chat.rows
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        delegate: Loader {
            id: rowLoader
            required property var modelData
            width: ListView.view.width
            height: item ? item.implicitHeight : 0
            sourceComponent: modelData.type === "msg" ? msgComp : modelData.type === "event" ? eventComp : dayComp
            Component {
                id: msgComp
                ChatMessage {
                    objectName: "chatRow"
                    row: rowLoader.modelData
                    width: rowLoader.width
                    onRetryRequested: (cid) => root.chat.retry(cid)
                }
            }
            Component {
                id: eventComp
                Item {
                    objectName: "chatEvent"
                    width: rowLoader.width
                    implicitHeight: evText.implicitHeight + Theme.space1
                    Text {
                        id: evText
                        objectName: "chatEventText"
                        x: Theme.space3
                        y: Theme.space05
                        width: parent.width - 2 * Theme.space3
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        text: rowLoader.modelData.text + " · " + rowLoader.modelData.time
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        lineHeight: Theme.roles.small.line
                        lineHeightMode: Text.FixedHeight
                        color: Theme.dim
                    }
                }
            }
            Component {
                id: dayComp
                Item {
                    objectName: "chatDay"
                    width: rowLoader.width
                    implicitHeight: 28
                    Rectangle { x: Theme.space3; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 2 * Theme.space3; height: 1; color: Theme.rule2 }
                    Rectangle {
                        anchors.centerIn: parent
                        width: dayText.implicitWidth + Theme.space2
                        height: parent.height
                        color: Theme.ground
                        UzMono {
                            id: dayText
                            anchors.centerIn: parent
                            text: rowLoader.modelData.text
                            font.pixelSize: 10
                            font.letterSpacing: 1.4
                            color: Theme.dim
                        }
                    }
                }
            }
        }
        Text {
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.space3
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            visible: list.count === 0
            textFormat: Text.PlainText
            text: root.chat.loading ? qsTr("Loading…") : qsTr("No messages yet. Say hi.")
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
    }

    // Quick replies: three framed buttons with their keys
    RowLayout {
        objectName: "chatQuickReplies"
        Layout.fillWidth: true
        Layout.leftMargin: Theme.space3
        Layout.rightMargin: Theme.space3
        Layout.topMargin: Theme.space1
        visible: root.chat.available
        spacing: Theme.space1
        Repeater {
            model: root.chat.quickReplies
            delegate: UzButton {
                required property var modelData
                required property int index
                objectName: "chatQuick_" + (index + 1)
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.minimumWidth: 0
                size: "sm"
                hPadding: 12
                focusPolicy: Qt.TabFocus
                text: modelData.label
                value: modelData.key
                enabled: root.chat.canSend
                onClicked: { root.chat.sendQuick(index); composer.forceActiveFocus() }
            }
        }
    }

    // Composer: one line, Enter sends, Esc returns to the game; the counter shows near the limit
    Item {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.space3
        Layout.rightMargin: Theme.space3
        Layout.topMargin: Theme.space1
        Layout.bottomMargin: Theme.space15
        visible: root.chat.available
        implicitHeight: Theme.row36
        UzField {
            id: composer
            objectName: "chatComposer"
            anchors.fill: parent
            rightPadding: counter.visible ? counter.implicitWidth + Theme.space15 + 4 : Theme.space15
            placeholderText: qsTr("Message")
            accessibleName: qsTr("Chat message")
            enabled: root.chat.canSend || text !== ""
            readonly property int count: root.chat.charCount(text)
            invalid: count > root.chat.maxText
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    root.sendNow()
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    root.focusGameRequested()
                    event.accepted = true
                } else if ((event.modifiers & Qt.AltModifier) && event.key >= Qt.Key_1 && event.key <= Qt.Key_3) {
                    root.chat.sendQuick(event.key - Qt.Key_1)
                    event.accepted = true
                }
            }
            UzMono {
                id: counter
                objectName: "chatCounter"
                anchors.right: parent.right
                anchors.rightMargin: Theme.space15
                anchors.verticalCenter: parent.verticalCenter
                visible: composer.count >= root.chat.maxText - 50
                text: composer.count + "/" + root.chat.maxText
                font.pixelSize: 10
                color: composer.count > root.chat.maxText ? Theme.danger : Theme.muted
            }
        }
    }
}
