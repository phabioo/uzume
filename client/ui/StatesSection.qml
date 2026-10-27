import QtQuick
import QtQuick.Layouts
import Uzume.App

// States of the Saves tab (play-and-plugins.md 3c-2 and 3g): "STATES 3 OF 4 · ON HOME SERVER", Auto (A) and slots 1 to 4 with a
// preview, when it was saved and by which core, and the actions Save to N (the chosen slot is the primary one), Load N and a
// "⋯" menu with Delete. In the game the hints F6 Save, F8 Load and Shift+F6 Next slot show; on the game page, where nothing
// runs, the list is read-only apart from Delete. States are not the save: the note says so.
// objectNames: statesSection, statesSummary, statesBlocked, statesHints, stateRow_<n>, statePreview_<n>, stateSave_<n>, stateLoad_<n>,
// stateMenu_<n>, stateTag_<n>, statesNote.
ColumnLayout {
    id: root
    required property PlayerController player
    readonly property var st: root.player.states
    readonly property var game: root.player.selectedGame
    readonly property string gameId: root.game.id ?? ""
    readonly property var keys: root.player.controllers.hotkeyHints
    property int menuFor: -1

    objectName: "statesSection"
    visible: root.st.supported && root.gameId !== ""
    Layout.fillWidth: true
    Layout.minimumWidth: 0
    Layout.topMargin: 2
    spacing: Theme.space1

    onGameIdChanged: if (root.gameId !== "") root.st.view(root.gameId)
    Component.onCompleted: if (root.gameId !== "") root.st.view(root.gameId)

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 8
        Eyebrow { text: qsTr("States") }
        Item { Layout.fillWidth: true }
        UzMono {
            objectName: "statesSummary"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
            text: root.st.summary
            color: Theme.dim
            font.pixelSize: Theme.roles.meta.size
        }
    }

    UzLabel {
        objectName: "statesBlocked"
        visible: root.st.blockedText !== ""
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: root.st.blockedText
        color: Theme.danger
        font.pixelSize: Theme.roles.small.size
    }
    Flow {
        objectName: "statesHints"
        visible: root.st.playing && root.st.blockedText === ""
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: 14
        UzKbd { keys: root.keys.saveState || "F6"; label: qsTr("Save") }
        UzKbd { keys: root.keys.loadState || "F8"; label: qsTr("Load") }
        UzKbd { keys: "Shift+" + (root.keys.saveState || "F6"); label: qsTr("Next slot") }
        UzKbd { visible: root.st.rewindAvailable; keys: root.keys.rewind || "Backspace"; label: qsTr("Hold to rewind") }
    }
    UzLabel {
        visible: !root.st.playing
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Start the game to save or load a state.")
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
    }

    Repeater {
        model: root.st.slotRows
        delegate: ColumnLayout {
            id: row
            required property var modelData
            readonly property int n: modelData.n
            readonly property bool filled: !modelData.empty
            readonly property bool chosen: modelData.n === root.st.slot
            objectName: "stateRow_" + row.n
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 0
            // Auto has nothing to save into and nothing to show when it is empty
            visible: row.n >= 1 || row.filled

            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule }
            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.topMargin: Theme.space1
                Layout.bottomMargin: Theme.space1
                spacing: Theme.space15
                Rectangle {
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: 54
                    Layout.alignment: Qt.AlignTop
                    color: Theme.ground
                    border.width: row.chosen && root.st.playing ? 2 : 1
                    border.color: row.chosen && root.st.playing ? Theme.accent : Theme.rule
                    Image {
                        objectName: "statePreview_" + row.n
                        anchors.fill: parent
                        anchors.margins: 1
                        visible: row.filled && row.modelData.previewUrl !== ""
                        source: row.filled ? row.modelData.previewUrl : ""
                        fillMode: Image.PreserveAspectFit
                        cache: false
                        asynchronous: true
                    }
                    UzMono {
                        anchors.centerIn: parent
                        visible: !(row.filled && row.modelData.previewUrl !== "")
                        text: row.modelData.label
                        color: Theme.dim
                        font.pixelSize: Theme.roles.body.size
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 3
                    Flow {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 8
                        UzLabel {
                            text: row.n === 0 ? qsTr("Auto") : qsTr("State %1").arg(row.n)
                            font.pixelSize: Theme.roles.input.size
                            font.weight: Font.DemiBold
                        }
                        UzTag {
                            objectName: "stateTag_" + row.n
                            visible: row.filled && row.modelData.tag !== ""
                            small: true
                            tone: "warn"
                            text: row.filled ? row.modelData.tag : ""
                        }
                        UzTag {
                            visible: row.filled && row.modelData.pending === true
                            small: true
                            tone: "neutral"
                            text: qsTr("Waiting for the server")
                        }
                    }
                    UzLabel {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        wrapMode: Text.WordWrap
                        color: Theme.muted
                        font.pixelSize: Theme.roles.small.size
                        text: row.filled ? row.modelData.whenText + " · " + row.modelData.coreText
                                           + (row.modelData.device !== "" ? " · " + row.modelData.device : "")
                                         : (row.n === 0 ? "" : qsTr("Empty"))
                    }
                    Flow {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 6
                        UzButton {
                            objectName: "stateSave_" + row.n
                            visible: row.n >= 1 && root.st.playing
                            enabled: row.modelData.canSave === true && !root.st.busy
                            size: "sm"
                            hPadding: Theme.space2
                            kind: row.chosen ? "primary" : "outline"
                            text: qsTr("Save to %1").arg(row.n)
                            onClicked: { root.st.slot = row.n; root.st.save(row.n) }
                        }
                        UzButton {
                            objectName: "stateLoad_" + row.n
                            visible: row.filled && root.st.playing
                            enabled: row.modelData.canLoad === true && !root.st.busy
                            size: "sm"
                            hPadding: Theme.space2
                            text: row.filled ? (row.modelData.loadLabel + (row.n >= 1 ? " " + row.n : "")) : ""
                            onClicked: { if (row.n >= 1) root.st.slot = row.n; root.st.requestLoad(row.n) }
                        }
                        UzIconButton {
                            id: more
                            objectName: "stateMenu_" + row.n
                            visible: row.filled
                            glyph: "more"
                            text: qsTr("More for state %1").arg(row.n)
                            onClicked: { root.menuFor = row.n; menu.openFor(more) }
                        }
                    }
                }
            }
        }
    }
    UzMenu {
        id: menu
        items: [{ label: qsTr("Delete state"), danger: true }]
        onChosen: root.st.remove(root.menuFor)
    }
    UzLabel {
        objectName: "statesNote"
        Layout.fillWidth: true
        Layout.topMargin: Theme.space05
        wrapMode: Text.WordWrap
        text: qsTr("States are not your save. Your save changes only when the game saves.")
        color: Theme.muted
        font.pixelSize: Theme.roles.small.size
    }
    Rectangle { Layout.fillWidth: true; Layout.bottomMargin: Theme.space1; height: 1; color: Theme.rule }
}
