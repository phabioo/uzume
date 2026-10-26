import QtQuick

// VoiceMark (design-system.md "VoiceMark"): speaking (three Yamabuki bars), mic off (outlined square with the crossed mic) and
// muted by host (ink square with the crossed mic), each with its Mono word unless `word` is false. Never colour alone.
// `still` stops the bars (reduced motion, screenshots).
Row {
    id: root
    property string voice: ""        // speaking | micOff | mutedByHost
    property bool word: true
    property bool still: false
    visible: voice !== ""
    spacing: 6
    Accessible.role: Accessible.StaticText
    Accessible.name: voice === "speaking" ? qsTr("speaking") : voice === "mutedByHost" ? qsTr("muted by host") : qsTr("mic off")

    UzVoiceBars { visible: root.voice === "speaking"; still: root.still; anchors.verticalCenter: parent.verticalCenter }
    Rectangle {
        visible: root.voice === "micOff" || root.voice === "mutedByHost"
        anchors.verticalCenter: parent.verticalCenter
        width: 16
        height: 16
        radius: 0
        color: root.voice === "mutedByHost" ? Theme.ink : "transparent"
        border.width: root.voice === "micOff" ? 1 : 0
        border.color: Theme.muted
        UzGlyph { anchors.centerIn: parent; name: "micOff"; size: 12; color: root.voice === "mutedByHost" ? Theme.ground : Theme.muted }
    }
    Text {
        visible: root.word
        anchors.verticalCenter: parent.verticalCenter
        text: root.voice === "speaking" ? qsTr("SPEAKING") : root.voice === "mutedByHost" ? qsTr("MUTED BY HOST") : qsTr("MIC OFF")
        font.family: Theme.mono
        font.pixelSize: Theme.roles.meta.size
        font.weight: Font.Medium
        font.letterSpacing: 0.8
        color: root.voice === "micOff" ? Theme.muted : Theme.ink
    }
}
