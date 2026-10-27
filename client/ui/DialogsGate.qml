import QtQuick

// `usable` is true when the file dialogs of QtQuick.Dialogs can be opened without risk (see DialogsProbe.qml). One gate per place
// that opens a dialog: `DialogsGate { id: gate }` and `if (gate.usable) openTheDialog() else showThePathField()`.
Item {
    id: root
    readonly property bool usable: probe.status === Loader.Ready
    visible: false
    width: 0
    height: 0
    Loader {
        id: probe
        source: "DialogsProbe.qml"
    }
}
