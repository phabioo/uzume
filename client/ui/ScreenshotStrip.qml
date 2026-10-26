import QtQuick
import QtQuick.Layouts
import Uzume.App

// "Screenshots (N)" of the game page overview (board 3c-1): the person's own screenshots of the game as a horizontal strip of
// thumbnails with their dates, newest first. A server before 0.19 hides the section; none yet shows one short line. A click opens the
// viewer (`openRequested(id)`). Pictures come from image://screenshots/<id> (the app's own connection, a small cache).
// objectNames: screenshotSection, screenshotStrip, screenshotEmpty, screenshotThumb_<id>, screenshotDate_<id>.
ColumnLayout {
    id: root
    required property PlayerController player
    property string gameId: ""
    readonly property ScreenshotController shots: player.screenshots
    signal openRequested(string screenshotId)

    objectName: "screenshotSection"
    visible: root.shots.supported && root.gameId !== ""
    Layout.fillWidth: true
    Layout.minimumWidth: 0
    spacing: Theme.space1

    onGameIdChanged: root.shots.loadGame(root.gameId)
    Component.onCompleted: root.shots.loadGame(root.gameId)

    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule }
    UzSectionLabel {
        objectName: "screenshotLabel"
        Layout.fillWidth: true
        Layout.topMargin: Theme.space05
        text: qsTr("Screenshots (%1)").arg(root.shots.count)
        meta: root.shots.pendingCount > 0 ? qsTr("%1 waiting to upload").arg(root.shots.pendingCount) : ""
    }
    UzLabel {
        objectName: "screenshotEmpty"
        visible: root.shots.count === 0
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.Wrap
        text: qsTr("No screenshots yet. Take one in the game with %1.").arg(root.player.controllers.hotkeyHints.screenshot ?? "F12")
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
    }
    ListView {
        id: strip
        objectName: "screenshotStrip"
        visible: root.shots.count > 0
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.preferredHeight: 72 + 20 + Theme.space05
        orientation: ListView.Horizontal
        spacing: Theme.space1
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.shots.shots
        delegate: Item {
            id: cell
            required property var modelData
            width: 112
            height: strip.height
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Screenshot of %1").arg(cell.modelData.dateText)
            Accessible.onPressAction: root.openRequested(cell.modelData.id)
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                    root.openRequested(cell.modelData.id)
                    event.accepted = true
                }
            }
            onActiveFocusChanged: if (cell.activeFocus) strip.positionViewAtIndex(index, ListView.Contain)
            Rectangle {
                id: frame
                objectName: "screenshotThumb_" + cell.modelData.id
                width: parent.width
                height: 72
                color: Theme.stage
                border.width: 1
                border.color: thumbHover.hovered ? Theme.ink2 : Theme.rule
                Image {
                    anchors.fill: parent
                    anchors.margins: 1
                    asynchronous: true
                    cache: false   // the app keeps its own small cache
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(224, 144)
                    source: "image://screenshots/" + encodeURIComponent(cell.modelData.id)
                }
                UzFocusRing { visible: cell.activeFocus; inset: true }
                HoverHandler { id: thumbHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.openRequested(cell.modelData.id) }
            }
            UzMono {
                objectName: "screenshotDate_" + cell.modelData.id
                anchors.top: frame.bottom
                anchors.topMargin: Theme.space05
                width: parent.width
                text: cell.modelData.dateText
                elide: Text.ElideRight
                color: Theme.dim
                font.pixelSize: Theme.roles.sectionLabel.size
            }
        }
    }
}
