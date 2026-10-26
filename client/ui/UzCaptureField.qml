import QtQuick

// Binding field (Controllers): a boxed field that starts capture ("Press a key…" / "Press a button…") when it is clicked, or
// when it has the focus and Enter, Space or pad A is pressed. While `capturing` the keys are left alone so the page can capture them.
// `locked` fields (fixed bindings) take neither focus nor input.
Rectangle {
    id: root
    property bool capturing: false
    property bool locked: false
    property string accessibleName: ""
    signal activated()
    activeFocusOnTab: !root.locked
    Accessible.role: Accessible.Button
    Accessible.name: root.accessibleName
    Accessible.focusable: !root.locked
    Accessible.onPressAction: root.activated()
    Keys.onPressed: (event) => {
        if (root.locked || root.capturing) return
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
            root.activated()
            event.accepted = true
        }
    }
    // The ring sits inside the box: the rows around the field clip an outer ring.
    UzFocusRing { visible: root.activeFocus; inset: true }
    TapHandler { enabled: !root.locked; onTapped: root.activated() }
}
