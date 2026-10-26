// Design system blocks (docs/design/uzume/design-system.md) in isolation: every Uz* block in Sumi and Kinari, token facts
// (radius 0, Yamabuki, the two grounds, IBM Plex loaded), the focus ring, keyboard behaviour of toggle, checkbox, slider, tabs
// and menu, the dialog's busy state, popover closing, the mark and lockup. Offscreen, no server, no core.
#include <QFontDatabase>
#include <QFontInfo>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QtTest>
#include <functional>

#include "testsupport.h"

namespace {
const char* kPageQml = R"(
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

Item {
    id: page
    width: 960
    height: 1500

    function setDark(d) { Theme.dark = d }
    function setTextSize(scale, tv) { Theme.textScale = scale; Theme.tv = tv }
    function setReduceMotion(on) { Theme.reduceMotion = on }
    readonly property color accentTok: Theme.accent
    readonly property color groundTok: Theme.ground
    readonly property color surfaceTok: Theme.surface
    readonly property color dangerTok: Theme.danger
    readonly property color inkOnAccentTok: Theme.inkOnAccent
    readonly property int radiusSum: Theme.radiusNone
    readonly property string monoFamily: Theme.mono
    readonly property string sansFamily: Theme.sans
    readonly property bool plexLoaded: Theme.fontsLoaded
    readonly property color person1: Theme.playerColor(1, true)
    readonly property int playerCount: Theme.playerColors.length
    readonly property color warnTone: Theme.toneFg("warning", true)
    readonly property color bgWarn: Theme.selected
    readonly property color ruleControlTok: Theme.ruleControl
    readonly property color badgeTok: Theme.badge
    readonly property color markerNavTok: Theme.markerNav
    readonly property color dimTok: Theme.dim
    readonly property color selectedTok: Theme.selected
    readonly property int targetMinTok: Theme.targetMin
    readonly property int targetTvTok: Theme.targetTv
    readonly property int columnsCompactMaxTok: Theme.columnsCompactMax
    readonly property int bodySizeTok: Theme.roles.body.size
    readonly property int bodyLineTok: Theme.roles.body.line
    readonly property int row32Tok: Theme.row32
    readonly property int row44Tok: Theme.row44
    readonly property int row48Tok: Theme.row48
    readonly property int durFastTok: Theme.durFast
    readonly property int durPageTok: Theme.durPage
    readonly property int durListTok: Theme.durList
    property int tabsPicked: 0
    property string lastTab: ""

    ColumnLayout {
        x: 20; y: 20; width: 900; spacing: 12

        RowLayout {
            spacing: 8
            UzButton { objectName: "btnPrimary"; kind: "primary"; text: "Primary" }
            UzButton { objectName: "btnSecondary"; text: "Secondary" }
            UzButton { objectName: "btnGhost"; kind: "ghost"; text: "Ghost" }
            UzButton { objectName: "btnDanger"; kind: "danger"; text: "Delete" }
            UzButton { objectName: "btnSmall"; size: "sm"; text: "Small" }
            UzButton { objectName: "btnOff"; text: "Disabled"; enabled: false }
            UzButton { objectName: "btnBusy"; kind: "primary"; text: "Saving"; busy: true }
            UzIconButton { objectName: "iconBtn"; glyph: "bell"; count: 12 }
            UzChip { objectName: "chip"; text: "All"; count: 10; active: true }
            UzFilterChip { objectName: "filterChip"; text: "Saved" }
            UzSystemChip { objectName: "sysChip"; system: "NDS" }
        }
        RowLayout {
            spacing: 12
            UzField { objectName: "field"; Layout.preferredWidth: 220; placeholderText: "server.example.com" }
            UzField { objectName: "fieldBad"; Layout.preferredWidth: 220; invalid: true; text: "oops" }
            UzSelect { objectName: "select"; model: [{ value: "a", label: "Alpha" }, { value: "b", label: "Beta" }]; current: "a" }
            UzSegmented { objectName: "segmented"; options: [{ value: "x", label: "Server" }, { value: "y", label: "This PC" }]; current: "x" }
            UzToggle { objectName: "toggle"; text: "Sound"; showState: true }
            UzCheckbox { objectName: "checkbox"; text: "Remember me"; hint: "On this PC only" }
            UzCheckbox { objectName: "checkboxMixed"; text: "Some"; tristate: true; checkState: Qt.PartiallyChecked }
        }
        RowLayout {
            spacing: 12
            UzTag { objectName: "tagNeutral"; text: "Seen" }
            UzTag { objectName: "tagAccent"; tone: "accent"; variant: "plane"; text: "Audio" }
            UzTag { objectName: "tagOk"; tone: "ok"; text: "Matches" }
            UzTag { objectName: "tagDanger"; tone: "danger"; variant: "plane"; text: "Missing" }
            UzTag { objectName: "tagWarn"; tone: "warn"; text: "Unverified" }
            UzKbd { objectName: "kbd"; keys: "Ctrl+Shift+U"; label: "Switch user" }
            UzGamepadHint { objectName: "padA"; k: "A"; label: "Choose" }
            UzGamepadHint { objectName: "padDpad"; k: "UpDown" }
            UzGamepadHint { objectName: "padL2"; k: "L2"; pressed: true }
            UzStatusIcon { objectName: "statusWarn"; kind: "warning"; text: "Saves are older than the server" }
            UzStatusIcon { objectName: "statusErr"; kind: "error"; text: "Cannot reach the server" }
            UzStatusIcon { objectName: "statusDone"; kind: "done" }
            UzStatusLine { objectName: "statusLine"; address: "server.example.com:8443" }
        }
        RowLayout {
            spacing: 12
            UzLockup { objectName: "lockupNav" }
            UzLockup { objectName: "lockupDisplay"; variant: "display"; jp: true }
            UzMark { objectName: "markBig"; size: 96 }
            UzMark { objectName: "markMono"; size: 24; mono: true }
            UzAvatar { objectName: "avatar28"; name: "Max"; person: 1; size: 28; status: "online" }
            UzAvatar { objectName: "avatar40"; name: "Lena"; person: 2; size: 40; status: "offline"; voice: "speaking" }
            UzAvatar { objectName: "avatar20"; name: "Jonas"; person: 3; size: 20; voice: "mutedByHost" }
            UzAvatarStack { objectName: "avatarStack"; people: [{ name: "Max" }, { name: "Lena" }, { name: "Jonas" }]; label: "3 watching" }
            UzVoiceMark { objectName: "voiceSpeaking"; voice: "speaking" }
            UzVoiceMark { objectName: "voiceOff"; voice: "micOff" }
            UzVoiceMark { objectName: "voiceHost"; voice: "mutedByHost" }
        }
        UzSlider { objectName: "slider"; Layout.preferredWidth: 420; label: "Volume"; value: 20 }
        UzSlider { objectName: "sliderMuted"; Layout.preferredWidth: 420; value: 40; muted: true }
        UzProgress { objectName: "progress"; Layout.preferredWidth: 300; value: 0.5; label: "Uploading"; hint: "50 %" }
        UzProgress { objectName: "progressThin"; Layout.preferredWidth: 300; value: 0.25; thin: true }
        UzTabs { objectName: "tabs"; items: ["Run", "Chat", "Saves"]; active: "Run"
                 onPicked: label => { page.lastTab = label; page.tabsPicked++ } }
        UzTabs { objectName: "tabsLg"; items: ["General", "Plugins"]; active: "General"; size: "lg" }
        UzSectionLabel { objectName: "sectionLabel"; number: 3; text: "Library"; meta: "12 games"; Layout.preferredWidth: 300 }
        UzPageHeader { objectName: "pageHeader"; Layout.preferredWidth: 600; number: 1; label: "Library"; title: "Home server"
                       UzButton { text: "Add games"; kind: "primary" } }
        UzHintRow { objectName: "hintRowKbd"; Layout.preferredWidth: 500; device: "keyboard"
                    hints: [{ pad: "A", kbd: "Enter", label: "Choose" }, { pad: "B", kbd: "Esc", label: "Back" }] }
        UzHintRow { objectName: "hintRowPad"; Layout.preferredWidth: 500; device: "gamepad"
                    hints: [{ pad: "A", kbd: "Enter", label: "Choose" }, { pad: "B", kbd: "Esc", label: "Back" }] }
        RowLayout {
            spacing: 12
            UzOpenColumn { objectName: "openColumn"; Layout.preferredHeight: 200; label: "Uzume · Library · Home server"; lines: ["12 games"] }
            UzHelpColumn { objectName: "helpColumn"; Layout.preferredHeight: 420; label: "Uzume · Status"
                           items: [{ title: "Backups", text: "Run a backup before an update." }, { text: "Saves are kept 30 days." }] }
            Item { Layout.preferredWidth: 300; Layout.preferredHeight: 200
                   UzRegisterMarks { objectName: "marks"; inset: 8 } }
            UzStateBlock { objectName: "stateEmpty"; Layout.preferredWidth: 320; title: "No games yet"; text: "Add a folder."; action: "Add games" }
            UzStateBlock { objectName: "stateError"; Layout.preferredWidth: 320; kind: "error"; title: "Cannot load"; text: "Try again."; action: "Retry" }
            UzEnso { objectName: "enso" }
        }
        UzMenuList { objectName: "menuList"; selectable: false
                     items: [{ heading: "Server" }, { label: "Switch server" }, { label: "Share", hint: "Invite code", shortcut: "Ctrl+I" },
                             { separator: true }, { label: "Remove", danger: true }, { label: "Locked", disabled: true }] }
        UzToast { objectName: "toast"; kind: "warning"; text: "Home server needs an update to work with this version."; action: "Undo"; shortcut: "F5" }
        UzStageDiagnostics { objectName: "stageDiag"; rows: [["FPS", "60.0"], ["Latency", "42 ms"], ["Audio", "underrun", "warn"], ["Frame", "Buffer 259 ms · 69 underruns · 4 dropped · 48000 Hz stereo · long tail"]] }
        UzHotkeySheet { objectName: "hotkeys" }
    }

    UzDialog {
        id: dlg
        objectName: "dialog"
        label: "Saves"
        title: "Restore this save?"
        text: "The current save stays on the server."
        UzField { objectName: "dialogField"; Layout.fillWidth: true }
        actions: [UzButton { objectName: "dlgPrimary"; kind: "primary"; text: "Restore" }, UzButton { text: "Cancel" }]
        dangerActions: [UzButton { kind: "danger"; text: "Delete" }]
    }
    UzDialog { id: dlgLg; objectName: "dialogLg"; size: "lg"; title: "Wide" }
    UzPopover {
        id: pop
        objectName: "popover"
        label: "Audio"
        title: "Volume"
        UzSlider { Layout.fillWidth: true; value: 50 }
        actions: [UzButton { text: "Mute"; size: "sm" }]
    }
    UzMenu { id: menu; objectName: "menu"; items: [{ label: "One" }, { label: "Two", danger: true }] }
    UzButton { id: anchorBtn; objectName: "anchorBtn"; x: 40; y: 1400; text: "Anchor" }
    function openDialog() { dlg.open() }
    function openDialogLg() { dlgLg.open() }
    function openPopover() { pop.openFor(anchorBtn, "above", "start") }
    function openMenu() { menu.openFor(anchorBtn, "above", false) }
}
)";
}  // namespace

