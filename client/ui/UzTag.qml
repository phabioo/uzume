import QtQuick

// Tag (design-system.md "Tag"): Mono status label, 18 px, square. tone: neutral | accent | ok | danger (legacy names: warn and
// amber = accent, error = danger, info = neutral). variant: "outline" | "plane" (plane: ok / danger carry `ground` text).
// ok shows a check and danger a cross unless `icon` says otherwise ("none" | "warn" | "check" | "cross"); colour is never alone.
// `small` is the 16 px diagnostics variant. Text from peers, plugins and the server is plain text.
Rectangle {
    id: root
    property string tone: "neutral"
    property string variant: "outline"
    property string text: ""
    property string icon: "auto"
    property bool small: false
    property bool disabled: false
    property int textFormat: Text.PlainText
    readonly property string kind: tone === "warn" || tone === "amber" ? "accent"
                                  : tone === "error" ? "danger" : tone === "info" ? "neutral" : tone
    readonly property bool plane: variant === "plane"
    // Legacy texts that start with a symbol ("● Online", "▲ Conflict"): no extra icon then.
    readonly property bool textHasSymbol: /^[●▲✕⟳↓❚◆✓]/.test(text)
    readonly property string iconKind: icon !== "auto" ? icon : (textHasSymbol || small ? "none" : kind === "ok" ? "check" : kind === "danger" ? "cross" : (tone === "warn" || tone === "amber") ? "warn" : "none")
    readonly property color fg: disabled ? Theme.disabledInk
        : plane ? (kind === "accent" ? Theme.inkOnAccent : kind === "neutral" ? Theme.ink : Theme.ground)
        : (kind === "accent" ? Theme.ink : kind === "ok" ? Theme.ink : kind === "danger" ? Theme.danger : Theme.ink2)
    readonly property color edge: disabled ? Theme.disabledRule
        : kind === "accent" ? (plane ? Theme.accent : Theme.ruleControl) : kind === "ok" ? Theme.ok : kind === "danger" ? Theme.danger
        : (plane ? Theme.pressed : Theme.rule)
    readonly property color iconColor: disabled ? Theme.disabledInk : (plane ? fg : (kind === "ok" ? Theme.ok : fg))

    implicitHeight: small ? 16 : 18
    implicitWidth: probe.implicitWidth + (iconKind !== "none" ? 14 : 0) + 2 * 6 + 2
    radius: 0
    color: plane && !disabled ? (kind === "accent" ? Theme.accent : kind === "ok" ? Theme.ok : kind === "danger" ? Theme.danger : Theme.pressed) : "transparent"
    border.width: 1
    border.color: edge

    Text {
        id: probe
        visible: false
        textFormat: root.textFormat
        text: root.text
        font: label.font
    }
    Row {
        id: row
        anchors.centerIn: parent
        spacing: 4
        UzGlyph {
            visible: root.iconKind === "check" || root.iconKind === "cross"
            anchors.verticalCenter: parent.verticalCenter
            name: root.iconKind === "check" ? "check" : "cross"
            size: 10
            strokeWidth: 1.6
            color: root.iconColor
        }
        Rectangle {   // warning: the Yamabuki square with an ink "!" (r6), the text next to it stays ink
            objectName: "tagWarnSquare"
            visible: root.iconKind === "warn"
            anchors.verticalCenter: parent.verticalCenter
            width: 12; height: 12
            color: root.disabled ? "transparent" : Theme.accent
            Text {
                anchors.centerIn: parent
                text: "!"
                font.family: Theme.sans
                font.pixelSize: Theme.roles.meta.size
                font.weight: Font.Bold
                color: root.disabled ? Theme.disabledInk : Theme.inkOnAccent
            }
        }
        Text {
            id: label
            textFormat: root.textFormat
            text: root.text
            elide: Text.ElideRight
            width: Math.min(probe.implicitWidth, Math.max(0, root.width - 14 - (root.iconKind !== "none" ? 14 : 0)))
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            font.weight: Font.Medium
            font.letterSpacing: 0.6
            font.capitalization: root.small ? Font.MixedCase : Font.AllUppercase
            color: root.fg
        }
    }
}
