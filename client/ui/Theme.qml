pragma Singleton
import QtQuick

// Design tokens of the Uzume design system (docs/design/uzume/design-system.md, source of truth
// docs/design/source/uzume/ds/uzume/tokens.json): direction Ma, themes Sumi (dark) and Kinari (light), IBM Plex.
// Screens use the token names ("Design system tokens", typography roles, spacing, sizes); the screen layout constants and
// motion durations are in "Layout and motion". There are no legacy aliases left.
QtObject {
    id: root

    property bool dark: true

    // ---- Text size and motion (Settings > This PC > Appearance; spec readable-pass.md, tokens `scale`) -------------------
    // textScale 1 / 1.25; `tv` is the TV text size (scale 1.25 plus rows 44 / 48 to 52 and every focus target >= targetTv).
    // Set by Main.qml from the app setting. Standard (1) is pixel-identical to the unscaled design.
    property real textScale: 1.0
    property bool tv: false
    // Reduce motion: the dur* durations become 0 (set by Main.qml; follows the system setting until the user changes it).
    property bool reduceMotion: false

    // ---- Design system tokens --------------------------------------------------------------------------------------
    // Both palettes as tables (tokens.json). The game view stays Sumi in both themes and the plugin API passes `dark`
    // explicitly, so the tables are also read through tok().
    readonly property var sumi: ({
        ground: "#1c1b19", surface: "#242320", scrim: "#cc1c1b19", stage: "#171614", placeholder: "#262521", placeholderRule: "#34322e",
        ink: "#efece4", ink2: "#d3cfc5", muted: "#a19d94", dim: "#96928a", rule: "#3a3833", ruleControl: "#7a766d", badge: "#efece4", rule2: "#2b2a27",
        hover: "#242320", pressed: "#2c2b28", selected: "#2a2925",
        disabledGround: "#262521", disabledInk: "#6b6861", disabledRule: "#2b2a27",
        accent: "#f8b500", accentHover: "#ffc42e", accentPressed: "#dba000", accentText: "#f8b500", onAccent: "#1c1b19",
        danger: "#ee836f", dangerTint: "#1aee836f", ok: "#68be8d"
    })
    readonly property var kinari: ({
        ground: "#fbfaf5", surface: "#ffffff", scrim: "#ccfbfaf5", stage: "#f3f1e9", placeholder: "#e9e6dc", placeholderRule: "#d6d2c6",
        ink: "#1c1b19", ink2: "#3a3936", muted: "#5f5c55", dim: "#6a665f", rule: "#d6d2c6", ruleControl: "#86817a", badge: "#1c1b19", rule2: "#e9e6dd",
        hover: "#f4f2ea", pressed: "#ebe8de", selected: "#ece9de",
        disabledGround: "#efede6", disabledInk: "#a3a097", disabledRule: "#e9e6dd",
        accent: "#f8b500", accentHover: "#ffc42e", accentPressed: "#e0a400", accentText: "#715c1f", onAccent: "#1c1b19",
        danger: "#ac3a41", dangerTint: "#12b94047", ok: "#006b3a"
    })
    // CSS string for Canvas styles: Qt 6.8 on Windows ignores a QML color value for fillStyle/strokeStyle.
    function cssColor(c: color): string {
        return "rgba(" + Math.round(c.r * 255) + "," + Math.round(c.g * 255) + "," + Math.round(c.b * 255) + "," + c.a + ")"
    }
    function withAlpha(c: color, a: real): color { return Qt.rgba(c.r, c.g, c.b, a) }
    function tok(name: string, isDark: bool): color { return (isDark ? sumi : kinari)[name] }

    // Planes
    readonly property color ground: dark ? sumi.ground : kinari.ground
    readonly property color surface: dark ? sumi.surface : kinari.surface
    readonly property color scrim: dark ? sumi.scrim : kinari.scrim
    readonly property color stage: dark ? sumi.stage : kinari.stage
    readonly property color placeholder: dark ? sumi.placeholder : kinari.placeholder
    readonly property color placeholderRule: dark ? sumi.placeholderRule : kinari.placeholderRule
    // Text and lines
    readonly property color ink: dark ? sumi.ink : kinari.ink
    readonly property color ink2: dark ? sumi.ink2 : kinari.ink2
    readonly property color muted: dark ? sumi.muted : kinari.muted
    readonly property color dim: dark ? sumi.dim : kinari.dim
    readonly property color rule: dark ? sumi.rule : kinari.rule
    readonly property color ruleControl: dark ? sumi.ruleControl : kinari.ruleControl   // border of everything operable (>= 3:1)
    readonly property color badge: dark ? sumi.badge : kinari.badge                     // count badge fill (text: ground)
    readonly property color rule2: dark ? sumi.rule2 : kinari.rule2
    // States
    readonly property color hover: dark ? sumi.hover : kinari.hover
    readonly property color pressed: dark ? sumi.pressed : kinari.pressed
    readonly property color selected: dark ? sumi.selected : kinari.selected
    readonly property color disabledGround: dark ? sumi.disabledGround : kinari.disabledGround
    readonly property color disabledInk: dark ? sumi.disabledInk : kinari.disabledInk
    readonly property color disabledRule: dark ? sumi.disabledRule : kinari.disabledRule
    // Accent (Yamabuki)
    readonly property color accent: sumi.accent   // same in both themes
    readonly property color accentHover: sumi.accentHover
    readonly property color accentPressed: dark ? sumi.accentPressed : kinari.accentPressed
    readonly property color accentText: dark ? sumi.accentText : kinari.accentText
    readonly property color inkOnAccent: sumi.onAccent
    // Active nav marker (token marker-nav): Yamabuki on Sumi, accent-text on Kinari. marker-under (tabs) is the same colour.
    readonly property color markerNav: dark ? sumi.accent : kinari.accentText
    // Status
    readonly property color danger: dark ? sumi.danger : kinari.danger
    readonly property color dangerTint: dark ? sumi.dangerTint : kinari.dangerTint
    readonly property color ok: dark ? sumi.ok : kinari.ok
    // People: the four designed colours (join order Max, Lena, Jonas, Sam) are the same in both themes.
    readonly property color personWasurenagusa: "#89c3eb"
    readonly property color personMomo: "#f09199"
    readonly property color personMoegi: "#aacf53"
    readonly property color personFuji: "#bbbcde"
    readonly property color onPerson: "#1b1b1b"
    // Entries 5-8 are derived to the same rules (pastel, lightness 70-80 %, not Yamabuki, not the danger red, on-person
    // text >= 7:1): seiji 青磁, usu-fuji 薄藤, peach, nezumi-kinari. Not part of tokens.json.
    readonly property var playerColors: [personWasurenagusa, personMomo, personMoegi, personFuji, "#80d4c0", "#c8a3dd", "#e9ae8c", "#c9c3a6"]
    function playerColor(i: int, isDark: bool): color {
        const n = Math.max(1, Math.min(8, Math.round(i) || 1))
        return playerColors[n - 1]
    }

    // Families. The fonts are bundled (client/ui/fonts, loaded by uzume::ui::loadDesignFonts()); the old system mono font
    // is only the fallback when loading failed.
    readonly property bool fontsLoaded: Qt.fontFamilies().indexOf("IBM Plex Mono") >= 0
    readonly property string sans: "IBM Plex Sans"
    readonly property string mono: fontsLoaded ? "IBM Plex Mono"
                                                : (Qt.platform.os === "windows" ? "Consolas" : (Qt.platform.os === "osx" ? "Menlo" : "monospace"))
    readonly property string jp: "IBM Plex Sans JP"

    // Typography roles (design-system.md "Ma roles", "App sizes", "Brand"). size / line in px, weight as Font.*, spacing in px
    // (the em tracking of the design times the size, QML letterSpacing is absolute), family sans or mono, upper = uppercase.
    // Use: font.pixelSize: Theme.roles.body.size; font.weight: Theme.roles.body.weight; font.letterSpacing: Theme.roles.body.spacing.
    readonly property var roleBase: ({
        title:         { family: sans, size: 40, line: 40, weight: Font.Light,    spacing: -0.8,  upper: false },
        sectionLabel:  { family: mono, size: 11, line: 16, weight: Font.Medium,   spacing: 1.54,   upper: true },
        body:          { family: sans, size: 14, line: 22, weight: Font.Normal,   spacing: 0,     upper: false },
        meta:          { family: mono, size: 11, line: 16, weight: Font.Normal,   spacing: 0.66,  upper: true },
        meta12:        { family: mono, size: 12, line: 16, weight: Font.Normal,   spacing: 0,     upper: false },
        meta11:        { family: mono, size: 12, line: 16, weight: Font.Normal,   spacing: 0,     upper: false },   // old name of meta12
        dialogTitle:   { family: sans, size: 20, line: 28, weight: Font.Normal,   spacing: -0.2,  upper: false },
        headerTitle:   { family: sans, size: 16, line: 24, weight: Font.Medium,   spacing: 0,     upper: false },
        itemTitle:     { family: sans, size: 14, line: 22, weight: Font.Medium,   spacing: 0,     upper: false },
        itemTitleSm:   { family: sans, size: 13, line: 18, weight: Font.Medium,   spacing: 0,     upper: false },
        button:        { family: sans, size: 14, line: 20, weight: Font.Medium,   spacing: 0,     upper: false },
        buttonSm:      { family: sans, size: 13, line: 20, weight: Font.Normal,   spacing: 0,     upper: false },
        input:         { family: sans, size: 13, line: 20, weight: Font.Normal,   spacing: 0,     upper: false },
        fieldLabel:    { family: sans, size: 12, line: 16, weight: Font.Medium,   spacing: 0,     upper: false },
        small:         { family: sans, size: 12, line: 18, weight: Font.Normal,   spacing: 0,     upper: false },
        tableHeader:   { family: mono, size: 11, line: 16, weight: Font.Medium,   spacing: 1.32,   upper: true },
        systemCode:    { family: mono, size: 11, line: 14, weight: Font.Medium,   spacing: 0.66,   upper: true },
        tag:           { family: mono, size: 11, line: 16, weight: Font.DemiBold, spacing: 1.1,   upper: true },
        address:       { family: mono, size: 12, line: 16, weight: Font.Normal,   spacing: 0,     upper: false },
        inviteCode:    { family: mono, size: 28, line: 36, weight: Font.Medium,   spacing: 3.92,  upper: true },
        verticalLabel: { family: mono, size: 10, line: 16, weight: Font.Normal,   spacing: 2.4,   upper: true },
        wordmarkNav:   { family: sans, size: 17, line: 20, weight: Font.DemiBold, spacing: -0.17, upper: false },
        wordmarkDisplay: { family: sans, size: 46, line: 46, weight: Font.DemiBold, spacing: -1.61, upper: false },
        jpAccent:      { family: jp,   size: 15, line: 20, weight: Font.Normal,   spacing: 0,     upper: false }
    })
    // The roles as used by screens: size, line and tracking times textScale (rounded down to whole px).
    readonly property var roles: {
        if (textScale === 1.0) return roleBase
        const out = {}
        for (const k of Object.keys(roleBase)) {
            const r = roleBase[k]
            out[k] = { family: r.family, size: Math.floor(r.size * textScale), line: Math.floor(r.line * textScale),
                       weight: r.weight, spacing: r.spacing * textScale, upper: r.upper }
        }
        return out
    }

    // Spacing (design-system.md "Spacing")
    readonly property int space05: 4
    readonly property int space1: 8
    readonly property int space15: 12
    readonly property int space2: 16
    readonly property int space3: 24
    readonly property int space4: 32
    readonly property int space5: 40
    readonly property int space7: 56

    // Sizes (design-system.md "Sizes")
    // At the TV text size rows and controls grow: 28 / 32 / 36 -> targetTv (44), 44 -> 48, 48 -> 52.
    readonly property int row28: tv ? targetTv : 28
    readonly property int row32: tv ? targetTv : 32
    readonly property int row36: tv ? targetTv : 36
    readonly property int row44: tv ? 48 : 44
    readonly property int row48: tv ? 52 : 48
    readonly property int header56: 56
    readonly property int sidebar: 232
    readonly property int openColumn: 152
    readonly property int navInset: 28       // AppNav: left edge of the lockup, item text and group heads
    readonly property int sidePanel: 320
    readonly property int dialog: 400
    readonly property int dialogLg: 560
    readonly property int registerMark: 12
    readonly property int personMarker: 40
    readonly property int markNav: 20
    readonly property int menu: 200
    readonly property int targetMin: tv ? targetTv : 32      // smallest hit target (icon buttons, the x of a field, row actions)
    readonly property int targetTv: 44       // smallest focus target at the TV text scale
    readonly property int columnsCompactMax: 1440   // below this window width at most three columns including AppNav

    // Radius: none. The app icon tile (22.5 %) is the only rounded shape and is not drawn by QML.
    readonly property int radiusNone: 0

    // Focus ring (token focus-ring / focus-ring-inset): Sumi 2 px gap + 2 px Yamabuki, Kinari 2 px Yamabuki + 1 px Sumi outside.
    // Drawn by UzFocusRing. Widths in px, measured from the element edge.
    readonly property int focusGap: dark ? 2 : 0
    readonly property int focusWidth: 2
    readonly property int focusOuter: dark ? 0 : 1        // extra 1 px ink line outside the ring (Kinari)
    readonly property color focusOuterColor: kinari.ink

    // ---- Layout and motion -----------------------------------------------------------------------------------------
    readonly property int sidebarWidth: sidebar
    readonly property int columnWidth: 300       // second column of 3e / 3f / 3p
    readonly property int contentMaxWidth: 880   // Settings, Runs and plugin screens: left-aligned, never wider
    readonly property int gridMaxWidth: 1680     // Library grid
    readonly property int inputTestWidth: 340    // Controllers input test
    // SettingsRow grid 14 | flex | 280 | 72; Emulation and Settings share the content width, so the controls end on the same x.
    readonly property int settingsDotSlot: 14
    readonly property int settingsControlWidth: 280
    readonly property int settingsResetWidth: 72
    readonly property int settingsControlHeight: row32
    readonly property int settingsNarrowBelow: 640   // below this row width the control moves under the text

    // Motion (short; never in the game view)
    readonly property int durFast: reduceMotion ? 0 : 120
    readonly property int durPage: reduceMotion ? 0 : 160
    readonly property int durList: reduceMotion ? 0 : 180

    // Count with a plural noun, never "(s)": plural(1, "folder", "folders") -> "1 folder", plural(3, ...) -> "3 folders".
    function plural(n: int, one: string, many: string): string {
        return n + " " + (Number(n) === 1 ? one : many)
    }

    function toneColor(tone: string): color {
        switch (tone) {
        case "ok": return ok
        case "warn": return ink2
        case "error": return danger
        case "info": return ink2
        case "amber": return ink2
        default: return muted
        }
    }
    function toneBg(tone: string): color {
        switch (tone) {
        case "ok": return ground
        case "info": return selected
        case "amber": return selected
        case "warn": return selected
        case "error": return dangerTint
        default: return pressed
        }
    }

    // ---- Plugin building blocks (docs/design/legacy/plugins.md "Building blocks"). Plugins choose tones, size names and player
    // numbers only; the mapping to colors and pixels lives here. `dark` is passed explicitly because in-game surfaces stay
    // dark while the rest of the app follows `Theme.dark`. One yellow: the "warning" tone is the accent, "info" is plain ink.
    function toneFg(tone: string, dark: bool): color {
        switch (tone) {
        case "info": return tok("ink2", dark)
        case "ok": return tok("ok", dark)
        case "warning": return tok("ink2", dark)
        case "danger": return tok("danger", dark)
        default: return tok("muted", dark)
        }
    }
    function toneBgFor(tone: string, dark: bool): color {
        switch (tone) {
        case "info": return tok("ground", dark)
        case "ok": return tok("ground", dark)
        case "warning": return tok("selected", dark)
        case "danger": return tok("dangerTint", dark)
        default: return tok("pressed", dark)
        }
    }
    function toneLine(tone: string, dark: bool): color {
        switch (tone) {
        case "ok": return tok("ok", dark)
        case "warning": return tok("accent", dark)
        case "danger": return tok("danger", dark)
        default: return tok("rule", dark)
        }
    }
    // Gap names of the layout blocks: 4, 8, 12, 16, 20; anything else is rounded down to the nearest allowed value.
    function gapPx(g: var): int {
        const allowed = [4, 8, 12, 16, 20]
        let r = 8
        if (g === undefined || g === null) return r
        r = allowed[0]
        for (let i = 0; i < allowed.length; ++i) {
            if (allowed[i] <= g) r = allowed[i]
        }
        return r
    }
    function pluginImageUrl(pluginId: string, src: string): string {
        if (!src) return ""
        const m = /^(cache|asset):(.+)$/.exec(src)
        return m ? "image://uzplugin/" + pluginId + "/" + m[1] + "/" + m[2] : ""
    }
    // Neutral colors of the blocks by role, for the given theme.
    function blockColor(role: string, dark: bool): color {
        switch (role) {
        case "text": return tok("ink", dark)
        case "textSecondary": return tok("ink2", dark)
        case "textMuted": return tok("muted", dark)
        case "textMeta": return tok("muted", dark)
        case "textFaint": return tok("muted", dark)
        case "textDisabled": return tok("disabledInk", dark)
        case "textOnAccent": return tok("onAccent", dark)
        case "accent": return tok("accent", dark)
        case "surface": return tok("ground", dark)
        case "card": return tok("ground", dark)
        case "cardBorder": return tok("rule", dark)
        case "border": return tok("rule", dark)
        case "borderButton": return tok("ruleControl", dark)
        case "hover": return tok("hover", dark)
        case "selection": return tok("selected", dark)
        case "track": return tok("rule2", dark)
        case "popup": return tok("surface", dark)
        case "popupBorder": return tok("rule", dark)
        case "ownerRing": return tok("ground", dark)
        case "missedLine": return tok("rule", dark)
        case "mapWater": return tok("stage", dark)
        case "mapLand": return tok("placeholder", dark)
        case "scrim": return tok("scrim", dark)
        default: return tok("ink", dark)
        }
    }
}