class DesignSystemTest : public QObject {
  Q_OBJECT

  QQmlEngine* engine_ = nullptr;
  QQuickWindow* win_ = nullptr;
  QQuickItem* page_ = nullptr;

  QQuickItem* byName(const char* name) const { return page_->findChild<QQuickItem*>(QString::fromLatin1(name)); }
  QObject* objByName(const char* name) const { return page_->findChild<QObject*>(QString::fromLatin1(name)); }
  QColor colorOf(const char* prop) const { return page_->property(prop).value<QColor>(); }
  void setDark(bool dark) {
    QVERIFY(QMetaObject::invokeMethod(page_, "setDark", Q_ARG(QVariant, dark)));
    QQuickTest::qWaitForPolish(win_);
  }
  void focus(QQuickItem* it) {
    it->forceActiveFocus(Qt::TabFocusReason);
    QTest::qWait(10);
  }
  void key(Qt::Key k, Qt::KeyboardModifiers m = Qt::NoModifier) {
    QTest::keyClick(win_, k, m);
    QTest::qWait(10);
  }
  void call(const char* fn) { QVERIFY(QMetaObject::invokeMethod(page_, fn)); }

 private slots:
  void initTestCase() {
    uitest::installWarningCounter();
    uitest::warningCount() = 0;
    engine_ = new QQmlEngine(this);
    engine_->addImportPath(QStringLiteral("qrc:/qt/qml"));
    win_ = new QQuickWindow;
    win_->resize(1000, 1500);
    QQmlComponent c(engine_);
    c.setData(kPageQml, QUrl(QStringLiteral("qrc:/test/page.qml")));
    QObject* o = c.create();
    QVERIFY2(o != nullptr, qPrintable(c.errorString()));
    page_ = qobject_cast<QQuickItem*>(o);
    QVERIFY(page_ != nullptr);
    page_->setParentItem(win_->contentItem());
    win_->show();
    QVERIFY(QTest::qWaitForWindowExposed(win_));
    QQuickTest::qWaitForPolish(win_);
  }
  void cleanupTestCase() {
    delete win_;
    QCOMPARE(uitest::warningCount().load(), 0);
  }

