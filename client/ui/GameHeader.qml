import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// RunHeader of the running game (boards 7-1 and 7-2, design-system.md "RunHeader"): 56 px, `ground`, one `rule` line below.
// Left: back to the Library, a rule, the title (at least 160 px, the full title is its tooltip) with the system chip and under it
// the sub line ("Not shared · Saved 2 min ago"; in the Multiview "<server> · N people" and the layout switch). Right: the watchers
// as avatars and the control bar: every control is a framed button (rule-control) that says what it does and, where it has a state,
// what it is set to in Mono ("» Speed-up | Off · 2×" as a split button with the speeds menu, "Sound | 5 %", "Screens | Side by
// side", "Panel | Shown" as an ink plane when on, "⛶ Fullscreen", "❚❚ Pause" as the one primary with the Esc hint, "⋯").
// Multiview: the audio label, Speed-up, Sound, Screens, Add tile, Panel, Leave, Fullscreen, Pause and "⋯". Watching only: the Session
// as title, Sound, Panel, Leave, Fullscreen. When the width runs out (windows below about 1100 px, wide fonts) the bar gives way in
// four steps (`level`): words and values go first, then Screens and Fullscreen move into the "⋯" menu with their value,
// then Multiview, Speed-up and Panel; Pause / Resume never moves.
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
    readonly property TogetherController together: player.together
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
    readonly property SaveHistoryController saves: player.saveHistory
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
    // "Saved 2 min ago" of the running game (the same data the pause dialog used); empty while the saves are not known.
    readonly property string saveText: own && saves.slotsAvailable && saves.available && saves.history.length > 0 && saves.history[0].when !== ""
                                       ? qsTr("Saved %1").arg(saves.history[0].when) : ""
    readonly property string shareText: ctl.shared ? (ctl.visibility === "invite_only" ? qsTr("Invite only · %1 watching") : qsTr("Shared · %1 watching")).arg(ctl.viewerCount)
                                                  : qsTr("Not shared")
    readonly property int cheatsOn: player.plugins.cheatsOn
    readonly property string cheatsText: cheatsOn > 0 ? (cheatsOn === 1 ? qsTr("1 cheat on") : qsTr("%1 cheats on").arg(cheatsOn)) : ""
    readonly property string ownSub: {
        const parts = player.gameRun.id ? [qsTr("Run: %1").arg(player.gameRun.name || ""), shareText] : [shareText]
        if (saveText !== "") parts.push(saveText)
        if (cheatsText !== "") parts.push(cheatsText)
        return parts.join(" · ")
    }
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

    // Values of the control bar (Mono, uppercase on screen).
    readonly property string speedValue: (session.fastForward ? qsTr("On") : qsTr("Off")) + " · " + (Math.round(session.fastForwardRatio * 10) / 10) + "×"
    readonly property string soundValue: audio.muted ? qsTr("Muted") : qsTr("%1 %").arg(audio.volume)
    readonly property string screensValue: layoutLabel(ctl.screenLayout)
    readonly property string panelValue: panelShown ? qsTr("Shown") : qsTr("Hidden")
    readonly property string pauseHint: "Esc"
    readonly property string pauseLabel: session.paused ? "▶ " + qsTr("Resume") : "❚❚ " + qsTr("Pause")

    // Narrow headers (board 7-2 section 06): 0 everything; 1 words and values give way (Speed-up shows "»" and its value, Screens and
    // Panel lose their value, no avatars, no Esc hint); 2 Screens, Fullscreen and the speeds chevron move into "⋯";
    // 3 also Multiview, Speed-up and Panel (in the Multiview the mode switch too), and Sound shows only its value. Pause / Resume never moves. The widths are computed from the strings and the fonts (not from the
    // buttons), so the result does not depend on the level itself.
    readonly property real availWidth: headerRow.width
    FontMetrics { id: sansFm; font.family: Theme.sans; font.pixelSize: Theme.roles.buttonSm.size }
    FontMetrics { id: monoFm; font.family: Theme.mono; font.pixelSize: Theme.roles.meta.size; font.weight: Font.Medium; font.letterSpacing: 0.66 }
    // Width of a framed control-bar button: 24 padding, label, value (Mono, uppercase, letter-spaced), chevron.
    function bw(label, value, chevron) {
        const l = label !== "" ? Math.ceil(sansFm.advanceWidth(label)) : 0
        const v = value !== "" ? Math.ceil(monoFm.advanceWidth(value.toUpperCase())) + 2 : 0
        return Math.max(72, 24 + l + (v > 0 ? (l > 0 ? 8 : 0) + v : 0) + (chevron ? ((l > 0 || v > 0) ? 8 : 0) + 12 : 0))
    }
    readonly property real wSpeed0: bw("» " + qsTr("Speed-up"), speedValue, false)
    readonly property real wSpeed1: bw("»", speedValue, false)
    readonly property real wSound: bw(qsTr("Sound"), soundValue, true)
    readonly property real wSound3: bw("", soundValue, true)
    readonly property real wScreens0: bw(qsTr("Screens"), screensValue, true)
    readonly property real wScreens1: bw(qsTr("Screens"), "", true)
    readonly property real wPanel0: bw(qsTr("Panel"), panelValue, false)
    readonly property real wPanel1: bw(qsTr("Panel"), "", false)
    readonly property real wFullscreen: bw("⛶ " + qsTr("Fullscreen"), "", false)
    readonly property real wPause0: bw(pauseLabel, pauseHint, false)
    readonly property real wPause1: bw(pauseLabel, "", false)
    readonly property real wMulti: bw(qsTr("Multiview"), "", false)
    readonly property int level: {
        const gap = Theme.space1
        const left0 = backButton.implicitWidth + 2 * Theme.space2 + 1 + 160 + (systemCode !== "" ? 60 : 0)
        const more = moreButton.implicitWidth
        for (let l = 0; l <= 3; ++l) {
            const left = left0 + (modeAvailable && l < 3 ? modeSegment.implicitWidth + Theme.space2 : 0)
            let w = 0, n = 0
            const add = (x) => { w += x + (n > 0 ? gap : 0); ++n }
            if (watchingOnly) {
                if (ctl.tab !== "session" && ctl.surfaceCount >= 1) add(addButton.implicitWidth)
                add(l < 3 ? wSound : wSound3); add(l < 1 ? wPanel0 : wPanel1); add(leaveButton.implicitWidth)
                if (l < 2) add(wFullscreen)
                add(more)
            } else {
                if (l < 1 && watchers.length > 0) add(watchers.length * 16 + 8 + Theme.space1)
                if (own) add(l < 1 ? wPause0 : wPause1)
                if (l < 3 && own && session.fastForwardAvailable) add((l < 1 ? wSpeed0 : wSpeed1) + (l < 2 ? 32 - 1 : 0))
                add(l < 3 ? wSound : wSound3)
                if (l < 2 && layouts.length > 1) add(l < 1 ? wScreens0 : wScreens1)
                if (!multi && own && l < 3) add(wMulti)
                if (multi && ctl.surfaceCount >= 1) add(addButton.implicitWidth)
                if (l < 3) { add(1); add(l < 1 ? wPanel0 : wPanel1) }
                if (multi) add(leaveMulti.implicitWidth)
                if (l < 2) add(wFullscreen)
                if (own || multi) add(more)
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
    // "⋯" (board 7-2 section 05): Diagnostics F3, HUD widgets F4, Screenshot F12, Hotkeys, Settings for this game…, a rule, Report a
    // problem…, Reset game… (danger). Parts that left the bar because it is narrow (`level`) are listed first, with their value, so
    // every function stays reachable.
    readonly property var moreItems: {
        const it = []
        const playing = root.own || root.multi
        if (playing) {
            if (root.level >= 3 && root.own && root.session.fastForwardAvailable)
                it.push({ label: qsTr("Speed-up"), hint: root.speedValue, shortcut: root.key(root.hotkeys.speedup), name: "speedup" })
            if (root.level >= 3 && !root.watchingOnly)
                it.push({ label: root.panelShown ? qsTr("Hide panel") : qsTr("Show panel"), hint: root.panelValue, name: "panel" })
            if (root.level >= 3 && !root.multi && root.own)
                it.push({ label: qsTr("Multiview"), name: "multiview" })
            if (root.level >= 2 && root.layouts.length > 1)
                it.push({ label: qsTr("Screens"), hint: root.screensValue, name: "screens" })
            if (root.level >= 2 && root.own && root.session.fastForwardAvailable) {
                const r = root.session.speedUpRatios
                for (let i = 0; i < r.length; ++i)
                    it.push({ label: qsTr("Speed-up speed: %1×").arg(r[i]),
                              hint: Math.abs(root.session.fastForwardRatio - Number(r[i])) < 0.001 ? qsTr("Current") : "", name: "speed_" + r[i] })
            }
            if (root.level >= 3 && root.modeAvailable) {
                for (let i = 0; i < root.modeOptions.length; ++i) {
                    const m = root.modeOptions[i]
                    it.push({ label: qsTr("Multiview: %1").arg(m.label), hint: root.ctl.multiviewMode === m.value ? qsTr("Current") : "", name: m.name, disabled: m.disabled === true })
                }
            }
            if (root.level >= 2)
                it.push({ label: qsTr("Fullscreen"), shortcut: root.key(root.hotkeys.fullscreen), name: "fullscreen" })
        } else if (root.level >= 2) {
            it.push({ label: qsTr("Fullscreen"), shortcut: root.key(root.hotkeys.fullscreen), name: "fullscreen" })
        }
        if (it.length > 0) it.push({ separator: true })
        // Together (ADR 0033 D3): one click each, enabled while a run or a Session is the scope of this game
        it.push({ label: qsTr("Ready check…"), name: "readyCheck", disabled: !root.together.available })
        it.push({ label: root.together.paused ? qsTr("Resume for everyone") : qsTr("Pause for everyone"), shortcut: "Shift+Esc", name: "sharedPause", disabled: !root.together.available })
        it.push({ separator: true })
        it.push({ label: root.diag.open ? qsTr("Hide diagnostics") : qsTr("Diagnostics"), shortcut: root.key(root.hotkeys.diagnostics) || "F3", name: "diagnostics" })
        it.push({ label: qsTr("HUD widgets"), shortcut: "F4", name: "hud", disabled: root.player.plugins.hudWidgets.length === 0 })
        if (root.own && root.session.active)
            it.push({ label: qsTr("Screenshot"), shortcut: root.key(root.hotkeys.screenshot) || "F12", name: "screenshot" })
        it.push({ label: qsTr("Hotkeys"), hint: qsTr("Hold %1").arg(root.player.controllers.padHotkeyModifier || qsTr("Select")), name: "hotkeys" })
        if (root.own) it.push({ label: qsTr("Settings for this game…"), name: "gameSettings" })
        it.push({ separator: true })
        it.push({ label: qsTr("Report a problem…"), name: "report" })
        if (root.own) it.push({ label: qsTr("Reset game…"), danger: true, name: "reset" })
        return it
    }
    function runMore(name) {
        if (name === "pause") root.session.togglePause()
        else if (name === "readyCheck") root.together.startCheck()
        else if (name === "sharedPause") root.together.togglePause()
        else if (name === "speedup") root.session.toggleFastForward()
        else if (name === "panel") root.panelToggled()
        else if (name === "multiview") root.ctl.tab = "multiview"
        else if (name.indexOf("mode") === 0) root.ctl.multiviewMode = name.substring(4).toLowerCase()
        else if (name === "screens") Qt.callLater(root.toggleLayoutMenu)   // the menu opens after "⋯" has closed
        else if (name.indexOf("layout_") === 0) root.ctl.chooseScreenLayout(name.substring(7))
        else if (name === "swap") root.ctl.chooseScreenSwap(!root.ctl.screenSwap)
        else if (name.indexOf("speed_") === 0) root.session.fastForwardRatio = Number(name.substring(6))
        else if (name === "screenshot") root.player.screenshots.capture()
        else if (name === "fullscreen") root.fullscreenRequested()
        else if (name === "diagnostics") root.diag.toggle()
        else if (name === "hud") root.player.plugins.hudAllHidden = !root.player.plugins.hudAllHidden
        else if (name === "reset") root.resetRequested()
        else if (name === "gameSettings") root.gameSettingsRequested()
        else if (name === "hotkeys") root.hotkeysRequested()
        else if (name === "report") root.player.report.show()
    }
    // The quick menu (GameScreen) opens the same menus anchored at its rows.
    function openSpeedMenuAt(anchor: Item) { if (speedMenu.opened) speedMenu.close(); else speedMenu.openFor(anchor, "below", true) }
    function openSoundAt(anchor: Item) { if (soundPopover.opened) soundPopover.close(); else soundPopover.openFor(anchor, "below", "end") }
    function openLayoutAt(anchor: Item) { if (layoutMenu.opened) layoutMenu.close(); else layoutMenu.openFor(anchor, "below", true) }
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

        // 2 Identity: title with the system chip, the sub line under it, the Multiview mode switch
        RowLayout {
            id: ident
            objectName: "headerContext"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: Theme.space2
            ColumnLayout {
                id: identText
                Layout.fillWidth: true
                Layout.maximumWidth: Math.max(titleMetrics.advanceWidth + 1, subMetrics.advanceWidth + 1) + 70
                                         + (root.player.plugins.developerMode && !root.multi ? devTag.implicitWidth + Theme.space15 : 0)
                Layout.alignment: Qt.AlignVCenter
                spacing: 0
                // Title with the system chip after it: plain positioning (no Layout) so the chip can never sit over the title.
                Item {
                    id: titleRow
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    implicitHeight: titleText.implicitHeight
                    readonly property real titleNatural: titleMetrics.advanceWidth + 1
                    readonly property real chipSpace: chip.implicitWidth + Theme.space15
                    // The chip shows only when the full title and the chip fit; the title shrinks (to 160 px at least) before it hides.
                    readonly property bool chipFits: width >= Math.min(titleNatural, 160) + chipSpace
                    // "Developer mode" tag (board 4d-2F): shown while Developer mode is on, when it fits next to the title and the chip.
                    readonly property real devSpace: devTag.implicitWidth + Theme.space15
                    readonly property bool devFits: (root.own || (root.session.title !== "" && !root.multi && root.session.hasFrame)) && root.player.plugins.developerMode && !root.multi
                                                    && width >= Math.min(titleNatural, 160) + (chip.visible ? chipSpace : 0) + devSpace
                    Text {
                        id: titleText
                        objectName: root.multi ? "multiviewTitle" : "gameTitle"
                        // The natural width comes from TextMetrics: an elided Text can report its elided width as
                        // implicitWidth, which then pinned the title to "Rev…".
                        x: 0
                        width: Math.max(0, Math.min(titleRow.titleNatural, titleRow.width - (chip.visible ? titleRow.chipSpace : 0) - (devTag.visible ? titleRow.devSpace : 0)))
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
                        id: chip
                        visible: root.systemCode !== "" && !root.multi && titleRow.chipFits
                        system: root.systemCode
                        x: titleText.x + titleText.width + Theme.space15
                        anchors.verticalCenter: titleText.verticalCenter
                    }
                    UzTag {
                        id: devTag
                        objectName: "developerModeTag"
                        visible: titleRow.devFits
                        small: true
                        icon: "none"
                        tone: "accent"
                        text: qsTr("Developer mode")
                        x: (chip.visible ? chip.x + chip.width : titleText.x + titleText.width) + Theme.space15
                        anchors.verticalCenter: titleText.verticalCenter
                        ToolTip.visible: devHover.hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Ctrl+Shift+D opens the developer console")
                        HoverHandler { id: devHover }
                    }
                }
                Text {
                    id: subText
                    objectName: root.multi ? "multiSub" : (root.own ? "sharePill" : "watchLinkPill")
                    visible: text !== "" && !(root.multi && root.level >= 1)
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: root.multi ? root.multiSub
                          : root.own ? root.ownSub
                          : (root.watchLink ? root.watchLink.text : "")
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.muted
                    TextMetrics { id: subMetrics; font: subText.font; text: subText.text }
                    HoverHandler { id: subHover }
                    ToolTip.visible: subHover.hovered && subText.truncated && text !== ""
                    ToolTip.delay: 500
                    ToolTip.text: text
                }
            }
            UzSegmented {
                id: modeSegment
                objectName: "modeSegment"
                visible: root.modeAvailable && root.level < 3
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

        // 3 End: audio label, watchers, the control bar
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
                font.pixelSize: Theme.roles.meta.size
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
            // Speed-up as a split button: the left half toggles, the chevron opens the speeds (board 7-1 section 02).
            Row {
                id: speedSplit
                objectName: "speedSplit"
                visible: root.own && root.session.fastForwardAvailable && root.level < 3
                spacing: -1
                UzButton {
                    id: fastForwardButton
                    objectName: "fastForwardButton"
                    size: "sm"
                    focusPolicy: Qt.TabFocus
                    text: root.level < 1 ? "» " + qsTr("Speed-up") : "»"
                    value: root.speedValue
                    inverted: root.session.fastForward
                    Accessible.name: qsTr("Speed-up") + " " + root.speedValue
                    ToolTip.visible: hovered && !root.menuOpen
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Speed-up") + (root.key(root.hotkeys.speedup) !== "" ? " · " + root.hotkeys.speedup : "")
                    onClicked: root.session.toggleFastForward()
                }
                UzIconButton {
                    id: speedValueButton
                    objectName: "speedValueButton"
                    visible: root.level < 2
                    outlined: true
                    implicitWidth: Theme.row28
                    implicitHeight: fastForwardButton.implicitHeight
                    height: fastForwardButton.height
                    focusPolicy: Qt.TabFocus
                    glyph: "chevronDown"
                    glyphSize: 14
                    on: speedMenu.opened
                    Accessible.name: qsTr("Speed-up speed")
                    ToolTip.visible: hovered && !root.menuOpen
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Speed-up speed")
                    onClicked: speedMenu.opened ? speedMenu.close() : speedMenu.openFor(speedValueButton, "below", true)
                }
            }
            UzButton {
                id: volumeButton
                objectName: "volumeButton"
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: root.level < 3 ? qsTr("Sound") : ""
                value: root.soundValue
                chevron: true
                on: soundPopover.opened
                Accessible.name: qsTr("Sound") + " " + root.soundValue
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: root.audio.muted ? qsTr("Sound · muted") : qsTr("Sound · %1 %").arg(root.audio.volume)
                onClicked: soundPopover.opened ? soundPopover.close() : soundPopover.openFor(volumeButton, "below", "end")
            }
            UzButton {
                id: layoutButton
                objectName: "layoutSwitch"
                visible: root.layouts.length > 1 && root.level < 2
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: qsTr("Screens")
                value: root.level < 1 ? root.screensValue : ""
                chevron: true
                on: layoutMenu.opened
                Accessible.name: qsTr("Screens") + " " + root.screensValue
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: (root.multi ? qsTr("Screen layout · all tiles") : qsTr("Screen layout · this game only")) + " · " + root.screensValue
                onClicked: layoutMenu.opened ? layoutMenu.close() : layoutMenu.openFor(layoutButton, "below", true)
            }
            UzButton {
                id: viewButton
                objectName: "tabMultiview"
                visible: !root.multi && root.own && root.level < 3
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
            Rectangle {
                id: barDivider
                objectName: "headerDivider"
                visible: panelButton.visible
                Layout.preferredWidth: 1
                Layout.preferredHeight: 16
                Layout.leftMargin: Theme.space05
                Layout.rightMargin: Theme.space05
                color: Theme.rule
            }
            UzButton {
                id: panelButton
                objectName: "panelToggle"
                visible: root.level < 3 || root.watchingOnly
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: qsTr("Panel")
                value: root.level < 1 ? root.panelValue : ""
                inverted: root.panelShown
                Accessible.name: (root.panelShown ? qsTr("Hide panel") : qsTr("Show panel"))
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: (root.panelShown ? qsTr("Hide panel") : qsTr("Show panel")) + " · " + root.panelValue
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
            UzButton {
                id: fullscreenButton
                objectName: "fullscreenButton"
                visible: root.level < 2
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: "⛶ " + qsTr("Fullscreen")
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
                text: root.pauseLabel
                value: root.level < 1 ? root.pauseHint : ""
                kind: "primary"
                enabled: root.session.state === GameSession.Running || root.session.state === GameSession.Paused
                Accessible.name: root.session.paused ? qsTr("Resume") : qsTr("Pause")
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: (root.session.paused ? qsTr("Resume") : qsTr("Pause")) + (root.multi ? qsTr(" your game") : "") + " · Esc"
                onClicked: root.session.togglePause()
            }
            UzIconButton {
                id: moreButton
                objectName: "moreButton"
                outlined: true
                focusPolicy: Qt.TabFocus
                glyph: "more"
                on: moreMenu.opened
                Accessible.name: qsTr("More")
                ToolTip.visible: hovered && !root.menuOpen
                ToolTip.delay: 500
                ToolTip.text: qsTr("More")
                onClicked: moreMenu.opened ? moreMenu.close() : moreMenu.openFor(moreButton, "below", true)
            }
        }
    }
}
