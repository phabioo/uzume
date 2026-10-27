import QtQuick
import QtQuick.Layouts
import Uzume.App

// One plugin notice, in the look of Toast (design-system.md "Toast": `surface` plane, 1 px `rule` frame, no shadow, StatusIcon,
// message, accent text actions, close mark) and naming the plugin in a Mono source line. Tone info = info icon, important = done icon,
// warning = warning icon; the "⋯" (mute menu) is missing on host notices.
Item {
    id: root
    required property var toast                    // PluginsController.toasts entry
    property string marker: ""                     // "Unverified" / "Local" for non-official plugins
    signal dismissRequested()
    signal actionPicked(var act)
    signal menuRequested(Item anchor)
    readonly property bool warn: toast.tone === "warning"
    readonly property bool host: !!toast.host
    readonly property bool hovered: hover.hovered

    implicitWidth: 390
    implicitHeight: card.height
    height: card.height
    objectName: "toast_" + toast.toastId

    Rectangle {
        id: card
        width: parent.width
        height: Math.max(Theme.row44, grid.implicitHeight + 2 * Theme.space15)
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
        HoverHandler { id: hover }

        RowLayout {
            id: grid
            x: Theme.space15; y: Theme.space15
            width: parent.width - 2 * Theme.space15
            spacing: Theme.space15
            UzStatusIcon {
                Layout.alignment: Qt.AlignTop
                kind: root.warn ? "warning" : (root.toast.tone === "info" ? "info" : "done")
            }
            ColumnLayout {
                id: toastCol
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: Theme.space05
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space1
                    Text {
                        textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                        objectName: "toastSource"
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        text: root.host ? qsTr("%1 · %2").arg(root.toast.pluginName).arg(root.toast.title)
                                        : root.toast.tone === "info" ? root.toast.pluginName
                                        : qsTr("%1 · %2").arg(root.toast.pluginName).arg(root.warn ? qsTr("warning") : qsTr("important"))
                        font.family: Theme.mono
                        font.pixelSize: Theme.roles.meta.size
                        font.letterSpacing: Theme.roles.meta.spacing
                        font.weight: Font.Medium
                        font.capitalization: Font.AllUppercase
                        color: root.warn ? Theme.accentText : Theme.dim
                    }
                    UzTag {
                        objectName: "toastMarker"
                        visible: root.marker !== ""
                        small: true
                        tone: "accent"
                        icon: "none"
                        text: root.marker
                    }
                    Item { Layout.fillWidth: true }
                }
                Text {
                    objectName: "toastText"
                    Layout.fillWidth: true
                    visible: text !== ""
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    text: (!root.host && root.toast.title ? root.toast.title + "\n" : "") + (root.toast.text || "")
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.ink
                }
                // The Flow sits in a plain Item whose width is the column's: a Flow directly in the layout kept the width of an
                // earlier layout pass and ran past the notice with wide fonts.
                Item {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: (root.toast.actions || []).length > 0
                    implicitHeight: actionFlow.implicitHeight
                    Flow {
                        id: actionFlow
                        width: parent.width
                        spacing: Theme.space3
                        Repeater {
                            model: root.toast.actions || []
                            delegate: Item {
                                id: act
                                required property var modelData
                                required property int index
                                objectName: "toastAction_" + modelData.action
                                width: Math.min(actLabel.implicitWidth, act.parent ? act.parent.width : 100000)   // never wider than the notice (wide fonts)
                                height: Theme.row28
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: modelData.label
                                Keys.onPressed: (event) => {
                                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                                        root.actionPicked(act.modelData); event.accepted = true
                                    }
                                }
                                // The first action is the toast action (accent text, 500); further ones are quieter.
                                Text {
                                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                                    id: actLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.min(implicitWidth, act.width)
                                    elide: Text.ElideRight
                                    text: act.modelData.label
                                    font.family: Theme.sans
                                    font.pixelSize: Theme.roles.small.size
                                    font.weight: act.index === 0 ? Font.Medium : Font.Normal
                                    color: act.index === 0 ? Theme.accentText : (actHover.hovered ? Theme.ink : Theme.muted)
                                }
                                HoverHandler { id: actHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.actionPicked(act.modelData) }
                                UzFocusRing { visible: act.activeFocus; anchors.fill: undefined; y: 4; width: act.width; height: act.height - 8 }
                            }
                        }
                    }
                }
            }
            Row {
                Layout.alignment: Qt.AlignTop
                spacing: 0
                Item {
                    id: moreBtn
                    objectName: "toastMore"
                    visible: !root.host
                    width: 24; height: 24
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Notice menu")
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) { root.menuRequested(moreBtn); event.accepted = true }
                    }
                    Rectangle { anchors.fill: parent; color: moreHover.hovered ? Theme.pressed : "transparent" }
                    UzGlyph { anchors.centerIn: parent; name: "more"; size: 14; color: Theme.dim }
                    HoverHandler { id: moreHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.menuRequested(moreBtn) }
                    UzFocusRing { visible: moreBtn.activeFocus }
                }
                Item {
                    objectName: "toastDismiss"
                    id: closeBtn
                    width: 24; height: 24
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Dismiss")
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) { root.dismissRequested(); event.accepted = true }
                    }
                    Rectangle { anchors.fill: parent; color: closeHover.hovered ? Theme.pressed : "transparent" }
                    UzGlyph { anchors.centerIn: parent; name: "cross"; size: 12; color: Theme.dim }
                    HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.dismissRequested() }
                    UzFocusRing { visible: closeBtn.activeFocus }
                }
            }
        }
    }
}