  // IBM Plex Sans (four weights) and Plex Mono are bundled and loaded; Plex Sans is the application font.
  void plexFontsAreLoaded() {
    QVERIFY(uzume::ui::loadDesignFonts());
    const QStringList families = QFontDatabase::families();
    QVERIFY(families.contains(QStringLiteral("IBM Plex Sans")));
    QVERIFY(families.contains(QStringLiteral("IBM Plex Mono")));
    const QStringList styles = QFontDatabase::styles(QStringLiteral("IBM Plex Sans"));
    for (const char* s : {"Light", "Medium", "SemiBold"}) {
      QVERIFY2(styles.contains(QString::fromLatin1(s)), qPrintable(styles.join(QLatin1Char(','))));
    }
    QCOMPARE(QGuiApplication::font().family(), QStringLiteral("IBM Plex Sans"));
    QCOMPARE(page_->property("monoFamily").toString(), QStringLiteral("IBM Plex Mono"));
    QCOMPARE(page_->property("sansFamily").toString(), QStringLiteral("IBM Plex Sans"));
    QVERIFY(page_->property("plexLoaded").toBool());
    // A medium weight resolves to the Medium face of the family, not the regular one.
    QFont f(QStringLiteral("IBM Plex Sans"));
    f.setWeight(QFont::Medium);
    QCOMPARE(QFontInfo(f).weight(), int(QFont::Medium));
  }

