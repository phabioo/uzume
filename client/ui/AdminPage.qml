import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Frame of every admin page (accounts-and-server.md "Rules"): the page sits under the header and the tabs of ServerScreen (which
// carry the one page header), so the frame has no page header of its own: a toolbar line with the Mono label (counts), the
// title when it names something other than the tab (a system) and the header actions at the far right, an optional notice area
// under it, scrolling content and, in wide windows, the help column in the open column on the right (label
// `UZUME · <AREA> · <SERVER NAME>` from the real server name).
// While "Confirm it's you" is valid the notice area starts with "Confirmed · 4:12 left" (8f-2).
//
//   AdminPage {
//       player: root.player
//       title: qsTr("Home server")              // shown only when it differs from `area` and the server name
//       label: "3 people · 5 devices"           // Mono line in the toolbar (hidden when it equals `area` or "Server")
//       area: qsTr("Status")                    // names the help column
//       helpItems: [{ title: qsTr("..."), text: qsTr("...") }]   // UzHelpColumn paragraphs; empty = no help column
//       helpLines: ["..."]                      // Mono lines at the foot of the help column
//       headerActions: [ UzButton { ... } ]     // buttons next to the title
//       notice: [ UzStatusIcon { ... } ]        // lines under the header (problems, results of actions)
//       UzTable { ... }                         // everything else is the content, spaced by 32 px
//   }
// objectNames: adminPage (the frame), adminPageScroll, adminPageContent, adminToolbar, adminPageTitle, adminPageLabel, adminHelp,
// confirmedNote.
Item {
    id: root
    objectName: "adminPage"
    required property PlayerController player
    property string title: ""
    property string label: ""
    property string area: ""
    property var helpItems: []
    property var helpLines: []
    default property alias content: body.data
    property alias headerActions: actionRow.data
    property alias notice: noticeBox.data
    property alias helpExtra: help.extra
    readonly property string serverName: root.player.hubName !== "" ? root.player.hubName : qsTr("Server")
    readonly property string helpLabel: "UZUME · " + root.area.toUpperCase() + " · " + root.serverName.toUpperCase()
    // Help: the open column at 1440 px and wider (Theme.columnsCompactMax), a "Help" disclosure at the end of the content below.
    readonly property bool helpColumn: root.helpItems.length > 0 && root.width >= 880 && Window.width >= Theme.columnsCompactMax
    readonly property bool helpShown: root.helpColumn
    property alias scroll: flick
    readonly property real pageMargin: Theme.space7

    Flickable {
        id: flick
        objectName: "adminPageScroll"
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(page.y + page.implicitHeight, help.visible ? help.y + help.implicitHeight : 0) + Theme.space5
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: page
            objectName: "adminPageContent"
            x: root.pageMargin
            y: Theme.space3
            width: Math.min(Theme.contentMaxWidth, flick.width - root.pageMargin - (root.helpShown ? Theme.space7 + Theme.openColumn + Theme.space3 : root.pageMargin))
            spacing: 0   // the blocks below carry their own gaps, so an empty header takes no room

            ColumnLayout {
                id: headerBlock
                readonly property bool shown: toolbar.visible || noticeBox.hasVisible
                Layout.fillWidth: true
                Layout.bottomMargin: headerBlock.shown ? Theme.space4 : 0
                spacing: 0
                RowLayout {
                    id: toolbar
                    objectName: "adminToolbar"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: titleText.visible || labelText.visible || actionRow.children.length > 0
                    spacing: Theme.space3
                    UzLabel {
                        id: titleText
                        objectName: "adminPageTitle"
                        visible: root.title !== "" && root.title !== root.area && root.title !== root.serverName
                        Layout.maximumWidth: Math.max(0, toolbar.width / 2)
                        text: root.title
                        elide: Text.ElideRight
                        color: Theme.ink
                        font.pixelSize: Theme.roles.headerTitle.size
                        font.weight: Theme.roles.headerTitle.weight
                    }
                    UzMono {
                        id: labelText
                        objectName: "adminPageLabel"
                        visible: root.label !== "" && root.label !== root.area && root.label !== qsTr("Server")
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: root.label
                    }
                    Item { Layout.fillWidth: !labelText.visible; Layout.preferredWidth: 0 }
                    Row {
                        id: actionRow
                        objectName: "pageActions"
                        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                        spacing: Theme.space2
                    }
                }
                ColumnLayout {
                    id: noticeBox
                    // True while a notice line is showing (the layout itself stays visible so that this can change).
                    readonly property bool hasVisible: {
                        for (let i = 0; i < noticeBox.children.length; ++i) if (noticeBox.children[i].visible) return true
                        return false
                    }
                    Layout.fillWidth: true
                    Layout.topMargin: toolbar.visible && noticeBox.hasVisible ? Theme.space2 : 0
                    spacing: Theme.space1
                    UzStatusIcon {
                        objectName: "confirmedNote"
                        visible: root.player.admin.confirmSecondsLeft > 0
                        kind: "done"
                        small: true
                        text: qsTr("Confirmed · %1 left").arg(root.player.admin.confirmLeftText)
                    }
                }
            }
            ColumnLayout {
                id: body
                Layout.fillWidth: true
                spacing: Theme.space4
            }
            UzHelpColumn {
                id: helpInline
                Layout.topMargin: Theme.space4
                objectName: "adminHelpDisclosure"
                inline: true
                visible: root.helpItems.length > 0 && !root.helpColumn
                Layout.fillWidth: true
                items: root.helpItems
                lines: root.helpLines
            }
        }
        UzHelpColumn {
            id: help
            objectName: "adminHelp"
            visible: root.helpShown
            x: flick.width - width - Theme.space3
            y: page.y
            label: root.helpLabel
            items: root.helpItems
            lines: root.helpLines
        }
    }
}
