import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Tile of the Library grid (3c-2-r6): a 4:3 placeholder with a two-letter monogram and the system chip at the top left, the
// title (14 / 500, one line) and a status line: warning square + word when the game needs attention, else the muted word, with
// the play time of a ready game ("Ready · 12 h"). The selected tile has an `ink` frame; the focus ring is the tile's own.
// Same role contract as GameTile, so GameGrid can use either as its delegate.
Item {
    id: card
    objectName: "gameTile_" + gameId
    required property int index
    required property string gameId
    required property string title
    required property string system
    required property string monogram
    required property string stateKind
    required property string tileText
    required property string tileTone
    required property string statusWord
    required property bool statusWarn
    required property real progress
    required property bool onHub
    required property bool localFile
    required property string addedText
    required property string playTimeText
    required property string lastPlayedText
    property bool showAdded: false
    property bool showPersonal: false
    property bool selected: false
    property bool running: false
    property int numberOffset: 0
    readonly property int gapX: 24
    readonly property int gapY: 28
    readonly property real artWidth: Math.max(0, width - gapX)

    signal clicked()
    signal navigate(int dRow, int dCol)

    readonly property string line: {
        if (card.running) return qsTr("Running · paused")
        if (card.statusWarn || card.stateKind !== "ready" || !card.showPersonal) return card.statusWord
        return card.playTimeText === "–" ? card.statusWord + " · " + qsTr("never played") : card.statusWord + " · " + card.playTimeText
    }

    Rectangle {   // 4:3 placeholder
        id: art
        objectName: "tileArt"
        width: card.artWidth
        height: Math.round(card.artWidth * 3 / 4)
        color: Theme.placeholder
        border.width: 1
        border.color: card.selected || hover.hovered ? Theme.ink : Theme.ruleControl
        UzLabel {
            anchors.centerIn: parent
            text: card.monogram
            font.pixelSize: 34
            font.weight: Font.Light
            color: Theme.ink2
        }
        UzFocusRing {
            objectName: "tileFocusRing"
            visible: card.activeFocus
        }
        UzSystemChip {
            x: -1; y: -1
            system: card.system
            visible: card.system !== ""
        }
        Rectangle {   // download progress
            visible: card.stateKind === "downloading"
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            height: 3
            width: parent.width * Math.max(0, Math.min(1, card.progress))
            color: Theme.accent
        }
        UzTag {
            objectName: "tileRunningPill"
            visible: card.running
            small: true
            tone: "warn"
            text: qsTr("Paused")
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Theme.space1
        }
    }
    Column {
        anchors.top: art.bottom
        anchors.topMargin: 10
        width: card.artWidth
        spacing: 4
        UzLabel {
            objectName: "tileTitle"
            width: parent.width
            text: card.title
            font.pixelSize: Theme.roles.input.size
            font.weight: Font.Medium
            elide: Text.ElideRight
            HoverHandler { id: titleHover }
            ToolTip.visible: titleHover.hovered && truncated
            ToolTip.text: text
            ToolTip.delay: 400
        }
        Row {
            spacing: 6
            width: parent.width
            Rectangle {
                objectName: "tileWarn"
                visible: card.statusWarn && !card.running
                anchors.verticalCenter: parent.verticalCenter
                width: 12; height: 12
                color: Theme.accent
                Text { anchors.centerIn: parent; text: "!"; font.family: Theme.sans; font.pixelSize: 9; font.weight: Font.Bold; color: Theme.inkOnAccent }
            }
            UzLabel {
                objectName: "tileStatus"
                width: Math.max(0, parent.width - (card.statusWarn && !card.running ? 18 : 0))
                text: card.line
                font.pixelSize: Theme.roles.small.size
                color: card.statusWarn ? Theme.ink : Theme.muted
                elide: Text.ElideRight
            }
        }
    }

    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: card.title + ", " + card.system + ", " + card.line
    Accessible.onPressAction: card.clicked()
    Keys.onPressed: (event) => {
        switch (event.key) {
        case Qt.Key_Return: case Qt.Key_Enter: case Qt.Key_Space: card.clicked(); event.accepted = true; break
        case Qt.Key_Up: card.navigate(-1, 0); event.accepted = true; break
        case Qt.Key_Down: card.navigate(1, 0); event.accepted = true; break
        case Qt.Key_Left: card.navigate(0, -1); event.accepted = true; break
        case Qt.Key_Right: card.navigate(0, 1); event.accepted = true; break
        }
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: card.clicked() }
}