  // Tokens: one yellow, radius 0 everywhere, Sumi / Kinari grounds, four designed person colours plus four derived ones.
  void tokensInBothThemes() {
    for (const bool dark : {true, false}) {
      setDark(dark);
      QCOMPARE(colorOf("accentTok"), QColor(QStringLiteral("#f8b500")));
      QCOMPARE(colorOf("groundTok"), QColor(dark ? QStringLiteral("#1c1b19") : QStringLiteral("#fbfaf5")));
      QCOMPARE(colorOf("surfaceTok"), QColor(dark ? QStringLiteral("#242320") : QStringLiteral("#ffffff")));
      QCOMPARE(colorOf("dangerTok"), QColor(dark ? QStringLiteral("#ee836f") : QStringLiteral("#ac3a41")));
      // r6 tokens.
      QCOMPARE(colorOf("ruleControlTok"), QColor(dark ? QStringLiteral("#7a766d") : QStringLiteral("#86817a")));
      QCOMPARE(colorOf("badgeTok"), QColor(dark ? QStringLiteral("#efece4") : QStringLiteral("#1c1b19")));
      QCOMPARE(colorOf("markerNavTok"), QColor(dark ? QStringLiteral("#f8b500") : QStringLiteral("#715c1f")));
      QCOMPARE(colorOf("dimTok"), QColor(dark ? QStringLiteral("#96928a") : QStringLiteral("#6a665f")));
      QCOMPARE(colorOf("selectedTok"), QColor(dark ? QStringLiteral("#2a2925") : QStringLiteral("#ece9de")));
      QCOMPARE(page_->property("targetMinTok").toInt(), 32);
      QCOMPARE(page_->property("targetTvTok").toInt(), 44);
      QCOMPARE(page_->property("columnsCompactMaxTok").toInt(), 1440);
      QCOMPARE(colorOf("inkOnAccentTok"), QColor(QStringLiteral("#1c1b19")));
      QCOMPARE(page_->property("radiusSum").toInt(), 0);
      QCOMPARE(page_->property("playerCount").toInt(), 8);
      QCOMPARE(colorOf("person1"), QColor(QStringLiteral("#89c3eb")));
      // r6: the plugin "warning" tone is plain ink-2 text (the yellow is a square, never text).
      QCOMPARE(colorOf("warnTone"), QColor(QStringLiteral("#d3cfc5")));
    }
    setDark(true);
  }

