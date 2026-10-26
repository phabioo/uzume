import QtQuick
import QtQuick.Layouts

// AppNav item (frame.md "0-4 G"): 32 px row (28 px compact), Plex Sans 14 muted, `hover` fill, active = ink 500 and the 2 px
// start marker (`marker-nav`: Yamabuki, accent-text on Kinari). `badge` shows a count (neutral `badge` square, `ground` text), `dot` a 6 px Yamabuki square (update available),
// `warnText` a Yamabuki plane with a word ("!").
// Tab reaches it, Enter and Space activate it. `leading` takes an icon for nested plugin entries (`indent` moves the text).
Item {
    id: root
    property string label: ""
    property bool active: false
    property bool compact: false
    property int indent: Theme.navInset
    property int badge: 0
    property string badgeName: ""
    property bool dot: false
    property string warnText: ""      // Yamabuki square with a short word ("!"): a warning on the destination (server status)
    property string warnName: ""
    property string dotName: ""
    default property alias leading: leadRow.data
    signal activated()

    Layout.fillWidth: true
    Layout.minimumWidth: 0
    implicitHeight: compact ? Theme.row28 : Theme.row32
    activeFocusOnTab: true
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            root.activated()
            event.accepted = true
        }
    }
    Accessible.role: Accessible.Button
    Accessible.name: root.label
    Accessible.onPressAction: root.activated()

    Rectangle {
        anchors.fill: parent
        color: hover.hovered ? Theme.hover : "transparent"
        radius: 0
    }
    Rectangle {   // marker-start
        visible: root.active
        width: 2
        height: parent.height
        color: Theme.markerNav
    }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.indent
        anchors.rightMargin: Theme.space2
        spacing: Theme.space1
        Row { id: leadRow; spacing: Theme.space1; Layout.alignment: Qt.AlignVCenter }
        Text {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            elide: Text.ElideRight
            text: root.label
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.body.size
            font.weight: root.active ? Font.Medium : Font.Normal
            color: root.active ? Theme.ink : (hover.hovered ? Theme.ink2 : Theme.muted)
        }
        Rectangle {
            objectName: root.dotName
            visible: root.dot
            Layout.preferredWidth: 6
            Layout.preferredHeight: 6
            radius: 0
            color: Theme.accent
        }
        Rectangle {
            objectName: root.warnName
            visible: root.warnText !== ""
            Layout.preferredWidth: Math.max(18, warnLabel.implicitWidth + 10)
            Layout.preferredHeight: 18
            radius: 0
            color: Theme.accent
            Text {
                id: warnLabel
                anchors.centerIn: parent
                text: root.warnText
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.tag.size
                font.weight: Font.DemiBold
                color: Theme.inkOnAccent
            }
        }
        Rectangle {
            objectName: root.badgeName
            visible: root.badge > 0
            Layout.preferredWidth: Math.max(18, badgeText.implicitWidth + 10)
            Layout.preferredHeight: 18
            radius: 0
            color: Theme.badge
            Text {
                id: badgeText
                anchors.centerIn: parent
                text: root.badge > 9 ? "9+" : String(root.badge)
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.tag.size
                font.weight: Font.DemiBold
                color: Theme.ground
            }
        }
    }
    UzFocusRing { visible: root.activeFocus; inset: true }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.activated() }
}
