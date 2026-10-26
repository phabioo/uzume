import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Frame of every admin page (accounts-and-server.md "Rules"): 48 px top padding, 56 px left margin, the page header (Mono label,
// Light 40 title, header actions at the far right), an optional notice area under it, scrolling content and, in wide
// windows, the help column in the open column on the right (label `UZUME · <AREA> · <SERVER NAME>` from the real server name).
// While "Confirm it's you" is valid the notice area starts with "Confirmed · 4:12 left" (8f-2).
//
//   AdminPage {
//       player: root.player
//       title: qsTr("Home server")              // the page title
//       label: "0.18.0 · UP 2 DAYS"            // Mono label above the title (shown uppercase)
//       area: qsTr("Status")                    // names the help column
//       helpItems: [{ title: qsTr("..."), text: qsTr("...") }]   // UzHelpColumn paragraphs; empty = no help column
//       helpLines: ["..."]                      // Mono lines at the foot of the help column
//       headerActions: [ UzButton { ... } ]     // buttons next to the title
//       notice: [ UzStatusIcon { ... } ]        // lines under the header (problems, results of actions)
//       UzTable { ... }                         // everything else is the content, spaced by 32 px
//   }
// objectNames: adminPage (the frame), adminPageScroll, adminPageContent, adminHelp, confirmedNote.
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
    property alias headerActions: header.actions
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
            y: 48
            width: Math.min(Theme.contentMaxWidth, flick.width - root.pageMargin - (root.helpShown ? Theme.space7 + Theme.openColumn + Theme.space3 : root.pageMargin))
            spacing: Theme.space4

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space2
                UzPageHeader {
                    id: header
                    Layout.fillWidth: true
                    label: root.label
                    title: root.title
                }
                ColumnLayout {
                    id: noticeBox
                    Layout.fillWidth: true
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
