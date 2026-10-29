import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Run recap (design 4jh-app-2, runs-and-server.md "4jh Run board and recap"): four stat tiles (catches, lost, badges, play time),
// the Lanes block (one 48 px lane per person on a rule-2 track, square Mono glyphs ■ catch, ✕ lost, ◆ badge, □ note, ▣ screenshot,
// day axis DAY 1..N), the legend and the footnote. Export (image or HTML file) and Share link sit above the sheet. The sheet
// (everything between the two rules) is what "Export as image" grabs. Loading, error and empty states use StateBlock.
ColumnLayout {
    id: root
    required property PlayerController player
    readonly property RunRecapController recap: root.player.runs.recap
    readonly property var model: root.recap.model
    readonly property string serverName: root.player.hubName !== "" ? root.player.hubName : qsTr("this server")
    property string exportKind: ""     // png | html: what the open save dialog is for
    readonly property var glyphLegend: [
        { glyph: "■", label: qsTr("catch") }, { glyph: "✕", label: qsTr("lost") }, { glyph: "◆", label: qsTr("badge") },
        { glyph: "□", label: qsTr("note") }, { glyph: "▣", label: qsTr("screenshot") }
    ]
    spacing: Theme.space3

    function startExport(kind: string) {
        root.exportKind = kind
        saveLoader.ext = kind === "png" ? "png" : "html"
        saveLoader.suggested = root.recap.suggestedFileName(saveLoader.ext)
        saveLoader.open()
    }
    function finishExport(target: string) {
        if (root.exportKind === "png") {
            if (sheet.width <= 0 || sheet.height <= 0) {  // grabToImage on a zero-size item fails with a warning
                root.recap.saveImage(undefined, target)
                return
            }
            sheet.grabToImage(function(result) { root.recap.saveImage(result.image, target) })
        } else if (root.exportKind === "html") {
            root.recap.saveHtml(target)
        }
    }

    // ------------------------------------------------------------ actions and message
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        // Status note: lives in the action row (no extra row), so it never pushes the sheet down.
        RowLayout {
            objectName: "recapMessage"
            visible: root.recap.message !== ""
            Layout.fillWidth: true
            spacing: Theme.space2
            UzStatusIcon {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                kind: root.recap.messageIsError ? "error" : "done"
                small: true
                text: root.recap.message
                maxWidth: Math.max(0, root.width - 360)
            }
            UzButton { kind: "link"; text: qsTr("Dismiss"); onClicked: root.recap.dismissMessage() }
        }
        Item { Layout.fillWidth: true; visible: root.recap.message === "" }
        UzButton {
            id: exportButton
            objectName: "recapExport"
            text: qsTr("Export")
            chevron: true
            enabled: root.recap.state === "ready" && !root.recap.recapEmpty
            onClicked: exportMenu.openFor(exportButton, "below", true)
        }
        UzButton {
            objectName: "recapShare"
            text: qsTr("Share link")
            destructive: true
            enabled: root.recap.runId !== "" && !(root.recap.state === "ready" && root.recap.recapEmpty)
            onClicked: root.recap.shareLink()
        }
    }
    UzMenu {
        id: exportMenu
        objectName: "recapExportMenu"
        items: [{ label: qsTr("Export as image (PNG)"), name: "png" }, { label: qsTr("Export as HTML file"), name: "html" }]
        onChosen: item => root.startExport(item.name)
    }
    RowLayout {
        objectName: "recapStale"
        visible: root.recap.staleNotice !== "" && root.recap.state === "ready"
        Layout.fillWidth: true
        spacing: Theme.space2
        UzStatusIcon {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            kind: "warning"
            small: true
            text: qsTr("Showing the last loaded recap. %1").arg(root.recap.staleNotice)
            maxWidth: Math.max(0, root.width - 140)
        }
        UzButton { kind: "link"; text: qsTr("Try again"); onClicked: root.recap.reload() }
    }

    // ------------------------------------------------------------ states
    UzStateBlock {
        objectName: "recapLoading"
        visible: root.recap.state === "loading" || root.recap.state === "idle"
        Layout.fillWidth: true
        kind: "loading"
        title: qsTr("Loading the recap")
        text: qsTr("Reading the events and screenshots of this run.")
    }
    UzStateBlock {
        objectName: "recapError"
        visible: root.recap.state === "error"
        Layout.fillWidth: true
        kind: "error"
        title: qsTr("The recap could not be loaded")
        text: root.recap.errorText
        action: qsTr("Try again")
        onActionClicked: root.recap.reload()
    }
    UzStateBlock {
        objectName: "recapEmpty"
        visible: root.recap.state === "ready" && root.recap.recapEmpty
        Layout.fillWidth: true
        kind: "empty"
        title: qsTr("Nothing to recap yet")
        text: qsTr("Catches, badges, notes and screenshots of this run appear here as they happen.")
    }

    // ------------------------------------------------------------ the sheet (what "Export as image" grabs)
    Rectangle {
        id: sheet
        objectName: "recapSheet"
        visible: root.recap.state === "ready" && !root.recap.recapEmpty
        Layout.fillWidth: true
        implicitHeight: sheetCol.implicitHeight + 2 * Theme.space2
        color: Theme.ground

        ColumnLayout {
            id: sheetCol
            x: Theme.space2
            y: Theme.space2
            width: sheet.width - 2 * Theme.space2
            spacing: Theme.space3

            UzSectionLabel {
                Layout.fillWidth: true
                text: qsTr("Run recap")
                meta: (root.model.runName || "") + " · " + qsTr("DAY %1").arg(root.model.days || 1)
            }

            // Tiles (a Flow, not a GridLayout: the model is replaced on every reload and Qt 6.4 layouts crash on that)
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule }
            Flow {
                id: tiles
                objectName: "recapTiles"
                Layout.fillWidth: true
                spacing: 0
                readonly property int cols: sheet.width >= 640 ? 4 : 2
                Repeater {
                    model: root.model.tiles || []
                    delegate: UzMetric {
                        required property var modelData
                        required property int index
                        objectName: "recapTile_" + modelData.id
                        width: Math.floor(tiles.width / tiles.cols)
                        label: modelData.label
                        value: modelData.value
                        line: modelData.sub
                        leftRule: index % tiles.cols !== 0
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }

            // Lanes
            UzSectionLabel {
                Layout.fillWidth: true
                text: qsTr("Lanes")
                meta: qsTr("DAY 1 – DAY %1").arg(root.model.days || 1)
            }
            Item {
                id: lanes
                objectName: "recapLanes"
                Layout.fillWidth: true
                implicitHeight: axis.height + laneList.height
                readonly property int nameWidth: sheet.width >= 480 ? 120 : 84
                readonly property real trackX: nameWidth + Theme.space2
                readonly property real trackW: Math.max(0, width - trackX)

                Item {
                    id: axis
                    objectName: "recapAxis"
                    x: lanes.trackX
                    width: lanes.trackW
                    height: 22
                    Repeater {
                        model: root.model.axis || []
                        delegate: UzMono {
                            required property var modelData
                            x: Math.max(0, Math.min(axis.width - width, modelData.pos * axis.width - width / 2))
                            text: modelData.label
                            color: Theme.dim
                            font.pixelSize: 10
                            font.letterSpacing: 0.6
                        }
                    }
                }
                Rectangle { x: lanes.trackX; y: axis.height - 1; width: lanes.trackW; height: 1; color: Theme.rule }
                Column {
                    id: laneList
                    y: axis.height
                    width: lanes.width
                    Repeater {
                        model: root.model.lanes || []
                        delegate: Item {
                            id: lane
                            required property var modelData
                            objectName: "recapLane_" + lane.modelData.userId
                            width: laneList.width
                            height: 48
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                            UzAvatar {
                                id: avatar
                                anchors.verticalCenter: parent.verticalCenter
                                name: lane.modelData.name
                                person: lane.modelData.color >= 0 ? lane.modelData.color + 1 : 1
                                size: 20
                            }
                            UzLabel {
                                anchors.verticalCenter: parent.verticalCenter
                                x: avatar.width + Theme.space1
                                width: lanes.nameWidth - x
                                text: lane.modelData.name
                                elide: Text.ElideRight
                                font.pixelSize: Theme.roles.input.size
                            }
                            Item {
                                id: track
                                x: lanes.trackX
                                width: lanes.trackW
                                height: parent.height
                                Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: 1; color: Theme.rule2 }
                                Repeater {
                                    // Glyphs closer than one glyph width are grouped into one glyph with a "+N" badge.
                                    model: root.recap.groupEvents(lane.modelData.events, track.width, 16)
                                    delegate: Item {
                                        id: glyphItem
                                        required property var modelData
                                        objectName: "recapGlyph_" + glyphItem.modelData.glyph
                                        readonly property bool grouped: glyphItem.modelData.count > 1
                                        x: Math.max(0, Math.min(track.width - width, glyphItem.modelData.pos * track.width - width / 2))
                                        y: (track.height - height) / 2
                                        width: 16
                                        height: 24
                                        activeFocusOnTab: true
                                        Accessible.role: Accessible.StaticText
                                        Accessible.name: glyphItem.modelData.name
                                        Accessible.focusable: true
                                        Text {
                                            anchors.centerIn: parent
                                            text: glyphItem.modelData.glyphChar
                                            textFormat: Text.PlainText
                                            font.family: Theme.mono
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                            color: glyphItem.modelData.glyph === "screenshot" ? Theme.muted : Theme.ink
                                        }
                                        UzMono {
                                            objectName: "recapGlyphBadge"
                                            visible: glyphItem.grouped
                                            x: parent.width - 4
                                            y: 0
                                            text: "+" + glyphItem.modelData.extra
                                            color: Theme.accentText
                                            font.pixelSize: 9
                                        }
                                        UzFocusRing { visible: glyphItem.activeFocus; inset: true }
                                        HoverHandler { id: hover }
                                        // An explicit tooltip: the attached one on a plain Item keeps the style's palette and drew no text.
                                        ToolTip {
                                            objectName: "recapGlyphTip"
                                            visible: hover.hovered || glyphItem.activeFocus
                                            text: glyphItem.modelData.tip
                                            contentItem: Text {
                                                text: glyphItem.modelData.tip
                                                textFormat: Text.PlainText
                                                color: Theme.ink
                                                font.pixelSize: Theme.roles.small.size
                                            }
                                            background: Rectangle { color: Theme.surface; border.color: Theme.rule; border.width: 1 }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Legend
            Flow {
                objectName: "recapLegend"
                Layout.fillWidth: true
                spacing: Theme.space3
                Repeater {
                    model: root.glyphLegend
                    delegate: UzMono {
                        required property var modelData
                        text: modelData.glyph + " " + modelData.label
                        font.pixelSize: Theme.roles.small.size
                    }
                }
            }
        }
    }

    // Outside the sheet: it is a hint for this screen and must not end up in the exported image.
    UzLabel {
        objectName: "recapFootnote"
        visible: sheet.visible
        Layout.fillWidth: true
        text: qsTr("Share link shares the recap with everyone on %1.").arg(root.serverName)
        wrapMode: Text.WordWrap
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
    }

    // ------------------------------------------------------------ save dialog (QtQuick.Dialogs when installed)
    Loader {
        id: saveLoader
        property string suggested: ""
        property string ext: "png"
        active: false
        source: "AdminSaveDialog.qml"
        function open() {
            if (item) {
                item.suffix = saveLoader.ext
                item.prepare(saveLoader.suggested)
                item.open()
            } else {
                active = true
            }
        }
        onLoaded: {
            item.player = root.player
            item.title = qsTr("Save the run recap")
            item.suffix = saveLoader.ext
            item.chosen.connect(target => root.finishExport(target))
            item.prepare(saveLoader.suggested)
            item.open()
        }
        onStatusChanged: if (status === Loader.Error) root.recap.reportDialogMissing()
    }
}
