import QtQuick
import QtQuick.Layouts

// Stage diagnostics (design-system.md "StageDiagnostics"): the F3 block in the top right corner of the game stage: 232 px on
// `surface` in a `rule` frame, a Mono label with "Copy" (accent text; "Copied" with a check for a moment), label / value rows in
// Mono 10 (labels muted uppercase, values ink, right aligned; a "warn" row carries the Yamabuki "!" square).
// rows: [[label, value], [label, value, "warn"]]. `copyRequested()` is emitted on Copy; the owner puts the text on the clipboard.
// Optional "Details" disclosure below the rows (`detailRows`, `people`; see the properties).
Rectangle {
    id: root
    property string title: qsTr("Diagnostics")
    property var rows: []
    // Details: [[label, value]] (encoder, bitrate, resolution) and one object per person {name, path, rtt, loss, reason}.
    property var detailRows: []
    property var people: []
    property bool detailsOpen: true
    property real blockWidth: 232
    // Widths of the Path, RTT and Loss columns of the person table.
    readonly property var pathCols: [48, 56, 48]
    property bool copied: false
    signal copyRequested()
    signal detailsToggled(bool open)

    implicitWidth: blockWidth
    implicitHeight: head.height + 1 + grid.implicitHeight + Theme.space1 + Theme.space15 + (hasDetails ? detailsBox.implicitHeight + Theme.space1 : 0)
    radius: 0
    color: Theme.surface
    border.width: 1
    border.color: Theme.rule
    Accessible.role: Accessible.Pane
    Accessible.name: root.title

    Timer { id: reset; interval: 2000; onTriggered: root.copied = false }

    Item {
        id: head
        x: 1
        y: 1
        width: parent.width - 2
        height: 36
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space15
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            font.weight: Font.Medium
            font.letterSpacing: 1.4
            font.capitalization: Font.AllUppercase
            color: Theme.muted
        }
        Item {
            id: copyBtn
            objectName: "diagCopy"
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            width: copyRow.implicitWidth + 2 * Theme.space1
            height: 24
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: root.copied ? qsTr("Copied") : qsTr("Copy")
            function trigger() { root.copied = true; reset.restart(); root.copyRequested() }
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) { trigger(); event.accepted = true }
            }
            Rectangle { anchors.fill: parent; color: copyTap.pressed ? Theme.pressed : (copyHover.hovered ? Theme.hover : "transparent") }
            Row {
                id: copyRow
                anchors.centerIn: parent
                spacing: 4
                UzGlyph { visible: root.copied; anchors.verticalCenter: parent.verticalCenter; name: "check"; size: 10; color: Theme.ink }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.copied ? qsTr("Copied") : qsTr("Copy")
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    font.weight: Font.Medium
                    color: Theme.ink
                }
            }
            HoverHandler { id: copyHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { id: copyTap; onTapped: copyBtn.trigger() }
            UzFocusRing { visible: copyBtn.activeFocus }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
    }
    UzDiagRows {
        id: grid
        objectName: "diagRows"
        x: 1 + Theme.space15
        y: head.height + 1 + Theme.space1
        width: parent.width - 2 - 2 * Theme.space15
        rows: root.rows
    }

    // Details (readable pass, board 3g-6): encoder / decoder, bitrate, resolution and a table per person. Shown only when there is
    // something to show (no session: no section); `detailsOpen` is owned by the caller (persisted by the DiagnosticsModel).
    readonly property bool hasDetails: root.detailRows.length > 0 || root.people.length > 0
    Item {
        id: detailsBox
        objectName: "diagDetails"
        visible: root.hasDetails
        x: 1
        y: head.height + 1 + Theme.space1 + grid.implicitHeight + Theme.space1
        width: parent.width - 2
        implicitHeight: visible ? detailsCol.implicitHeight : 0
        Rectangle { width: parent.width; height: 1; color: Theme.rule2 }
        ColumnLayout {
            id: detailsCol
            y: 1
            width: parent.width
            spacing: Theme.space05
            UzDisclosure {
                id: disc
                objectName: "diagDetailsToggle"
                title: qsTr("Details")
                Layout.leftMargin: Theme.space05
                Binding { target: disc; property: "open"; value: root.detailsOpen }
                onOpenChanged: if (open !== root.detailsOpen) root.detailsToggled(open)
            }
            UzDiagRows {
                objectName: "diagDetailRows"
                visible: disc.open && root.detailRows.length > 0
                Layout.leftMargin: Theme.space15
                Layout.rightMargin: Theme.space15
                Layout.fillWidth: true
                rows: root.detailRows
            }
            // One row per person: name, path (Direct / Relay), RTT, loss; the reason for a relay in one sentence below the row.
            ColumnLayout {
                id: table
                objectName: "diagPeople"
                visible: disc.open && root.people.length > 0
                Layout.leftMargin: Theme.space15
                Layout.rightMargin: Theme.space15
                Layout.fillWidth: true
                Layout.bottomMargin: Theme.space15
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space1
                    Repeater {
                        model: [qsTr("Person"), qsTr("Path"), qsTr("RTT"), qsTr("Loss")]
                        delegate: Text {
                            required property int index
                            required property string modelData
                            Layout.fillWidth: index === 0
                            Layout.preferredWidth: index === 0 ? 0 : root.pathCols[index - 1]
                            horizontalAlignment: index >= 2 ? Text.AlignRight : Text.AlignLeft
                            text: modelData
                            font.family: Theme.mono
                            font.pixelSize: Theme.roles.meta.size
                            font.letterSpacing: 0.6
                            font.capitalization: Font.AllUppercase
                            color: Theme.muted
                        }
                    }
                }
                Repeater {
                    model: root.people
                    delegate: ColumnLayout {
                        id: person
                        required property var modelData
                        objectName: "diagPerson_" + modelData.name
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space1
                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: person.modelData.name
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                font.family: Theme.mono
                                font.pixelSize: Theme.roles.meta.size
                                color: Theme.ink
                            }
                            Text {
                                objectName: "diagPath"
                                Layout.preferredWidth: root.pathCols[0]
                                text: person.modelData.path || "—"
                                textFormat: Text.PlainText
                                font.family: Theme.mono
                                font.pixelSize: Theme.roles.meta.size
                                font.weight: person.modelData.path === qsTr("Relay") ? Font.Medium : Font.Normal
                                color: person.modelData.path === qsTr("Relay") ? Theme.ink : Theme.ink
                            }
                            Text {
                                objectName: "diagRtt"
                                Layout.preferredWidth: root.pathCols[1]
                                horizontalAlignment: Text.AlignRight
                                text: person.modelData.rtt || "—"
                                textFormat: Text.PlainText
                                font.family: Theme.mono
                                font.pixelSize: Theme.roles.meta.size
                                color: Theme.ink
                            }
                            Text {
                                objectName: "diagLoss"
                                Layout.preferredWidth: root.pathCols[2]
                                horizontalAlignment: Text.AlignRight
                                text: person.modelData.loss || "—"
                                textFormat: Text.PlainText
                                font.family: Theme.mono
                                font.pixelSize: Theme.roles.meta.size
                                color: Theme.ink
                            }
                        }
                        Text {
                            objectName: "diagReason"
                            visible: text !== ""
                            Layout.fillWidth: true
                            text: person.modelData.reason || ""
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.small.size
                            color: Theme.ink2
                        }
                    }
                }
            }
        }
    }
}
