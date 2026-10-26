import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// RunHeader of the running game (boards 3g and 3r, design-system.md "RunHeader"): 56 px, `ground`, one `rule` line below.
// Left: back to the Library, a rule, the title (elided, the full title is its tooltip), the system chip, the sub line ("Shared · 2
// watching" / "Not shared"; in the Multiview "<server> · N people" and the layout switch). Right: the watchers as avatars and the
// header buttons. Session: Speed-up (with its speed menu), Sound, Layout, Multiview, Panel (toggled), the Fullscreen icon button,
// Pause / Resume as the one primary and the "⋯" menu (Diagnostics, Screenshot, Hotkeys, Reset game, Report a problem). Multiview:
// the audio label, Speed-up, Layout, Add tile, Panel, Leave, Fullscreen, Pause and "⋯". Watching only: the Session as title, Panel,
// Leave, Fullscreen. When the width runs out (windows below about 1100 px, wide fonts) the optional parts give way in four steps
// (`level`) and their functions move into the "⋯" menu, so every function stays reachable; Pause / Resume never moves.
// The Add tile popover and the Reset dialog live in the game screen (they cover the stage); this header reports the clicks.
Rectangle {
    id: root
    objectName: "gameHeader"
    required property PlayerController player
    property bool panelShown: true
    property string popover: ""          // the popover of the game screen that is open ("picker"), for the pressed look of its button
    property var hotkeys: ({})
    readonly property GameSession session: player.gameSession
    readonly property SessionController ctl: player.sessions
    readonly property DiagnosticsModel diag: ctl.diagnostics
    readonly property bool own: session.active
    // Multiview: tab "multiview" with a game of your own or several surfaces; one remote Session alone is "watching".
    readonly property bool multi: ctl.tab !== "session" && (own || ctl.surfaceCount > 1)
    readonly property bool watchingOnly: !own && !multi
    readonly property bool segmentFocused: modeSegment.activeFocus || viewButton.activeFocus || leaveMulti.activeFocus
    readonly property var layouts: session.active ? session.screenLayouts : (ctl.watching ? ctl.watchLayouts : [])
    readonly property Item addAnchor: addButton
    readonly property Item moreAnchor: moreButton
    readonly property Item layoutAnchor: layoutButton
    readonly property Item speedAnchor: speedValueButton
    readonly property bool menuOpen: moreMenu.opened || layoutMenu.opened || speedMenu.opened || soundPopover.opened
    readonly property var audio: player.audio
    signal fullscreenRequested()
    signal resetRequested()
    signal gameSettingsRequested()
    signal hotkeysRequested()   // "⋯ › Hotkeys": the hotkey sheet over the game
    signal panelToggled()
    signal pickerToggled()
    signal layoutMenuClosed()

    color: Theme.ground
    implicitHeight: Theme.header56
    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule }

    function key(label) { return label !== undefined && label !== "" ? label : "" }
    readonly property string shareText: ctl.shared ? (ctl.visibility === "invite_only" ? qsTr("Invite only · %1 watching") : qsTr("Shared · %1 watching")).arg(ctl.viewerCount)
                                                  : qsTr("Not shared")
    readonly property var watchLink: ctl.surfaceLinks[ctl.surfaceOrder.length > 0 ? ctl.surfaceOrder[0] : ""] || null
    // System code of the running game: the selected game is the running one while the game view is shown.
    readonly property string systemCode: {
        const g = player.selectedGame
        return own && g && g.title === session.title && g.system ? String(g.system) : ""
    }
    // Watchers as avatars: the people watching your game; in the Multiview the people whose tiles are shown.
    readonly property var watchers: {
        const out = []
        if (multi) {
            const o = ctl.surfaceOrder, info = ctl.surfaceInfo
            for (let i = 0; i < o.length; ++i) {
                const who = o[i] === "local" ? (ctl.ownName !== "" ? ctl.ownName : qsTr("You")) : ((info[o[i]] || ({})).who || "")
                if (who !== "") out.push({ name: who, person: i + 1 })
            }
        } else if (own) {
            const p = ctl.participants
            for (let i = 0; i < p.length; ++i) if (p[i].kind === "viewer") out.push({ name: p[i].name, person: out.length + 2 })
        }
        return out
    }
    readonly property string audioWho: {
        const f = ctl.audioFocus
        if (f === "local") return ctl.ownName !== "" ? ctl.ownName : qsTr("You")
        return (ctl.surfaceInfo[f] || ({})).who || ""
    }
    readonly property string multiSub: qsTr("%1 · %2").arg(player.hubName !== "" ? player.hubName : qsTr("Server"))
                                            .arg(ctl.surfaceCount === 1 ? qsTr("1 person") : qsTr("%1 people").arg(ctl.surfaceCount))
    // The three modes never change, so the segments are created once (a Repeater that rebuilds its segments inside the nested
    // header layouts crashes Qt 6.4); the label and the disabled state of Grid are read live through getters.
    readonly property var modeOptions: [
        { value: "pip", label: qsTr("PiP"), name: "modePip" },
        { value: "row", label: qsTr("Row"), name: "modeRow" },
        { get value() { return "grid" }, get label() { return root.ctl.gridAvailable ? qsTr("Grid 2×2") : qsTr("Grid 2×2 · from 4") },
          name: "modeGrid", get disabled() { return !root.ctl.gridAvailable } }
    ]
    readonly property bool modeAvailable: multi && ctl.availableLayouts.length > 0

    // Narrow headers: 0 everything, 1 no avatars, no Layout button and no speed menu button, 2 also no Panel / Multiview button,
    // 3 also no Pause and Speed-up button. The widths are the implicit ones, so the result does not depend on the level itself.
    readonly property real availWidth: headerRow.width
    readonly property int level: {
        const gap = Theme.space1
        const left = backButton.implicitWidth + 2 * Theme.space2 + 1 + 100 + (systemCode !== "" ? 60 : 0)
                     + (modeAvailable ? modeSegment.implicitWidth + Theme.space2 : 0)
        for (let l = 0; l <= 3; ++l) {
            let w = 0, n = 0
            const add = (x) => { w += x + (n > 0 ? gap : 0); ++n }
            if (watchingOnly) {
                if (ctl.tab !== "session" && ctl.surfaceCount >= 1) add(addButton.implicitWidth)
                add(volumeButton.implicitWidth); add(panelButton.implicitWidth); add(leaveButton.implicitWidth)
                if (l < 2) add(fullscreenButton.implicitWidth)
                add(moreButton.implicitWidth)
            } else {
                if (l < 1 && watchers.length > 0) add(watchers.length * 16 + 8 + Theme.space1)
                if (own) add(pauseButton.implicitWidth)
                if (l < 3 && own && session.fastForwardAvailable) add(fastForwardButton.implicitWidth + (l < 1 ? speedValueButton.implicitWidth : 0))
                add(volumeButton.implicitWidth)
                if (l < 1 && layouts.length > 1) add(layoutButton.implicitWidth)
                if (!multi && l < 2) add(viewButton.implicitWidth)
                if (multi && ctl.surfaceCount >= 1) add(addButton.implicitWidth)
                if (l < 2) add(panelButton.implicitWidth)
                if (multi) add(leaveMulti.implicitWidth)
                if (l < 2) add(fullscreenButton.implicitWidth)
                if (own || multi) add(moreButton.implicitWidth)
                if (multi && l < 1) add(audioLabel.implicitWidth)
            }
            if (left + w <= availWidth) return l
        }
        return 3
    }

    function leaveMultiview() {
        if (root.own) root.ctl.tab = "session"
        else root.player.leaveGameView()
    }

    // ---- menus ("⋯", speed, screen layout) ----
    function layoutLabel(v) { return root.ctl.layoutLabel(v) }
    // Layout menu (F7): the layouts, "Swap screens" (Shift+F7; only for layouts that show both screens) and, for your own
    // game, "Bottom screen in its own window". Choosing an item closes the menu, like every menu.
    readonly property bool swapAllowed: root.ctl.screenLayout === "stacked" || root.ctl.screenLayout === "side" || root.ctl.screenLayout === "large"
    readonly property string layoutKey: root.key(root.hotkeys.layout)
    readonly property var layoutItems: {
        const it = root.layouts.map(function (v) { return { label: root.layoutLabel(v), name: "layout_" + v, value: v, shortcut: "" } })
        it.push({ separator: true })
        it.push({ label: qsTr("Swap screens"), name: "swap", hint: root.ctl.screenSwap ? qsTr("On") : "", disabled: !root.swapAllowed || root.ctl.bottomWindow,
                  shortcut: root.layoutKey !== "" && root.layoutKey.indexOf("+") < 0 ? "Shift+" + root.layoutKey : "" })
        if (root.own && !root.multi)
            it.push({ label: qsTr("Bottom screen in its own window"), name: "bottomWindow", hint: root.ctl.bottomWindow ? qsTr("On") : "" })
        return it
    }
    // F7: opens the layout menu below the Layout button (the "⋯" button when the header is narrow).
    function toggleLayoutMenu() {
        if (root.layouts.length < 2) return
        if (layoutMenu.opened) { layoutMenu.close(); return }
        layoutMenu.openFor(layoutButton.visible ? layoutButton : moreButton, "below", true)
    }
    function chooseLayoutItem(item) {
        if (item.name === "swap") root.ctl.chooseScreenSwap(!root.ctl.screenSwap)
        else if (item.name === "bottomWindow") root.ctl.chooseBottomWindow(!root.ctl.bottomWindow)
        else if (item.value !== undefined) root.ctl.chooseScreenLayout(item.value)
    }
    readonly property var speedItems: root.session.speedUpRatios.map(function (r) { return { label: r + "×", name: "speed_" + r, value: Number(r) } })
    // "⋯": Diagnostics (F3), Screenshot (F12), Settings for this game, Hotkeys, Reset game …, Report a problem …. Parts that
    // left the header because it is narrow (`level`) are listed first, so every function stays reachable.
    readonly property var moreItems: {
        const it = []
        const cur = (cond) => cond ? qsTr("Current") : ""
        const playing = root.own || root.multi
        if (playing) {
            if (root.level >= 3 && root.own && root.session.fastForwardAvailable)
                it.push({ label: root.session.fastForward ? qsTr("Speed-up off") : qsTr("Speed-up"), shortcut: root.key(root.hotkeys.speedup), name: "speedup" })
            if (root.level >= 2 && !root.watchingOnly) {
                it.push({ label: root.panelShown ? qsTr("Hide panel") : qsTr("Show panel"), name: "panel" })
                if (!root.multi && root.own) it.push({ label: qsTr("Multiview"), name: "multiview" })
            }
            if (root.level >= 1) {
                for (let i = 0; i < root.layouts.length && root.layouts.length > 1; ++i)
                    it.push({ label: qsTr("Screen layout: %1").arg(root.layoutLabel(root.layouts[i])), hint: cur(root.ctl.screenLayout === root.layouts[i]), name: "layout_" + root.layouts[i] })
                if (root.layouts.length > 1 && root.own)
                    it.push({ label: qsTr("Swap screens"), hint: root.ctl.screenSwap ? qsTr("On") : "", name: "swap", disabled: !root.swapAllowed || root.ctl.bottomWindow })
                if (root.own && root.session.fastForwardAvailable) {
                    const r = root.session.speedUpRatios
                    for (let i = 0; i < r.length; ++i)
                        it.push({ label: qsTr("Speed-up speed: %1×").arg(r[i]), hint: cur(Math.abs(root.session.fastForwardRatio - Number(r[i])) < 0.001), name: "speed_" + r[i] })
                }
            }
            if (root.level >= 2)
                it.push({ label: qsTr("Fullscreen"), shortcut: root.key(root.hotkeys.fullscreen), name: "fullscreen" })
        } else if (root.level >= 2) {
            it.push({ label: qsTr("Fullscreen"), shortcut: root.key(root.hotkeys.fullscreen), name: "fullscreen" })
        }
        it.push({ label: root.diag.open ? qsTr("Hide diagnostics") : qsTr("Diagnostics"), shortcut: root.key(root.hotkeys.diagnostics), name: "diagnostics" })
        if (root.own && root.session.active)
            it.push({ label: qsTr("Screenshot"), shortcut: root.key(root.hotkeys.screenshot), name: "screenshot" })
        if (root.own) it.push({ label: qsTr("Settings for this game…"), name: "gameSettings" })
        it.push({ label: qsTr("Hotkeys"), hint: qsTr("Hold Select on a pad"), name: "hotkeys" })
        if (root.own) it.push({ label: qsTr("Reset game …"), danger: true, name: "reset" })
        it.push({ separator: true })
        it.push({ label: qsTr("Report a problem …"), name: "report" })
        return it
    }
    function runMore(name) {
        if (name === "pause") root.session.togglePause()
        else if (name === "speedup") root.session.toggleFastForward()
        else if (name === "panel") root.panelToggled()
        else if (name === "multiview") root.ctl.tab = "multiview"
        else if (name.indexOf("layout_") === 0) root.ctl.chooseScreenLayout(name.substring(7))
        else if (name === "swap") root.ctl.chooseScreenSwap(!root.ctl.screenSwap)
        else if (name.indexOf("speed_") === 0) root.session.fastForwardRatio = Number(name.substring(6))
        else if (name === "screenshot") root.player.screenshots.capture()
        else if (name === "fullscreen") root.fullscreenRequested()
        else if (name === "diagnostics") root.diag.toggle()
        else if (name === "reset") root.resetRequested()
        else if (name === "gameSettings") root.gameSettingsRequested()
        else if (name === "hotkeys") root.hotkeysRequested()
        else if (name === "report") root.player.report.show()
    }
    UzMenu {
        id: moreMenu
        objectName: "morePopover"
        items: root.moreItems
        menuWidth: 280
        onChosen: (item) => root.runMore(item.name)
    }
    UzMenu {
        id: layoutMenu
        objectName: "layoutPopover"
        onClosed: root.layoutMenuClosed()
        items: root.layoutItems
        selectable: true
        value: root.ctl.screenLayout
        menuWidth: 280
        onChosen: (item) => root.chooseLayoutItem(item)
        list.footer: Text {
            x: Theme.space15
            height: 28
            verticalAlignment: Text.AlignVCenter
            text: root.multi ? qsTr("Applies to all tiles") : qsTr("This game only")
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
    }
    UzMenu {
        id: speedMenu
        objectName: "speedPopover"
        items: root.speedItems
        selectable: true
        value: Number(Math.round(root.session.fastForwardRatio * 10) / 10)
        menuWidth: 160
        onChosen: (item) => root.session.fastForwardRatio = Number(item.value)
    }

    // Sound (board 3g-9): the master volume of this PC. Voice and plugin sound rows come with 0.21.
    UzPopover {
        id: soundPopover
        objectName: "soundPopover"
        label: qsTr("Sound")
        popWidth: 320
        UzSlider {
            id: soundSlider
            objectName: "soundVolumeSlider"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.space2
            Layout.rightMargin: Theme.space2
            Layout.topMargin: Theme.space1
            label: qsTr("My game")
            from: 0
            to: 100
            stepSize: root.audio.volumeStep
            muted: root.audio.muted
            onMoved: root.audio.setVolume(Math.round(value))
            Binding { target: soundSlider; property: "value"; value: root.audio.volume }   // survives the key nudges
        }
        Text {
            objectName: "soundBackgroundLine"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.space2
            Layout.rightMargin: Theme.space2
            Layout.topMargin: Theme.space1
            Layout.bottomMargin: Theme.space15
            text: qsTr("Muted in the background: %1").arg(root.audio.muteInBackground ? qsTr("on") : qsTr("off"))
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
        actions: [
            UzButton {
                objectName: "soundMuteButton"
                size: "sm"
                text: root.audio.muted ? qsTr("Unmute") : qsTr("Mute all")
                onClicked: root.audio.setMuted(!root.audio.muted)
            },
            UzButton {
                objectName: "soundSettingsButton"
                size: "sm"
                kind: "link"
                text: qsTr("Sound settings")
                onClicked: { soundPopover.close(); root.player.leaveGameView(); root.player.showSettingsSection("audio") }
            }
        ]
    }

    RowLayout {
        id: headerRow
        anchors.fill: parent
        anchors.leftMargin: Theme.space3
        anchors.rightMargin: Theme.space3
        spacing: Theme.space2

        // 1 Back
        UzButton {
            id: backButton
            objectName: "backToLibraryButton"
            kind: "link"
            focusPolicy: Qt.TabFocus
            text: qsTr("← Library")
            font.pixelSize: Theme.roles.buttonSm.size
            font.weight: Font.Normal
            hPadding: 0
            Accessible.name: qsTr("Library")
            ToolTip.visible: hovered && !root.menuOpen
            ToolTip.delay: 500
            ToolTip.text: root.own ? qsTr("Game keeps running, paused") : qsTr("Leave the Session and go to the Library")
            onClicked: root.player.leaveGameView()
        }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 16; color: Theme.rule }

        // 2 Identity: title, system chip, sub, layout switch
        RowLayout {
            id: ident
            objectName: "headerContext"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.space2
            Text {
                id: titleText
                objectName: root.multi ? "multiviewTitle" : "gameTitle"
                // The title keeps its full width first; the sub text shrinks before it. fillWidth + maximumWidth = natural width: it
                // never grows but shrinks (an item without fillWidth would not). The natural width comes from TextMetrics: an elided
                // Text can report its elided width as implicitWidth, which then pinned the title to "Rev…".
                Layout.fillWidth: true
                Layout.preferredWidth: titleMetrics.advanceWidth + 1
                Layout.maximumWidth: titleMetrics.advanceWidth + 1
                Layout.minimumWidth: Math.min(titleMetrics.advanceWidth + 1, 100)
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: root.multi ? qsTr("Multiview")
                      : root.own ? root.session.title
                      : qsTr("%1 · %2").arg(root.ctl.watchedWho).arg(root.ctl.watchedGame)
                font.family: Theme.sans
                font.pixelSize: Theme.roles.headerTitle.size
                font.weight: Theme.roles.headerTitle.weight
                color: Theme.ink
                Accessible.name: text
                TextMetrics { id: titleMetrics; font: titleText.font; text: titleText.text }
                HoverHandler { id: titleHover }
                ToolTip.visible: titleHover.hovered && text !== ""
                ToolTip.delay: 500
                ToolTip.text: text
            }
            UzSystemChip {
                visible: root.systemCode !== "" && !root.multi
                system: root.systemCode
            }
            Text {
                id: subText
                objectName: root.multi ? "multiSub" : (root.own ? "sharePill" : "watchLinkPill")
                visible: text !== "" && !(root.multi && root.level >= 1)
                Layout.fillWidth: true
                Layout.preferredWidth: subMetrics.advanceWidth + 1
                Layout.maximumWidth: subMetrics.advanceWidth + 1
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                textFormat: Text.PlainText
                text: root.multi ? root.multiSub
                      : root.own ? root.shareText
                      : (root.watchLink ? root.watchLink.text : "")
                font.family: Theme.sans
                font.pixelSize: Theme.roles.buttonSm.size
                color: Theme.muted
                TextMetrics { id: subMetrics; font: subText.font; text: subText.text }
            }
            UzSegmented {
                id: modeSegment
                objectName: "modeSegment"
                visible: root.modeAvailable
                Layout.fillWidth: true
                Layout.preferredWidth: implicitWidth
                Layout.maximumWidth: implicitWidth
                Layout.minimumWidth: 0
                segmentHeight: Theme.row28
                current: root.ctl.multiviewMode
                // PiP | Row | Grid 2×2; Grid is disabled below four Sessions ("Grid 2×2 · from 4", ADR 0026 D2)
                options: root.modeOptions
                onPicked: (v) => root.ctl.multiviewMode = v
            }
            Item { Layout.fillWidth: true; Layout.preferredWidth: 0; Layout.minimumWidth: 0 }
        }

        // 3 End: audio label, watchers, header buttons
        RowLayout {
            id: endRow
            spacing: Theme.space1
            Text {
                id: audioLabel
                objectName: "audioLabel"
                visible: root.multi && root.level < 1 && root.audioWho !== ""
                Layout.rightMargin: Theme.space1
                text: qsTr("Audio · %1").arg(root.audioWho)
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: 10
                font.letterSpacing: 0.8
                font.capitalization: Font.AllUppercase
                color: Theme.dim
            }
            UzAvatarStack {
                id: avatars
                objectName: "watcherAvatars"
                visible: root.watchers.length > 0 && root.level < 1 && !root.watchingOnly
                Layout.rightMargin: Theme.space1
                people: root.watchers
            }
            UzButton {
                id: fastForwardButton
                objectName: "fastForwardButton"
                visible: root.own && root.session.fastForwardAvailable && root.level < 3
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: root.session.fastForward ? qsTr("Speed-up on · %1×").arg(Math.round(root.session.fastForwardRatio * 10) / 10)
                                               : qsTr("Speed-up %1×").arg(Math.round(root.session.fastForwardRatio * 10) / 10)
                on: root.session.fastForward
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: qsTr("Speed-up") + (root.key(root.hotkeys.speedup) !== "" ? " · " + root.hotkeys.speedup : "")
                onClicked: root.session.toggleFastForward()
            }
            UzIconButton {
                id: speedValueButton
                objectName: "speedValueButton"
                visible: root.own && root.session.fastForwardAvailable && root.level < 1
                focusPolicy: Qt.TabFocus
                glyph: "chevronDown"
                glyphSize: 12
                on: speedMenu.opened
                Accessible.name: qsTr("Speed-up speed")
                onClicked: speedMenu.opened ? speedMenu.close() : speedMenu.openFor(speedValueButton, "below", true)
            }
            UzIconButton {
                id: volumeButton
                objectName: "volumeButton"
                focusPolicy: Qt.TabFocus
                glyph: root.audio.effectiveMuted ? "volumeOff" : "volume"
                on: soundPopover.opened
                Accessible.name: qsTr("Sound")
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: root.audio.muted ? qsTr("Sound · muted") : qsTr("Sound · %1 %").arg(root.audio.volume)
                onClicked: soundPopover.opened ? soundPopover.close() : soundPopover.openFor(volumeButton, "below", "end")
            }
            UzButton {
                id: layoutButton
                objectName: "layoutSwitch"
                visible: root.layouts.length > 1 && root.level < 1
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: qsTr("Layout")
                on: layoutMenu.opened
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: root.multi ? qsTr("Screen layout · all tiles") : qsTr("Screen layout · this game only")
                onClicked: layoutMenu.opened ? layoutMenu.close() : layoutMenu.openFor(layoutButton, "below", true)
            }
            UzButton {
                id: viewButton
                objectName: "tabMultiview"
                visible: !root.multi && root.own && root.level < 2
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: qsTr("Multiview")
                onClicked: root.ctl.tab = "multiview"
            }
            UzButton {
                id: addButton
                objectName: "addSessionToggle"
                visible: root.ctl.tab !== "session" && root.ctl.surfaceCount >= 1
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: qsTr("Add tile")
                on: root.popover === "picker"
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: qsTr("Running sessions · %1 of %2 tiles").arg(root.ctl.surfaceCount).arg(root.ctl.maxSurfaces)
                onClicked: root.pickerToggled()
            }
            UzButton {
                id: panelButton
                objectName: "panelToggle"
                visible: root.level < 2 || root.watchingOnly
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: qsTr("Panel")
                on: root.panelShown
                Accessible.name: root.panelShown ? qsTr("Hide panel") : qsTr("Show panel")
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: root.panelShown ? qsTr("Hide panel") : qsTr("Show panel")
                onClicked: root.panelToggled()
            }
            UzButton {
                id: leaveMulti
                objectName: "leaveMultiview"
                visible: root.multi
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: qsTr("Leave")
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: root.own ? qsTr("Back to your game") : qsTr("Leave the Session")
                onClicked: root.leaveMultiview()
            }
            UzButton {
                id: leaveButton
                objectName: "leaveButton"
                visible: root.watchingOnly
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: qsTr("Leave")
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: qsTr("Leave the Session")
                onClicked: root.player.leaveGameView()
            }
            UzIconButton {
                id: fullscreenButton
                objectName: "fullscreenButton"
                visible: root.level < 2
                focusPolicy: Qt.TabFocus
                glyph: "fullscreen"
                Accessible.name: qsTr("Fullscreen")
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: qsTr("Fullscreen") + " · " + (root.key(root.hotkeys.fullscreen) !== "" ? root.hotkeys.fullscreen : "F11")
                onClicked: root.fullscreenRequested()
            }
            UzButton {
                id: pauseButton
                objectName: "pauseButton"
                visible: root.own
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: root.session.paused ? qsTr("Resume") : qsTr("Pause")
                kind: "primary"
                enabled: root.session.state === GameSession.Running || root.session.state === GameSession.Paused
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: (root.session.paused ? qsTr("Resume") : qsTr("Pause")) + (root.multi ? qsTr(" your game") : "") + " · Esc"
                onClicked: root.session.togglePause()
            }
            UzIconButton {
                id: moreButton
                objectName: "moreButton"
                focusPolicy: Qt.TabFocus
                glyph: "more"
                on: moreMenu.opened
                Accessible.name: qsTr("More")
                onClicked: moreMenu.opened ? moreMenu.close() : moreMenu.openFor(moreButton, "below", true)
            }
        }
    }
}
