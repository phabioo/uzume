import QtQuick
import QtQuick.Layouts

// ColumnNav row (design-system.md "ColumnNav"): a row of the second column of Settings, Emulation, Controllers and the server
// settings. Title (14 px), optional mono `meta` at the right, optional warning square (UzStatusIcon, `warn`) and extra lines
// below (default children). Selected = `selected` tint plus the 2 px `marker-nav` start marker. Focusable: Up / Down move
// between the rows of the UzColumnNav, Enter / Space (pad A) open, Right goes into the content; the focus ring is inset.
Item {
    id: root
    property string label: ""
    property string meta: ""
    property string metaName: ""
    property bool selected: false
    property bool warn: false
    property string warnName: ""
    default property alias sub: subCol.data
    readonly property bool colnavRow: true
    signal clicked()

    Layout.fillWidth: true
    Layout.minimumWidth: 0
    implicitHeight: Math.max(Theme.row32, rowCol.implicitHeight + 2 * Theme.space05)
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: root.label + (root.meta !== "" ? ", " + root.meta : "") + (root.warn ? ", " + qsTr("warning") : "")
    Accessible.selected: root.selected
    Accessible.focusable: true
    Accessible.onPressAction: root.clicked()

    function colNav(): Item {
        let p = root.parent
        while (p) {
            if (p.colnavNav === true) return p
            p = p.parent
        }
        return null
    }
    Keys.onPressed: (event) => {
        const nav = root.colNav()
        switch (event.key) {
        case Qt.Key_Return:
        case Qt.Key_Enter:
        case Qt.Key_Space:
            root.clicked()
            event.accepted = true
            break
        case Qt.Key_Down:
            if (nav) nav.step(root, 1)
            event.accepted = true
            break
        case Qt.Key_Up:
            if (nav) nav.step(root, -1)
            event.accepted = true
            break
        case Qt.Key_Right:
            if (nav) nav.contentRequested()
            event.accepted = true
            break
        }
    }
    onActiveFocusChanged: if (root.activeFocus) {
        const nav = root.colNav()
        if (nav) nav.reveal(root)
    }

    Rectangle {
        anchors.fill: parent
        color: root.selected ? Theme.selected : (hover.hovered ? Theme.hover : "transparent")
    }
    Rectangle {   // marker-nav
        objectName: "columnItemMarker"
        visible: root.selected
        width: 2
        height: parent.height
        color: Theme.markerNav
    }
    ColumnLayout {
        id: rowCol
        x: Theme.space15
        y: Math.max(0, (root.height - implicitHeight) / 2)
        width: Math.max(0, root.width - 2 * Theme.space15)
        spacing: 0
        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.space1
            UzLabel {
                Layout.fillWidth: true
                text: root.label
                elide: Text.ElideRight
                font.weight: root.selected ? Font.Medium : Font.Normal
                color: root.selected ? Theme.ink : Theme.muted
            }
            UzStatusIcon {
                objectName: root.warnName
                visible: root.warn
                kind: "warning"
                small: true
                Accessible.ignored: true
            }
            Text {
                objectName: root.metaName
                visible: root.meta !== ""
                text: root.meta
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.meta.size
                font.letterSpacing: Theme.roles.meta.spacing
                font.capitalization: Font.AllUppercase
                color: Theme.dim
            }
        }
        ColumnLayout {
            id: subCol
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 0
        }
    }
    UzFocusRing { visible: root.activeFocus; inset: true }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.clicked() }
}
