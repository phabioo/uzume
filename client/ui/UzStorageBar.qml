import QtQuick
import QtQuick.Layouts
import Uzume.App

// Storage bar block (design-system.md "Storage bar", accounts-and-server.md 3q-app-6 / play-and-plugins.md 6l-1): one 8 px bar on
// rule-2 whose kinds are ink tints (72 / 52 / 36 / 24 / 14 %, never hues), and a hairline legend list with the values so the bar is
// never read alone. Used by Settings > This PC > Storage and Server > Storage.
// `kinds`: [{id, label, text, bytes}] in bar order (the tint follows the position); the segments are bytes / totalBytes of the width.
// `extraRows`: [{id, label, text}] legend rows without a segment (e.g. the backup disk); `freeText` adds the "Free" row (empty = none).
// An entry may carry `name` to override the objectName of its value text (default <rowPrefix><id>).
// `otherBytes` / `otherText`: used space on the disk that is not Uzume's own data (disk used minus the kinds); drawn as a neutral
// segment after the kinds with its own legend row, so the bar shows the real disk usage and the kinds are Uzume's share of it.
// objectNames: <otherName> (the value of the "Other data" row), <barName> (the bar), <rowPrefix><id> (the value text of a kind row), <freeName> (the value of the Free row).
ColumnLayout {
    id: root
    property var kinds: []
    property real totalBytes: 0
    property string freeText: ""
    property real otherBytes: 0
    property string otherText: ""
    property string otherName: "storageOther"
    property var extraRows: []
    property string barName: "storageBar"
    property string rowPrefix: "storageKind_"
    property string freeName: "storageFree"
    readonly property var tints: [0.72, 0.52, 0.36, 0.24, 0.14]
    spacing: Theme.space1

    Rectangle {
        objectName: root.barName
        Layout.fillWidth: true
        height: 8
        color: Theme.rule2
        clip: true
        Row {
            anchors.fill: parent
            Repeater {
                model: root.kinds
                delegate: Rectangle {
                    required property int index
                    required property var modelData
                    height: parent.height
                    width: root.totalBytes > 0 ? Math.min(parent.width, parent.width * modelData.bytes / root.totalBytes) : 0
                    color: Qt.alpha(Theme.ink, root.tints[index % root.tints.length])
                }
            }
            Rectangle {
                objectName: root.barName + "_other"
                visible: root.otherBytes > 0
                height: parent.height
                width: root.totalBytes > 0 ? Math.min(parent.width, parent.width * root.otherBytes / root.totalBytes) : 0
                color: Theme.rule
            }
        }
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        Repeater {
            model: root.kinds
            delegate: Item {
                required property int index
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: 36
                Rectangle {
                    id: swatch
                    anchors.verticalCenter: parent.verticalCenter
                    width: 8
                    height: 8
                    color: Qt.alpha(Theme.ink, root.tints[index % root.tints.length])
                }
                Text {
                    anchors.left: swatch.right
                    anchors.leftMargin: Theme.space15
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.ink
                }
                Text {
                    objectName: modelData.name ?? (root.rowPrefix + modelData.id)
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.text
                    textFormat: Text.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.ink2
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
            }
        }
        Item {
            visible: root.otherBytes > 0 && root.otherText !== ""
            Layout.fillWidth: true
            implicitHeight: 36
            Rectangle {
                id: otherSwatch
                anchors.verticalCenter: parent.verticalCenter
                width: 8
                height: 8
                color: Theme.rule
            }
            Text {
                anchors.left: otherSwatch.right
                anchors.leftMargin: Theme.space15
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Other data on this disk")
                textFormat: Text.PlainText
                font.pixelSize: Theme.roles.input.size
                color: Theme.ink
            }
            Text {
                objectName: root.otherName
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.otherText
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.input.size
                color: Theme.ink2
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
        }
        Item {
            visible: root.freeText !== ""
            Layout.fillWidth: true
            implicitHeight: 36
            Rectangle {
                id: freeSwatch
                anchors.verticalCenter: parent.verticalCenter
                width: 8
                height: 8
                color: Theme.rule2
            }
            Text {
                anchors.left: freeSwatch.right
                anchors.leftMargin: Theme.space15
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Free")
                textFormat: Text.PlainText
                font.pixelSize: Theme.roles.input.size
                color: Theme.ink
            }
            Text {
                objectName: root.freeName
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: root.freeText
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.input.size
                color: Theme.ink2
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
        }
        Repeater {
            model: root.extraRows
            delegate: Item {
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: 36
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.label
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.ink
                }
                Text {
                    objectName: modelData.name ?? (root.rowPrefix + modelData.id)
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.text
                    textFormat: Text.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.ink2
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
            }
        }
    }
}