  // Text size 100 % / 125 % / TV and Reduce motion are one scale factor and one motion switch in Theme.
  void textSizeAndReduceMotion() {
    auto set = [&](double scale, bool tv) {
      QVERIFY(QMetaObject::invokeMethod(page_, "setTextSize", Q_ARG(QVariant, scale), Q_ARG(QVariant, tv)));
      QQuickTest::qWaitForPolish(win_);
    };
    const qreal btnH = byName("btnPrimary")->height();
    QCOMPARE(page_->property("bodySizeTok").toInt(), 14);  // 100 %: unchanged
    QCOMPARE(page_->property("row32Tok").toInt(), 32);
    QCOMPARE(page_->property("row44Tok").toInt(), 44);
    set(1.25, false);
    QCOMPARE(page_->property("bodySizeTok").toInt(), 17);
    QCOMPARE(page_->property("bodyLineTok").toInt(), 27);
    QCOMPARE(page_->property("row32Tok").toInt(), 32);  // rows stay at 125 %
    set(1.25, true);
    QCOMPARE(page_->property("bodySizeTok").toInt(), 17);
    QCOMPARE(page_->property("row32Tok").toInt(), 44);
    QCOMPARE(page_->property("row44Tok").toInt(), 48);
    QCOMPARE(page_->property("row48Tok").toInt(), 52);
    QVERIFY(byName("btnPrimary")->height() >= 44);
    set(1.0, false);
    QCOMPARE(page_->property("bodySizeTok").toInt(), 14);
    QCOMPARE(byName("btnPrimary")->height(), btnH);

    QCOMPARE(page_->property("durFastTok").toInt(), 120);
    QVERIFY(QMetaObject::invokeMethod(page_, "setReduceMotion", Q_ARG(QVariant, true)));
    QCOMPARE(page_->property("durFastTok").toInt(), 0);
    QCOMPARE(page_->property("durPageTok").toInt(), 0);
    QCOMPARE(page_->property("durListTok").toInt(), 0);
    QVERIFY(QMetaObject::invokeMethod(page_, "setReduceMotion", Q_ARG(QVariant, false)));
    QCOMPARE(page_->property("durFastTok").toInt(), 120);
  }

  // Every block exists and has the measures of the spec; geometry is the same in both themes.
  void blocksAndMeasures() {
    for (const bool dark : {true, false}) {
      setDark(dark);
      QCOMPARE(byName("btnPrimary")->height(), 36.0);
      QCOMPARE(byName("btnSmall")->height(), 32.0);
      QCOMPARE(byName("field")->height(), 36.0);
      QCOMPARE(byName("select")->height(), 36.0);
      QCOMPARE(byName("chip")->height(), 28.0);
      QCOMPARE(byName("iconBtn")->width(), 32.0);
      QCOMPARE(byName("toast")->height(), 44.0);
      QCOMPARE(byName("hintRowKbd")->height(), 44.0);
      QCOMPARE(byName("openColumn")->width(), 152.0);
      QCOMPARE(byName("helpColumn")->width(), 152.0);
      QCOMPARE(byName("progressThin")->findChild<QQuickItem*>("progressTrack")->height(), 2.0);
      QCOMPARE(byName("progress")->findChild<QQuickItem*>("progressTrack")->height(), 4.0);
      QCOMPARE(byName("progress")->findChild<QQuickItem*>("progressFill")->width(), 150.0);
      QCOMPARE(byName("avatar28")->width(), 28.0);
      QCOMPARE(byName("avatar40")->width(), 40.0);
      QCOMPARE(byName("avatar20")->width(), 20.0);
      QCOMPARE(byName("lockupNav")->findChild<QQuickItem*>("uzMark")->width(), 20.0);
      QCOMPARE(byName("lockupDisplay")->findChild<QQuickItem*>("uzMark")->width(), 52.0);
      QCOMPARE(byName("stageDiag")->width(), 232.0);
      QCOMPARE(byName("hotkeys")->width(), 400.0);
      QVERIFY(byName("menuList")->width() >= 200.0);
      QCOMPARE(byName("statusWarn")->findChild<QQuickItem*>("statusIconBox")->width(), 20.0);
      int marks = 0;
      for (const char* m : {"registerMarkTL", "registerMarkTR", "registerMarkBL", "registerMarkBR"}) {
        QQuickItem* mk = uitest::Harness::findVisual(byName("marks"), QString::fromLatin1(m));
        QVERIFY2(mk != nullptr, m);
        QCOMPARE(mk->width(), 12.0);   // register-mark: 12 px arms
        ++marks;
      }
      QCOMPARE(marks, 4);
    }
    setDark(true);
  }

