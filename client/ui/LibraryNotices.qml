import QtQuick
import QtQuick.Layouts
import Uzume.App

// The one notices line of the Library (32 px, hairlines above and below): the most important core notice as a sentence with its
// link on the right. More notices fold into "+N more", which expands them as further lines of the same kind. Merges the core
// download info, the core problems and the core version warnings (PlayerController.coreWarnings); problems come first.
// objectNames: libraryNotices (the block), libraryNotice (each line), libraryNoticeText, libraryNoticeMore, libraryNoticeAction,
// libraryNoticeDismiss.
ColumnLayout {
    id: root
    objectName: "libraryNotices"
    required property PlayerController player
    property bool expanded: false
    // Problems before the info notice (the sort is stable).
    readonly property var notices: {
        const all = root.player.coreWarnings
        return all.filter(w => w.tone !== "info").concat(all.filter(w => w.tone === "info"))
    }
    readonly property var shown: root.expanded ? notices : notices.slice(0, 1)
    visible: notices.length > 0
    spacing: 0
    onNoticesChanged: if (notices.length < 2) root.expanded = false

    Repeater {
        model: root.shown
        delegate: Item {
            id: line
            required property var modelData
            required property int index
            objectName: "libraryNotice"
            readonly property bool info: modelData.tone === "info"
            readonly property bool dismissible: modelData.code === "core_download"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.row32
            Rectangle { width: parent.width; height: 1; color: Theme.rule2; visible: line.index === 0 }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space05
                anchors.rightMargin: Theme.space05
                spacing: Theme.space15
                Rectangle {   // marker: Yamabuki for a problem, neutral for information
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    color: line.info ? Theme.dim : Theme.accent
                }
                UzLabel {
                    objectName: "libraryNoticeText"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: line.modelData.text
                    elide: Text.ElideRight
                    color: line.info ? Theme.ink2 : Theme.ink
                    font.pixelSize: Theme.roles.small.size
                }
                UzButton {
                    objectName: "libraryNoticeMore"
                    Layout.fillWidth: false
                    Layout.minimumWidth: implicitWidth
                    visible: line.index === 0 && root.notices.length > 1
                    kind: "link"
                    text: root.expanded ? qsTr("Show less") : qsTr("+%1 more").arg(root.notices.length - 1)
                    onClicked: root.expanded = !root.expanded
                }
                UzButton {
                    objectName: "libraryNoticeAction"
                    Layout.fillWidth: false
                    Layout.minimumWidth: implicitWidth
                    visible: root.player.isAdmin
                    kind: "link"
                    text: qsTr("Open Systems & cores")
                    onClicked: root.player.showServerPage("systems")
                }
                UzIconButton {
                    objectName: "libraryNoticeDismiss"
                    visible: line.dismissible
                    Layout.preferredWidth: Theme.row28
                    Layout.preferredHeight: Theme.row28
                    glyph: "cross"
                    glyphSize: 12
                    Accessible.name: qsTr("Dismiss")
                    onClicked: root.player.dismissCoreNotice(line.modelData.code)
                }
            }
        }
    }
}
