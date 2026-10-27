import QtQuick
import QtQuick.Layouts
import Uzume.App

// 8c-2 Invite link while already on a server: "Add <server>?" with the current and the new server as two rows and the three
// ways out: Add and switch (primary), Add only, Cancel.
StartPage {
    id: root
    required property PlayerController player
    readonly property var ob: player.onboarding
    readonly property var choice: ob.linkChoice
    readonly property string linkedLabel: (choice.linkedName || "") !== "" ? choice.linkedName : (choice.linkedAddress || "")
    screenLabel: qsTr("Invite link")
    openLabel: qsTr("UZUME · INVITE LINK")
    initialFocus: addSwitch
    hints: [{ kbd: "Enter", pad: "A", label: qsTr("Add and switch") }, { kbd: "Esc", pad: "B", label: qsTr("Cancel") }]
    onBackRequested: ob.cancel()

    StartHead {
        step: root.screenLabel
        number: 1
        label: qsTr("Invite link")
        title: qsTr("Add %1?").arg(root.linkedLabel)
        text: qsTr("You stay on %1 too. Switch between them from the server line at the foot of the sidebar.").arg(root.choice.currentName || qsTr("your server"))
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.rule }
        Repeater {
            model: [{ name: "linkCurrent", title: root.choice.currentName || "", sub: (root.choice.currentUser || "") !== "" ? qsTr("Signed in as %1").arg(root.choice.currentUser) : qsTr("Open now"), tag: "" },
                    { name: "linkNew", title: root.linkedLabel, sub: root.choice.linkedAddress || "", tag: qsTr("Code matches") }]
            delegate: Item {
                required property var modelData
                objectName: modelData.name
                Layout.fillWidth: true
                implicitHeight: 64
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space1
                    anchors.rightMargin: Theme.space1
                    spacing: Theme.space2
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text { Layout.fillWidth: true; elide: Text.ElideRight; text: modelData.title; textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.itemTitle.size; font.weight: Font.Medium; color: Theme.ink }
                        Text { Layout.fillWidth: true; elide: Text.ElideRight; text: modelData.sub; textFormat: Text.PlainText; font.family: Theme.mono; font.pixelSize: Theme.roles.address.size; color: Theme.dim }
                    }
                    UzTag { visible: modelData.tag !== ""; tone: "ok"; text: modelData.tag }
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        UzButton { id: addSwitch; objectName: "linkAddAndSwitch"; kind: "primary"; text: qsTr("Add and switch"); onClicked: root.ob.confirmLink(true) }
        UzButton { objectName: "linkAddOnly"; text: qsTr("Add only"); onClicked: root.ob.confirmLink(false) }
        UzButton { objectName: "linkCancel"; kind: "ghost"; text: qsTr("Cancel"); onClicked: root.ob.cancel() }
        Item { Layout.fillWidth: true }
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("One account per server. Saves stay with the server they were made on. Add only keeps the new server in Settings › This PC › Servers and changes nothing now.")
        textFormat: Text.PlainText
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
        color: Theme.muted
    }
}
