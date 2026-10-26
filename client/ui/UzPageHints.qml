import QtQuick

// Hint row of a main page (readable-pass.md "Gamepad everywhere"): shown while the last input was a pad, takes no space
// otherwise. A Choose, B Back, Y Search (`search`) and, in the Library, L / R Filter (`filter`): only what the pad bridge does
// (ControllersController::feedPadNav).
UzHintRow {
    id: root
    required property var player
    property bool search: false
    property bool filter: false
    objectName: "pageHints"
    device: "gamepad"
    visible: root.player.controllers.padActive
    height: visible ? implicitHeight : 0
    hints: {
        const h = [{ pad: "A", label: qsTr("Choose") }, { pad: "B", label: qsTr("Back") }]
        if (root.search) h.push({ pad: "Y", label: qsTr("Search") })
        if (root.filter) h.push({ pad: "L / R", label: qsTr("Filter") })
        return h
    }
}
