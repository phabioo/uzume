import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Settings › This PC › Voice (board 6w-1): the voice chat of this PC. The label says which server carries it. Microphone (a Select and
// the level with the voice-activation threshold line: a click on the bar sets the threshold), Talk (push-to-talk or voice activation),
// the push-to-talk key, Noise suppression, "Lower game volume while someone speaks" and "Join voice automatically". The page header and
// the scrolling belong to SettingsScreen. While the page is open the microphone is open too, so the level moves; nothing is sent.
ColumnLayout {
    id: root
    objectName: "voiceContent"
    required property PlayerController player
    readonly property VoiceController voice: root.player.voice
    spacing: Theme.space15

    Component.onCompleted: root.voice.monitoring = true
    Component.onDestruction: root.voice.monitoring = false

    readonly property var talkRow: {
        const rows = root.player.controllers.hotkeyRows
        for (let i = 0; i < rows.length; ++i) if (rows[i].action === "talk") return rows[i]
        return ({ key: "V", pad: "" })
    }
    readonly property bool micProblem: root.voice.micState === "blocked" || root.voice.micState === "noDevice"

    UzSectionLabel {
        objectName: "voiceThrough"
        Layout.fillWidth: true
        text: qsTr("Through %1").arg(root.voice.serverName !== "" ? root.voice.serverName : qsTr("your server"))
    }
    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule }

    UzSettingsRow {
        objectName: "voiceMicRow"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        label: qsTr("Microphone")
        description: qsTr("The level shows what the microphone hears. With voice activation the line is where Uzume starts sending: click the bar to move it.")
        ctrl: "select"
        controlName: "voiceMicSelect"
        values: root.voice.micDevices
        current: root.voice.micDevice
        onValuePicked: value => root.voice.micDevice = value
    }
    // The level with the threshold line
    Item {
        id: meter
        objectName: "voiceMeter"
        Layout.fillWidth: true
        Layout.leftMargin: Theme.settingsDotSlot
        Layout.preferredHeight: 28
        UzProgress {
            id: bar
            objectName: "voiceLevel"
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            value: root.voice.level
            Accessible.name: qsTr("Microphone level")
        }
        Rectangle {   // the threshold line: 1 px ink, taller than the bar
            objectName: "voiceThresholdLine"
            x: Math.round(root.voice.thresholdFraction * bar.width) - 1
            anchors.verticalCenter: parent.verticalCenter
            width: 2
            height: 16
            color: Theme.ink
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: mouse => root.voice.setThresholdFraction(mouse.x / Math.max(1, bar.width))
        }
    }
    // A microphone that cannot be used says so here, with the one thing to do about it.
    RowLayout {
        objectName: "voiceMicProblem"
        visible: root.micProblem
        Layout.fillWidth: true
        Layout.leftMargin: Theme.settingsDotSlot
        spacing: Theme.space15
        Text {
            objectName: "voiceMicProblemText"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: root.voice.micDetail
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: root.voice.micState === "blocked" ? Theme.danger : Theme.muted
        }
        UzButton {
            objectName: "voiceMicFix"
            visible: root.voice.micState === "blocked"
            size: "sm"
            text: Qt.platform.os === "windows" ? qsTr("Open Windows settings") : qsTr("Try again")
            onClicked: root.voice.openSystemSettings()
        }
        UzButton {
            objectName: "voiceMicRetry"
            visible: root.voice.micState === "noDevice"
            size: "sm"
            text: qsTr("Look again")
            onClicked: root.voice.retryMic()
        }
    }

    UzSettingsRow {
        objectName: "voiceTalkRow"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        label: qsTr("Talk")
        description: qsTr("Push-to-talk sends only while you hold the key. Voice activation sends while you speak. Headphones keep the game out of the microphone.")
        ctrl: "segment"
        controlName: "voiceTalkSegment"
        values: [
            { value: "ptt", label: qsTr("Push-to-talk"), name: "voiceTalkPtt" },
            { value: "vad", label: qsTr("Voice activation"), name: "voiceTalkVad" }
        ]
        current: root.voice.talkMode
        onValuePicked: value => root.voice.talkMode = value
    }
    UzSettingsRow {
        objectName: "voiceKeyRow"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        label: qsTr("Push-to-talk key")
        description: qsTr("With voice activation the same keys turn the microphone on or off. Shift+M does that in either mode. Change the keys under Hotkeys.")
        ctrl: "value"
        controlName: "voiceKeyValue"
        valueText: (root.talkRow.key || "V") + (root.talkRow.padSet ? qsTr(" · hold %1").arg(root.talkRow.pad) : "")
    }
    UzSettingsRow {
        objectName: "voiceNoiseRow"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        label: qsTr("Noise suppression")
        description: qsTr("Takes steady noise and the sound of your speakers out of what you send.")
        ctrl: "toggle"
        controlName: "voiceNoiseToggle"
        checked: root.voice.noiseSuppression
        onToggled: on => root.voice.noiseSuppression = on
    }
    UzSettingsRow {
        objectName: "voiceDuckRow"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        label: qsTr("Lower game volume while someone speaks")
        description: qsTr("Halves your game and the tiles you watch while a person speaks.")
        ctrl: "toggle"
        controlName: "voiceDuckToggle"
        checked: root.voice.ducking
        onToggled: on => root.voice.ducking = on
    }
    UzSettingsRow {
        objectName: "voiceAutoRow"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        resetMode: "none"
        label: qsTr("Join voice automatically")
        description: qsTr("In runs you play and in invite-only Sessions. Other Sessions ask you to join with one click.")
        ctrl: "toggle"
        controlName: "voiceAutoToggle"
        checked: root.voice.autoJoin
        onToggled: on => root.voice.autoJoin = on
    }
}
