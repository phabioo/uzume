import QtQuick
import QtQuick.Layouts
import Uzume.App

// Base of the pages of the start screen and the switch-user screen: the page declares its frame texts and hints, the host
// (StartFrame) shows them. `initialFocus` gets the focus when the page appears. `backRequested()` is Esc / "Back".
ColumnLayout {
    id: root
    property string screenLabel: ""        // Mono label in the top bar ("JOIN A SERVER · 1 OF 3")
    property string openLabel: "UZUME"     // vertical label of the open column
    property var hints: []
    property var helpItems: []             // [{ title?, text }] of the 260 px help column; empty = no help column
    property real maxWidth: 520
    property Item initialFocus: null
    signal backRequested()
    spacing: 28
}