  // Keyboard focus is always visible: the ring is shown on focus and gone without it.
  void focusRingIsVisibleOnFocus() {
    for (const bool dark : {true, false}) {
      setDark(dark);
      for (const char* n : {"btnPrimary", "field", "select", "toggle", "checkbox", "chip", "segmented", "slider", "tabs", "iconBtn"}) {
        QQuickItem* it = byName(n);
        QVERIFY2(it != nullptr, n);
        QQuickItem* ring = uitest::Harness::findVisual(it, QStringLiteral("focusRing"));
        QVERIFY2(ring != nullptr, n);
        QVERIFY2(!ring->isVisible(), n);
        focus(it);
        QVERIFY2(it->hasActiveFocus(), n);
        QVERIFY2(ring->isVisible(), n);
        QVERIFY2(ring->width() > 0, n);
      }
      byName("btnSecondary")->forceActiveFocus(Qt::TabFocusReason);   // moves the focus away
    }
    setDark(true);
  }

  void toggleKeyboard() {
    QQuickItem* t = byName("toggle");
    QVERIFY(!t->property("checked").toBool());
    focus(t);
    key(Qt::Key_Space);
    QVERIFY(t->property("checked").toBool());
    key(Qt::Key_Space);
    QVERIFY(!t->property("checked").toBool());
  }

  void checkboxKeyboard() {
    QQuickItem* c = byName("checkbox");
    QVERIFY(!c->property("checked").toBool());
    focus(c);
    key(Qt::Key_Space);
    QVERIFY(c->property("checked").toBool());
    key(Qt::Key_Space);
    QVERIFY(!c->property("checked").toBool());
    QCOMPARE(byName("checkboxMixed")->property("checkState").toInt(), int(Qt::PartiallyChecked));
  }

  void sliderKeyboard() {
    QQuickItem* s = byName("slider");
    QCOMPARE(s->property("value").toDouble(), 20.0);
    focus(s);
    key(Qt::Key_Right);
    QCOMPARE(s->property("value").toDouble(), 21.0);
    key(Qt::Key_Left);
    QCOMPARE(s->property("value").toDouble(), 20.0);
    key(Qt::Key_Right, Qt::ShiftModifier);   // a tenth of the range
    QCOMPARE(s->property("value").toDouble(), 30.0);
    key(Qt::Key_PageDown);
    QCOMPARE(s->property("value").toDouble(), 20.0);
    key(Qt::Key_End);
    QCOMPARE(s->property("value").toDouble(), 100.0);
    key(Qt::Key_Home);
    QCOMPARE(s->property("value").toDouble(), 0.0);
    QCOMPARE(s->findChild<QQuickItem*>("sliderValue")->property("text").toString(), QStringLiteral("0 %"));
    // Muted: the value shows OFF and the fill is empty.
    QQuickItem* m = byName("sliderMuted");
    QCOMPARE(m->findChild<QQuickItem*>("sliderValue")->property("text").toString(), QStringLiteral("OFF"));
    QCOMPARE(m->findChild<QQuickItem*>("sliderFill")->width(), 0.0);
  }

  void tabsAndSegmentedKeyboard() {
    QQuickItem* tabs = byName("tabs");
    focus(tabs);
    QSignalSpy spy(tabs, SIGNAL(picked(QString)));
    key(Qt::Key_Right);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("Chat"));
    QCOMPARE(page_->property("lastTab").toString(), QStringLiteral("Chat"));
    // The 2 px underline belongs to the active tab only.
    QVERIFY(tabs->height() == 36.0);
    QQuickItem* seg = byName("segmented");
    focus(seg);
    QSignalSpy segSpy(seg, SIGNAL(picked(QString)));
    key(Qt::Key_Right);
    QCOMPARE(segSpy.count(), 1);
    QCOMPARE(segSpy.at(0).at(0).toString(), QStringLiteral("y"));
  }

  void menuListKeyboard() {
    QQuickItem* list = byName("menuList");
    focus(list);
    QSignalSpy spy(list, SIGNAL(chosen(QVariant, int)));
    key(Qt::Key_Down);   // skips the heading
    QCOMPARE(list->property("activeIndex").toInt(), 1);
    key(Qt::Key_Down);
    QCOMPARE(list->property("activeIndex").toInt(), 2);
    key(Qt::Key_Down);   // skips the separator
    QCOMPARE(list->property("activeIndex").toInt(), 4);
    key(Qt::Key_Down);   // skips the disabled item and wraps to the first enabled one
    QCOMPARE(list->property("activeIndex").toInt(), 1);
    key(Qt::Key_Return);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(1).toInt(), 1);
  }

  void tagTones() {
    QCOMPARE(byName("tagWarn")->property("kind").toString(), QStringLiteral("accent"));   // legacy "warn" is the accent
    QCOMPARE(byName("tagOk")->property("iconKind").toString(), QStringLiteral("check"));
    QCOMPARE(byName("tagDanger")->property("iconKind").toString(), QStringLiteral("cross"));
    QCOMPARE(byName("tagDanger")->property("radius").toInt(), 0);
    QCOMPARE(byName("tagAccent")->property("color").value<QColor>(), QColor(QStringLiteral("#f8b500")));
  }

  void avatarsUsePersonColours() {
    setDark(false);
    QQuickItem* face = byName("avatar28")->findChild<QQuickItem*>("avatarFace");
    QCOMPARE(face->property("color").value<QColor>(), QColor(QStringLiteral("#89c3eb")));
    QCOMPARE(face->property("radius").toInt(), 0);
    QVERIFY(byName("avatar28")->findChild<QQuickItem*>("avatarStatus")->isVisible());
    QVERIFY(byName("avatar40")->findChild<QQuickItem*>("avatarVoice")->isVisible());
    QVERIFY(!byName("avatar28")->findChild<QQuickItem*>("avatarVoice")->isVisible());
    setDark(true);
  }

  void markAndLockup() {
    QQuickItem* mark = byName("markBig");
    QCOMPARE(mark->width(), 96.0);
    QCOMPARE(mark->height(), 96.0);
    QQuickItem* word = byName("lockupNav")->findChild<QQuickItem*>("uzWordmark");
    QCOMPARE(word->property("text").toString(), QStringLiteral("Uzume"));
    QCOMPARE(word->property("font").value<QFont>().pixelSize(), 17);
    QCOMPARE(byName("lockupNav")->property("spacing").toInt(), 10);
    QCOMPARE(byName("lockupDisplay")->property("spacing").toInt(), 16);
    QCOMPARE(byName("lockupDisplay")->findChild<QQuickItem*>("uzWordmark")->property("font").value<QFont>().pixelSize(), 46);
  }

  void hintRowFollowsDevice() {
    QQuickItem* kbd = byName("hintRowKbd")->findChild<QQuickItem*>("hintRowKeys");
    QQuickItem* pad = byName("hintRowPad")->findChild<QQuickItem*>("hintRowKeys");
    QVERIFY(uitest::Harness::findVisual(kbd, "gamepadKey") == nullptr);   // keyboard: Kbd keys only
    QVERIFY(uitest::Harness::findVisual(pad, "gamepadKey") != nullptr);    // gamepad: GamepadHint glyphs
  }

  // Dialog: 400 / 560 px, busy dims the content, blocks input and cannot be dismissed; otherwise Esc closes it.
  void dialogBusyState() {
    QObject* dlg = objByName("dialog");
    QVERIFY(dlg != nullptr);
    QVERIFY(!dlg->property("visible").toBool());
    QCOMPARE(dlg->findChild<QQuickItem*>("dialogFrame")->width(), 400.0);
    call("openDialogLg");
    QTRY_VERIFY(objByName("dialogLg")->property("visible").toBool());
    QCOMPARE(objByName("dialogLg")->findChild<QQuickItem*>("dialogFrame")->width(), 560.0);
    QVERIFY(QMetaObject::invokeMethod(objByName("dialogLg"), "close"));
    QTRY_VERIFY(!objByName("dialogLg")->property("visible").toBool());
    call("openDialog");
    QTRY_VERIFY(dlg->property("visible").toBool());
    QQuickItem* content = dlg->findChild<QQuickItem*>("dialogContent");
    QVERIFY(content != nullptr);
    QVERIFY(content->isEnabled());
    QCOMPARE(content->opacity(), 1.0);
    QCOMPARE(dlg->findChild<QQuickItem*>("dialogTitle")->property("text").toString(), QStringLiteral("Restore this save?"));
    dlg->setProperty("busy", true);
    QVERIFY(!content->isEnabled());
    QCOMPARE(content->opacity(), 0.6);
    key(Qt::Key_Escape);
    QTest::qWait(60);
    QVERIFY(dlg->property("visible").toBool());   // busy: Esc does not dismiss
    dlg->setProperty("busy", false);
    QVERIFY(content->isEnabled());
    key(Qt::Key_Escape);
    QTRY_VERIFY(!dlg->property("visible").toBool());
  }

  // Popover: opens at its anchor, no pointer, closes on Esc and reports it.
  void popoverClosesOnEscape() {
    QObject* pop = objByName("popover");
    QVERIFY(pop != nullptr);
    QSignalSpy spy(pop, SIGNAL(closeRequested()));
    call("openPopover");
    QTRY_VERIFY(pop->property("visible").toBool());
    QCOMPARE(pop->property("width").toInt(), 280);
    QCOMPARE(pop->findChild<QQuickItem*>("popoverTitle")->property("text").toString(), QStringLiteral("Volume"));
    key(Qt::Key_Escape);
    QTRY_VERIFY(!pop->property("visible").toBool());
    QCOMPARE(spy.count(), 1);
  }

  void menuChoosesAndCloses() {
    QObject* menu = objByName("menu");
    QVERIFY(menu != nullptr);
    QSignalSpy spy(menu, SIGNAL(chosen(QVariant, int)));
    call("openMenu");
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(menu->property("width").toInt() >= 200);
    key(Qt::Key_Down);
    key(Qt::Key_Down);
    key(Qt::Key_Return);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(1).toInt(), 1);
    QTRY_VERIFY(!menu->property("visible").toBool());
  }

  void toastActionAndClose() {
    QQuickItem* toast = byName("toast");
    QSignalSpy act(toast, SIGNAL(actionTriggered()));
    QSignalSpy cls(toast, SIGNAL(closeRequested()));
    focus(toast->findChild<QQuickItem*>("toastAction"));
    key(Qt::Key_Return);
    QCOMPARE(act.count(), 1);
    focus(toast->findChild<QQuickItem*>("toastClose"));
    key(Qt::Key_Space);
    QCOMPARE(cls.count(), 1);
  }

  // Long values wrap onto further lines instead of ending in "…".
  void stageDiagnosticsValuesWrap() {
    QQuickItem* diag = byName("stageDiag");
    QQuickItem* value = nullptr;
    const QString want = QStringLiteral("Buffer 259 ms · 69 underruns · 4 dropped · 48000 Hz stereo · long tail");
    std::function<void(QQuickItem*)> walk = [&](QQuickItem* it) {   // delegates of a Repeater are visual children only
      if (it->property("text").toString() == want) value = it;
      for (QQuickItem* c : it->childItems()) walk(c);
    };
    walk(diag);
    QVERIFY(value != nullptr);
    QVERIFY(!value->property("truncated").toBool());
    QVERIFY2(value->property("lineCount").toInt() > 1, "the long value wraps");
    QVERIFY(value->mapToItem(diag, QPointF(value->width(), 0)).x() <= diag->width());
  }

  void stageDiagnosticsCopy() {
    QQuickItem* diag = byName("stageDiag");
    QSignalSpy spy(diag, SIGNAL(copyRequested()));
    focus(diag->findChild<QQuickItem*>("diagCopy"));
    key(Qt::Key_Return);
    QCOMPARE(spy.count(), 1);
    QVERIFY(diag->property("copied").toBool());
  }
};

UITEST_MAIN(DesignSystemTest)
#include "design_system_ui_test.moc"
