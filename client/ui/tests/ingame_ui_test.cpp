// In-game view revision (0.7.x; designs 3g-2/3g-3, 3r-2/3h-2/3i-2, 3t-2/3x-2, 3c-5 sidebar part): GameHeader zones in Session
// and Multiview at 1440, 1280 and 960 px, game controls (also in the Multiview), the Reset popover, the single diagnostics
// toggle and the overlay position, the panel following the tile selection, the picker anchored under "+ Add" (never opening
// on its own), the "Now running" strip in the sidebar. Preview game and FakeHub, no core needed.
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickWindow>
#include <QtTest>

#include "audiocontroller.h"
#include "controllerscontroller.h"
#include "fakehub.h"
#include "gameview.h"
#include "savestore.h"
#include "savesync.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;

namespace {

QJsonObject sessionObj(const QString& id, const QString& who, const QString& game) {
  return {{QStringLiteral("session_id"), id},
          {QStringLiteral("game_id"), QStringLiteral("g1")},
          {QStringLiteral("game_title"), game},
          {QStringLiteral("owner"), QJsonObject{{QStringLiteral("user_id"), QStringLiteral("u_") + who.toLower()},
                                                {QStringLiteral("display_name"), who},
                                                {QStringLiteral("device_name"), QStringLiteral("PC")}}},
          {QStringLiteral("visibility"), QStringLiteral("hub_users")},
          {QStringLiteral("created_at"), QDateTime::currentDateTimeUtc().addSecs(-600).toString(Qt::ISODate)},
          {QStringLiteral("viewer_count"), 0},
          {QStringLiteral("is_owner"), false},
          {QStringLiteral("invited"), false}};
}

QQuickItem* visibleItem(Harness& h, const char* name) {
  for (QQuickItem* i : h.items(name)) {
    if (i->isVisible()) return i;
  }
  return nullptr;
}
// The header collapsed to icons because the font is wider than the layout was designed for (window width alone does not decide).
bool shown(Harness& h, const char* name) { return visibleItem(h, name) != nullptr; }

// Picks a Multiview mode through the header: the segment, or (collapsed when the header is tight) its cycle button.
bool clickMode(Harness& h, const char* modeName) {
  QQuickItem* seg = h.item(modeName);
  if (seg != nullptr && seg->isVisible()) return h.click(modeName);
  const QString want = QString::fromLatin1(modeName).mid(4).toLower();
  QQuickItem* cycle = h.item("modeCycle");
  if (cycle == nullptr || !cycle->isVisible()) {   // absurdly wide fonts: the header has no room for the mode control at all
    h.controller->sessions()->setMultiviewMode(want);
    return h.controller->sessions()->multiviewMode() == want;
  }
  for (int i = 0; i < 4 && h.controller->sessions()->multiviewMode() != want; ++i)
    if (!h.click("modeCycle")) return false;
  return h.controller->sessions()->multiviewMode() == want;
}
void collectLabels(QQuickItem* it, QList<QQuickItem*>& out) {
  for (QQuickItem* c : it->childItems()) {
    if (c->objectName() == QLatin1String("segmentLabel")) out.append(c);
    collectLabels(c, out);
  }
}

// A function of the header's "..." menu (reset, diagnostics, fullscreen ...): opens the menu and picks the entry.
bool clickMore(Harness& h, const char* name) {
  if (!h.click("moreButton")) return false;
  const QByteArray item = QByteArray("menuItem_") + name;
  for (int i = 0; i < 60 && (h.item(item.constData()) == nullptr || !h.item(item.constData())->isVisible()); ++i) QTest::qWait(50);
  return h.click(item.constData());
}
// A header button, or its entry in the "..." menu when the header is too tight to show it (wide fonts, narrow windows).
bool clickHeader(Harness& h, const char* button, const char* menuName) {
  QQuickItem* it = h.item(button);
  if (it != nullptr && it->isVisible()) return h.click(button);
  return clickMore(h, menuName);
}

// Opens the Reset confirmation: it lives in the "..." menu.
bool openReset(Harness& h) { return clickMore(h, "reset"); }
QRectF sceneRect(QQuickItem* i) { return i ? i->mapRectToScene(QRectF(0, 0, i->width(), i->height())) : QRectF(); }
QRectF rectOf(Harness& h, const char* name) { return sceneRect(visibleItem(h, name)); }
QString textOf(QQuickItem* i) { return i ? i->property("text").toString() : QString();}
// The attached ToolTip.text of an item.
QString tooltipOf(QQuickItem* i) {
  if (i == nullptr) return {};
  const QQmlProperty p(i, QStringLiteral("ToolTip.text"), qmlContext(i));
  return p.isValid() ? p.read().toString() : QString();
}

// Failure diagnostics: the chain from an item up to the window with the geometry and the layout minimum of every ancestor.
QString chain(QQuickItem* it) {
  QString out;
  for (QQuickItem* i = it; i != nullptr; i = i->parentItem()) {
    QVariant minW;
    if (QQmlContext* ctx = qmlContext(i)) {
      const QQmlProperty p(i, QStringLiteral("Layout.minimumWidth"), ctx);
      if (p.isValid()) minW = p.read();
    }
    const QRectF r = sceneRect(i);
    out += QStringLiteral("\n  %1 '%2' x=%3 w=%4 impl=%5 minW=%6").arg(QString::fromLatin1(i->metaObject()->className()), i->objectName())
               .arg(r.x()).arg(r.width()).arg(i->implicitWidth()).arg(minW.isValid() ? minW.toString() : QStringLiteral("n/a"));
  }
  return out;
}

FakeHub* gHub = nullptr;

void pair(FakeHub& hub, Harness& h, bool withGames, bool withSaveHistory = false) {
  hub.hubId = QStringLiteral("hub-ingame");
  hub.name = QStringLiteral("Home");
  hub.decision = FakeHub::Decision::Approve;
  hub.features = {QStringLiteral("saves_v1"), QStringLiteral("sessions_v1")};
  if (withSaveHistory) hub.features += {QStringLiteral("saves_v2"), QStringLiteral("saves_v3"), QStringLiteral("saves_v4")};
  if (withGames) {
    hub.games = QJsonObject{{QStringLiteral("games"),
                             QJsonArray{uitest::gameJson(QStringLiteral("g1"), QStringLiteral("Lumen Drift"), uitest::sha256Hex("a"), 1)}}};
  }
  hub.sessions.insert(QStringLiteral("s1"), sessionObj(QStringLiteral("s1"), QStringLiteral("Lena"), QStringLiteral("Harbor Rally")));
  hub.sessions.insert(QStringLiteral("s2"), sessionObj(QStringLiteral("s2"), QStringLiteral("Mo"), QStringLiteral("Orbit Gardens")));
  QVERIFY(hub.start());
  QVERIFY(h.start());
  h.controller->addHub(hub.address());
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation, 8000);
  h.controller->confirmTrust();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsPairing, 8000);
  h.controller->requestPairing();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->sessions()->hubLink(), QStringLiteral("online"), 8000);
  if (withGames) QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
}

QImage frame() {
  QImage img(256, 384, QImage::Format_RGB32);
  img.fill(QColor(40, 90, 140));
  return img;
}

emu::DisplayProfile ndsProfile() {
  emu::DisplayProfile p;
  p.layout = QStringLiteral("vertical");
  p.gap = 0;
  p.screens = {{QStringLiteral("top"), 256, 192, false}, {QStringLiteral("bottom"), 256, 192, true}};
  return p;
}

void startPreview(Harness& h, bool library, const QString& title = QStringLiteral("Lumen Drift")) {
  GameSession* gs = h.controller->gameSession();
  if (library) {
    h.controller->selectGame(QStringLiteral("g1"));
    h.controller->adoptPreviewGame(QStringLiteral("g1"));
  }
  gs->setPreview(title, frame(), ndsProfile(), EmulationDiagnostics());
  gs->setPreviewFastForwardSupported(true);
  emit gs->started();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("game"), 8000);
  if (!library) h.controller->sessions()->gameStarted(QStringLiteral("g1"), title);
  h.controller->sessions()->setTab(QStringLiteral("session"));
  QQuickTest::qWaitForPolish(h.window);
}

void resize(Harness& h, int w, int ht) {
  h.window->resize(w, ht);
  QTest::qWait(60);
  QQuickTest::qWaitForPolish(h.window);
}

// ---- saves view inside the game (live) ----

// Stand-in for the running core: 12 bytes of battery save; "apply" writes the save file like the emulation thread does.
struct FakeCore {
  int flushes = 0, applies = 0;
  qint64 size = 12;
  QByteArray applied;
  QString file;
};

QByteArray fileBytes(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
void writeBytes(const QString& path, const QByteArray& data) {
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
  f.write(data);
}

// Scrolls the Flickable around `name` so that the item is at its top (screenshots of long views).
void scrollTo(Harness& h, const char* name, bool end = false) {
  QQuickItem* it = h.item(name);
  for (QQuickItem* a = it ? it->parentItem() : nullptr; a != nullptr; a = a->parentItem()) {
    const QVariant ch = a->property("contentHeight"), cy = a->property("contentY");
    QQuickItem* content = a->property("contentItem").value<QQuickItem*>();
    if (ch.isValid() && cy.isValid() && content != nullptr) {
      const qreal y = end ? ch.toReal() - a->height() : it->mapToItem(content, QPointF(0, 0)).y() - 8;
      a->setProperty("contentY", std::clamp<qreal>(y, 0, std::max<qreal>(0, ch.toReal() - a->height())));
      break;
    }
  }
  QQuickTest::qWaitForPolish(h.window);
}

// Adds letter spacing to every item that has a font (stand-in for the wider Windows fonts).
void widenFonts(Harness& h, qreal spacing) {
  QList<QQuickItem*> all{h.window->contentItem()};
  for (int i = 0; i < all.size(); ++i) all.append(all.at(i)->childItems());
  for (QQuickItem* it : std::as_const(all)) {
    const QVariant fv = it->property("font");
    if (fv.metaType() != QMetaType::fromType<QFont>()) continue;
    QFont f = fv.value<QFont>();
    if (f.letterSpacing() < 1.0) { f.setLetterSpacing(QFont::AbsoluteSpacing, spacing); it->setProperty("font", f); }
  }
  QQuickTest::qWaitForPolish(h.window);
  QTest::qWait(60);
}

}  // namespace

class InGameUiTest : public QObject {
  Q_OBJECT

  // The header zones lie inside the window and the header, in this order, without overlapping each other.
  static QString zonesProblem(Harness& h, const QStringList& names) {
    QQuickItem* header = visibleItem(h, "gameHeader");
    if (header == nullptr) return QStringLiteral("no header");
    const QRectF hr = sceneRect(header);
    const QRectF win(0, 0, h.window->width(), h.window->height());
    QList<QPair<QString, QRectF>> rects;
    for (const QString& n : names) {
      QQuickItem* it = visibleItem(h, qPrintable(n));
      if (it == nullptr) continue;
      const QRectF r = sceneRect(it);
      if (!hr.contains(r.adjusted(0.5, 0.5, -0.5, -0.5)) || !win.contains(r.adjusted(0.5, 0.5, -0.5, -0.5))) {
        return QStringLiteral("%1 outside header/window: %2,%3 %4x%5 (header %6 wide, window %7)%8")
            .arg(n).arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).arg(hr.width()).arg(win.width()).arg(chain(it));
      }
      rects.append({n, r});
    }
    for (int i = 0; i < rects.size(); ++i) {
      for (int j = i + 1; j < rects.size(); ++j) {
        if (rects[i].second.adjusted(0.5, 0, -0.5, 0).intersects(rects[j].second.adjusted(0.5, 0, -0.5, 0))) {
          return rects[i].first + QStringLiteral(" overlaps ") + rects[j].first + chain(visibleItem(h, qPrintable(rects[i].first)));
        }
      }
      if (i + 1 < rects.size() && rects[i].second.right() > rects[i + 1].second.left() + 1.5) {
        return rects[i].first + QStringLiteral(" is not left of ") + rects[i + 1].first;
      }
    }
    return {};
  }

  static const QStringList& zoneNames() {
    static const QStringList z{QStringLiteral("backToLibraryButton"), QStringLiteral("gameTitle"), QStringLiteral("multiviewTitle"),
                               QStringLiteral("modeSegment"), QStringLiteral("fastForwardButton"), QStringLiteral("speedValueButton"),
                               QStringLiteral("layoutSwitch"), QStringLiteral("tabMultiview"), QStringLiteral("addSessionToggle"), QStringLiteral("panelToggle"),
                               QStringLiteral("leaveMultiview"), QStringLiteral("leaveButton"), QStringLiteral("fullscreenButton"), QStringLiteral("pauseButton"),
                               QStringLiteral("moreButton")};
    return z;
  }

  // Diagnostics sit on the stage, top right: inside the play area, under the header, never over the panel.
  static QString overlayProblem(Harness& h) {
    const QRectF play = rectOf(h, "playArea"), ov = rectOf(h, "diagnosticsOverlay"), panel = rectOf(h, "gamePanel"), header = rectOf(h, "gameHeader");
    if (ov.isEmpty()) return QStringLiteral("diagnostics not visible");
    if (ov.left() < play.left() - 0.5 || ov.right() > play.right() + 0.5 || ov.top() < play.top() - 0.5 || ov.bottom() > play.bottom() + 0.5)
      return QStringLiteral("diagnostics outside the stage");
    if (ov.top() < header.bottom() - 0.5) return QStringLiteral("diagnostics over the header");
    if (ov.intersects(panel) && ov.intersected(panel).width() > 1.0) return QStringLiteral("diagnostics over the panel");
    if (ov.center().x() < play.center().x()) return QStringLiteral("diagnostics not on the right of the stage");
    return {};
  }

  // A running game (preview, no core) with a real save session against the FakeHub and a fake core behind the live hooks.
  void startLiveGame(FakeHub& hub, Harness& h, FakeCore* core) {
    // The Hub's data exists before pairing: a history refresh started by the connection coming up must not race it
    hub.setHubSave(QStringLiteral("g1"), "hub-cp-1-xxx");
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "hub-old-1234", QStringLiteral("session_end"));
    hub.addHistory(QStringLiteral("g1"), QStringLiteral("default"), "hub-snap-123", QStringLiteral("manual_snapshot"), QStringLiteral("Boss door"));
    pair(hub, h, true, true);
    const QString sha = uitest::sha256Hex("a");
    SaveSync* sync = h.controller->saveSync();
    QSignalSpy ready(sync, &SaveSync::startReady);
    sync->prepareStart(QStringLiteral("g1"), QStringLiteral("/cache/") + sha + QStringLiteral(".nds"), {sha});
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
    core->file = sync->activeSaveFile();
    QCOMPARE(fileBytes(core->file), QByteArray("hub-cp-1-xxx"));
    SaveSync::LiveHooks hooks;
    hooks.flush = [core]() { ++core->flushes; };
    hooks.accepts = [core](qint64 n) { return n == core->size; };
    hooks.apply = [core](const QByteArray& d) {
      ++core->applies;
      core->applied = d;
      QFile f(core->file);
      if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
      f.write(d);
      return true;
    };
    sync->setLiveHooks(std::move(hooks));
    startPreview(h, true);   // GameSession::started begins the save session
    QVERIFY(sync->sessionActive());
    SaveHistoryController* hist = h.controller->saveHistory();
    QTRY_VERIFY_WITH_TIMEOUT(hist->available() && !hist->loading() && hist->versionCount() == 3, 8000);
    QVERIFY(hist->gameRunning());
    resize(h, 1280, 800);
  }
  // Every named visible item lies inside the game panel, which lies inside the window.
  static QString outsidePanel(Harness& h, const QStringList& names, int minChecked) {
    QQuickTest::qWaitForPolish(h.window);
    QTest::qWait(40);
    const QRectF panel = rectOf(h, "gamePanel");
    if (panel.isEmpty() || panel.right() > h.window->width() + 0.5) return QStringLiteral("panel outside the window");
    int checked = 0;
    for (const QString& n : names) {
      for (QQuickItem* it : h.items(n.toLatin1().constData())) {
        if (!it->isVisible() || it->width() <= 0) continue;
        ++checked;
        const QRectF r = sceneRect(it);
        if (r.left() < panel.left() - 0.5 || r.right() > panel.right() + 0.5) {
          return QStringLiteral("'%1' outside the panel (%2..%3 vs %4..%5)%6").arg(n).arg(r.left()).arg(r.right()).arg(panel.left()).arg(panel.right()).arg(chain(it));
        }
      }
    }
    return checked >= minChecked ? QString() : QStringLiteral("only %1 items visible").arg(checked);
  }

 private slots:
  void initTestCase() { uitest::installWarningCounter(); }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  // ---- Session ----

  // ---- Manage saves inside the game ----

  void manageSavesOpensTheSavesViewInsideTheGame() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    FakeCore core;
    startLiveGame(hub, h, &core);
    PlayerController* c = h.controller.get();
    GameSession* gs = c->gameSession();
    SaveHistoryController* hist = c->saveHistory();
    QVERIFY(shown(h, "manageSavesButton"));
    QVERIFY(h.click("manageSavesButton"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "savesView"), 4000);
    // Still in the game: no navigation, the game runs on
    QCOMPARE(c->screen(), QStringLiteral("game"));
    QVERIFY(gs->isActive());
    QVERIFY(gs->state() != GameSession::Idle);
    QVERIFY(shown(h, "gamePanel") && shown(h, "gameHeader"));
    QVERIFY(shown(h, "endGameButton"));          // Quit game stays reachable
    QVERIFY(!shown(h, "gameSnapshotButton"));    // the panel page is replaced by the saves view
    QVERIFY(hist->liveMode() && hist->liveApply());
    QTRY_COMPARE(h.items("historyRow").size() + h.items("historyRowCurrent").size(), 3);
    QVERIFY(shown(h, "savesRunningBanner"));
    QVERIFY(h.item("restoreButton")->isEnabled());    // restore works live
    QVERIFY(!h.item("newSlotButton")->isEnabled());   // slots cannot be changed while the game runs
    // History filter
    QVERIFY(h.click("historyFilterSnapshots"));
    QTRY_COMPARE(h.items("historyRow").size() + h.items("historyRowCurrent").size(), 2);
    QVERIFY(h.click("historyFilterAll"));
    QTRY_COMPARE(h.items("historyRow").size() + h.items("historyRowCurrent").size(), 3);
    uitest::saveShot(h.window, QStringLiteral("ingame-saves-view"));

    // Scene rects at 1280 and 1920 px: the panel is 320 wide and everything stays inside it
    const QStringList names{"savesView", "savesBack", "historyRefresh", "slotTabs", "currentBar", "saveActions", "snapshotButton", "uploadSaveButton",
                            "historyHeader", "historyFilter", "historyRow", "restoreButton", "deleteButton", "savesRunningBanner", "endGameButton"};
    for (const int w : {1280, 1920}) {
      resize(h, w, 800);
      QCOMPARE(rectOf(h, "gamePanel").width(), 320.0);
      const QRectF box = rectOf(h, "gamePanelSaves"), panel = rectOf(h, "gamePanel");
      QVERIFY2(panel.contains(box.adjusted(0.5, 0.5, -0.5, -0.5)), qPrintable(QStringLiteral("saves box outside the panel at %1").arg(w)));
      QVERIFY2(box.bottom() <= rectOf(h, "endGameButton").top(), "saves view above Quit game");
      const QString problem = outsidePanel(h, names, 8);
      QVERIFY2(problem.isEmpty(), qPrintable(QStringLiteral("%1 px: %2").arg(w).arg(problem)));
    }
    resize(h, 1280, 800);
    widenFonts(h, 6.0);
    const QString wide = outsidePanel(h, names, 8);
    QVERIFY2(wide.isEmpty(), qPrintable(QStringLiteral("wide fonts: ") + wide));

    // Back returns to the panel; the game was never left
    QVERIFY(h.click("savesBack"));
    QTRY_VERIFY(!shown(h, "savesView"));
    QVERIFY(shown(h, "gameSnapshotButton"));
    QCOMPARE(c->screen(), QStringLiteral("game"));
    QVERIFY(gs->isActive());
    QVERIFY(!hist->liveMode());
    QVERIFY(h.click("manageSavesButton"));
    QTRY_VERIFY(shown(h, "savesView"));
    QTest::keyClick(h.window, Qt::Key_Escape);   // Esc in the saves view = back, not "pause the game"
    QTRY_VERIFY(!shown(h, "savesView"));
    QCOMPARE(c->screen(), QStringLiteral("game"));
  }

  void createSnapshotRestoreAndDeleteWorkLive() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    FakeCore core;
    startLiveGame(hub, h, &core);
    PlayerController* c = h.controller.get();
    SaveHistoryController* hist = c->saveHistory();
    SaveSync* sync = c->saveSync();
    const QString dir = QFileInfo(core.file).absolutePath();
    QVERIFY(h.click("manageSavesButton"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "snapshotButton"), 4000);

    // Create snapshot: the game's current state is saved first (core flush + upload), then the snapshot is made
    writeBytes(core.file, "play-1-xxxxx");
    QVERIFY(h.click("snapshotButton"));
    QTRY_VERIFY(shown(h, "snapshotForm"));
    h.item("snapshotLabel")->setProperty("text", QStringLiteral("Before the boss"));
    QVERIFY(h.click("snapshotCreate"));
    QTRY_VERIFY_WITH_TIMEOUT(hist->message().startsWith(QStringLiteral("Snapshot v")), 8000);
    QVERIFY(core.flushes >= 1);
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).content, QByteArray("play-1-xxxxx"));   // uploaded first
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).history.last().reason, QStringLiteral("manual_snapshot"));
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).history.last().label, QStringLiteral("Before the boss"));
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).history.last().content, QByteArray("play-1-xxxxx"));
    QCOMPARE(c->screen(), QStringLiteral("game"));
    QVERIFY(c->gameSession()->isActive());
    QTRY_COMPARE_WITH_TIMEOUT(h.items("historyRow").size() + h.items("historyRowCurrent").size(), 4, 8000);

    // Delete a snapshot
    QVERIFY(h.click("deleteButton"));
    QTRY_VERIFY(shown(h, "deleteConfirmBox"));
    QVERIFY(h.click("deleteConfirm"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.deleteCount, 1, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(h.items("historyRow").size() + h.items("historyRowCurrent").size(), 3, 8000);

    // Restore while running: confirmation first (with the restart line), nothing changes before it
    writeBytes(core.file, "play-2-xxxxx");   // unsynced progress of the running game
    hist->requestRestore(1);
    QTRY_VERIFY(shown(h, "restoreConfirmBox"));
    QVERIFY(textOf(visibleItem(h, "confirmBody")).contains(QStringLiteral("The game restarts from this save")));
    QCOMPARE(hub.restoreCount, 0);
    QCOMPARE(core.applies, 0);
    QCOMPARE(fileBytes(core.file), QByteArray("play-2-xxxxx"));
    scrollTo(h, "restoreConfirmBox");
    uitest::saveShot(h.window, QStringLiteral("ingame-saves-restore-confirm"));
    const QString problem = outsidePanel(h, {"restoreConfirmBox", "confirmTitle", "confirmBody", "restoreCancel", "restoreConfirm", "savesView"}, 4);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
    // Cancel changes nothing, a second request is confirmed
    QVERIFY(h.click("restoreCancel"));
    QTRY_VERIFY(!shown(h, "restoreConfirmBox"));
    hist->requestRestore(1);
    QTRY_VERIFY(shown(h, "restoreConfirm"));
    QVERIFY(h.click("restoreConfirm"));
    QTRY_COMPARE_WITH_TIMEOUT(hub.restoreCount, 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(hist->message().contains(QStringLiteral("restored")), 8000);
    QCOMPARE(core.applies, 1);
    QCOMPARE(core.applied, QByteArray("hub-old-1234"));              // loaded into the running core
    QCOMPARE(fileBytes(core.file), QByteArray("hub-old-1234"));
    const QStringList backups = QDir(dir).entryList({QStringLiteral("*.restore-*.bak")});
    QCOMPARE(backups.size(), 1);                                     // the local backup of the game's progress
    QCOMPARE(fileBytes(dir + QLatin1Char('/') + backups.first()), QByteArray("play-2-xxxxx"));
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).history.last().reason, QStringLiteral("before_restore"));
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).history.last().content, QByteArray("play-2-xxxxx"));   // and on the Hub
    QCOMPARE(c->screen(), QStringLiteral("game"));
    QVERIFY(c->gameSession()->isActive());
    QVERIFY(sync->sessionActive());
    QVERIFY(!SaveStore::loadState(dir).pending);

    // Upload save file while running: confirmation with the restart line, then the core loads it
    const QString picked = h.dir.filePath(QStringLiteral("picked.sav"));
    writeBytes(picked, "picked-12-by");
    QTRY_VERIFY_WITH_TIMEOUT(!hist->loading() && hist->current().value(QStringLiteral("revision")).toInt() == hub.saves.value(QStringLiteral("g1")).revision, 8000);
    hist->requestUploadFile(picked);
    QTRY_VERIFY(shown(h, "uploadConfirmBox"));
    QVERIFY(textOf(visibleItem(h, "uploadConfirmNote")).contains(QStringLiteral("The game restarts from this save")));
    QVERIFY(h.click("uploadConfirm"));
    QTRY_VERIFY2_WITH_TIMEOUT(hub.uploadCount == 1, qPrintable(hist->message()), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(core.applies, 2, 8000);
    QCOMPARE(core.applied, QByteArray("picked-12-by"));
    QCOMPARE(QDir(dir).entryList({QStringLiteral("*.upload-*.bak")}).size(), 1);
    QCOMPARE(c->screen(), QStringLiteral("game"));
  }

  void restoreAsksToRestartWhenTheCoreCannotTakeTheSave() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    FakeCore core;
    core.size = 99;   // the core's save memory has another size: nothing can be loaded live
    startLiveGame(hub, h, &core);
    SaveHistoryController* hist = h.controller->saveHistory();
    QVERIFY(h.click("manageSavesButton"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "savesView"), 4000);
    hist->requestRestore(1);
    QTRY_VERIFY(shown(h, "restoreConfirm"));
    QVERIFY(h.click("restoreConfirm"));
    QTRY_VERIFY_WITH_TIMEOUT(hist->messageIsError() && hist->message().contains(QStringLiteral("Quit the game")), 8000);
    QCOMPARE(hub.restoreCount, 0);              // nothing happened on the Hub
    QCOMPARE(core.applies, 0);
    QCOMPARE(fileBytes(core.file), QByteArray("hub-cp-1-xxx"));
    QVERIFY(shown(h, "historyMessage"));
    QCOMPARE(h.controller->screen(), QStringLiteral("game"));
  }

  void resolveConflictInTheGameBothWays() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    FakeCore core;
    startLiveGame(hub, h, &core);
    PlayerController* c = h.controller.get();
    SaveSync* sync = c->saveSync();
    const QString dir = QFileInfo(core.file).absolutePath();
    const auto makeConflict = [&](const QByteArray& local, const QByteArray& hubNew) {
      writeBytes(core.file, local);
      hub.setHubSave(QStringLiteral("g1"), hubNew);   // another device saved meanwhile
      QSignalSpy fin(sync, &SaveSync::finalSyncFinished);
      sync->finalSync(false);                          // the checkpoint upload runs into the conflict
      QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 1, 8000);
      QTRY_COMPARE_WITH_TIMEOUT(c->selectedGame().value(QStringLiteral("syncKind")).toString(), QStringLiteral("conflict"), 8000);
    };
    makeConflict("local-1-xxxxx", "hub-newer-xx");
    QVERIFY(h.click("manageSavesButton"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "savesConflictBanner"), 8000);
    QVERIFY(h.click("resolveConflictButton"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "conflictBox") && shown(h, "conflictLocal") && shown(h, "conflictHub"), 8000);
    QCOMPARE(c->screen(), QStringLiteral("game"));
    QVERIFY(c->saveConflict().isEmpty());
    QCOMPARE(textOf(visibleItem(h, "conflictHubDevice")), QStringLiteral("Laptop Office"));
    QVERIFY(!textOf(visibleItem(h, "conflictLocalWhen")).isEmpty());
    scrollTo(h, "conflictBox");
    uitest::saveShot(h.window, QStringLiteral("ingame-saves-conflict"));
    scrollTo(h, "conflictKeepLocal", true);
    uitest::saveShot(h.window, QStringLiteral("ingame-saves-conflict-actions"));
    scrollTo(h, "conflictBox");
    const QStringList names{"savesView", "conflictBox", "conflictLocal", "conflictHub", "conflictLocalDevice", "conflictHubDevice", "conflictLocalWhen",
                            "conflictHubWhen", "conflictKeepLocal", "conflictUseHub", "conflictCancel"};
    for (const int w : {1280, 1920}) {
      resize(h, w, 800);
      const QString problem = outsidePanel(h, names, 8);
      QVERIFY2(problem.isEmpty(), qPrintable(QStringLiteral("%1 px: %2").arg(w).arg(problem)));
    }
    resize(h, 1280, 800);
    widenFonts(h, 6.0);
    const QString wide = outsidePanel(h, names, 8);
    QVERIFY2(wide.isEmpty(), qPrintable(QStringLiteral("wide fonts: ") + wide));

    // Use the Hub save: backup of the device copy, then the core loads the Hub save
    QVERIFY(h.click("conflictUseHub"));
    QTRY_VERIFY_WITH_TIMEOUT(!shown(h, "conflictBox"), 8000);
    QCOMPARE(core.applied, QByteArray("hub-newer-xx"));
    QCOMPARE(fileBytes(core.file), QByteArray("hub-newer-xx"));
    const QStringList backups = QDir(dir).entryList({QStringLiteral("*.local-*.bak")});
    QCOMPARE(backups.size(), 1);
    QCOMPARE(fileBytes(dir + QLatin1Char('/') + backups.first()), QByteArray("local-1-xxxxx"));
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).conflicts.first().status, QStringLiteral("resolved_hub"));
    QTRY_COMPARE(c->selectedGame().value(QStringLiteral("syncKind")).toString(), QStringLiteral("synced"));
    QCOMPARE(c->screen(), QStringLiteral("game"));
    QVERIFY(c->gameSession()->isActive());

    // Next conflict: keep this device's save; the core keeps its save, the Hub takes it as the new current version
    const int applies = core.applies;
    makeConflict("local-2-xxxxx", "hub-third-xx");
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "resolveConflictButton"), 8000);
    QVERIFY(h.click("resolveConflictButton"));
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "conflictKeepLocal") && h.item("conflictKeepLocal")->isEnabled(), 8000);
    QVERIFY(h.click("conflictKeepLocal"));
    QTRY_VERIFY_WITH_TIMEOUT(!shown(h, "conflictBox"), 8000);
    QCOMPARE(core.applies, applies);
    QCOMPARE(hub.saves.value(QStringLiteral("g1")).content, QByteArray("local-2-xxxxx"));
    QCOMPARE(fileBytes(core.file), QByteArray("local-2-xxxxx"));
    QTRY_COMPARE(c->selectedGame().value(QStringLiteral("syncKind")).toString(), QStringLiteral("synced"));
    QCOMPARE(c->screen(), QStringLiteral("game"));
    QVERIFY(c->gameSession()->isActive());
  }

  // ---- screenshots (UZUME_SHOT_DIR only) ----

  void zScreenshots() {
    if (qEnvironmentVariableIsEmpty("UZUME_SHOT_DIR")) QSKIP("UZUME_SHOT_DIR not set");
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, true);
    PlayerController* c = h.controller.get();
    SessionController* ctl = c->sessions();
    auto shot = [&](const char* name) { uitest::saveShot(h.window, QString::fromLatin1(name)); };
    auto feed = [&](const QString& id, const QColor& tint) {
      if (QObject* v = ctl->viewer(id)) {
        QImage img(256, 192, QImage::Format_RGB32);
        img.fill(tint);
        QMetaObject::invokeMethod(v, "frameReady", Qt::DirectConnection, Q_ARG(QImage, img));
      }
    };
    startPreview(h, true);
    resize(h, 1440, 900);
    shot("3g-2-session-1440");
    if (const int w = qEnvironmentVariableIntValue("UZUME_SHOT_WIDTH"); w > 0 && w != 1440) {   // extra width, e.g. 1920
      resize(h, w, 1080);
      shot((QByteArray("3g-2-session-") + QByteArray::number(w)).constData());
      resize(h, 1440, 900);
    }
    // shared + Reset popover
    ctl->shareSession();
    QTRY_VERIFY_WITH_TIMEOUT(ctl->shared(), 8000);
    QQuickTest::qWaitForPolish(h.window);
    openReset(h);
    QTest::qWait(250);
    shot("3g-3-session-shared-reset-1440");
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTest::qWait(150);
    ctl->stopSharing();
    QTest::qWait(300);
    resize(h, 1280, 800);
    shot("3g-2-session-1280");
    // diagnostics in the Session
    resize(h, 1440, 900);
    ctl->diagnostics()->setOpen(true);
    QTest::qWait(200);
    shot("3t-2-diagnostics-1440");
    ctl->diagnostics()->setOpen(false);
    // multiview
    clickHeader(h, "tabMultiview", "multiview");
    QQuickTest::qWaitForPolish(h.window);
    for (const char* id : {"s1", "s2"}) {
      h.click("addSessionToggle");
      h.clickWhenReady((QByteArray("multiviewAddButton_") + id).constData());
      QTest::qWait(150);
    }
    clickMode(h, "modeGrid");
    QTest::qWait(200);
    feed(QStringLiteral("s1"), QColor(150, 80, 60));
    feed(QStringLiteral("s2"), QColor(70, 140, 90));
    QTest::qWait(150);
    h.click("addSessionToggle");
    QTest::qWait(250);
    shot("3r-2-multiview-1440");
    h.click("addSessionToggle");
    QTest::qWait(150);
    ctl->diagnostics()->setOpen(true);
    QTest::qWait(250);
    shot("3x-2-diagnostics-multiview-1440");
    ctl->diagnostics()->setOpen(false);
    resize(h, 1280, 800);
    h.click("addSessionToggle");
    QTest::qWait(250);
    shot("3r-2-multiview-1280");
    h.click("addSessionToggle");
    // sidebar strip
    resize(h, 1440, 900);
    c->leaveGameView();
    QTRY_VERIFY(c->gameSession()->isPaused());
    QQuickTest::qWaitForPolish(h.window);
    shot("sidebar-now-running");
  }

  // Icons, labels and carets of the header controls sit on one centre line (Pause, Speed-up, "2× ▾", Reset, Session/Multiview, layout,
  // Diagnostics, Fullscreen): within 1 px of each other and of the header's centre.
  void headerIconsAndLabelsShareOneCentreLine() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    QVERIFY(h.controller->gameSession()->fastForwardAvailable());
    for (const QSize sz : {QSize(1920, 1080), QSize(1440, 900)}) {
      resize(h, sz.width(), sz.height());
      const qreal mid = rectOf(h, "gameHeader").center().y();
      int checked = 0;
      for (const char* name : {"backToLibraryButton", "fastForwardButton", "speedValueButton", "layoutSwitch", "tabMultiview", "panelToggle", "fullscreenButton", "pauseButton", "moreButton"}) {
        QQuickItem* btn = visibleItem(h, name);
        if (btn == nullptr) continue;   // not shown at this size (layout picker without layouts)
        ++checked;
        const QString tag = QStringLiteral("%1 at %2 px").arg(QString::fromLatin1(name)).arg(sz.width());
        QVERIFY2(qAbs(sceneRect(btn).center().y() - mid) <= 1.0, qPrintable(tag + QStringLiteral(": button off the header centre")));
        // The label / glyph of a button sits on the button's centre line
        const auto items = btn->findChildren<QQuickItem*>();
        for (QQuickItem* it : items) {
          if (!it->isVisible() || it->width() <= 0 || it->height() <= 0 || it->height() >= btn->height()) continue;
          if (it->property("text").toString().isEmpty()) continue;
          QVERIFY2(qAbs(sceneRect(it).center().y() - sceneRect(btn).center().y()) <= 1.5,
                   qPrintable(tag + QStringLiteral(": label centre %1 vs button %2").arg(sceneRect(it).center().y()).arg(sceneRect(btn).center().y())));
        }
      }
      QVERIFY2(checked >= (qEnvironmentVariableIsSet("UZUME_TEST_WIDE_FONT") ? 3 : 5), qPrintable(QStringLiteral("only %1 buttons checked").arg(checked)));   // wide fonts: the bar gives way
      // Title and system chip share the line with the buttons
      QQuickItem* title = visibleItem(h, "gameTitle");
      QVERIFY(title != nullptr);
      QVERIFY(qAbs(sceneRect(title).center().y() - mid) <= 14.0);
    }
  }

  void sessionHeaderZonesAndPanel() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    GameSession* gs = h.controller->gameSession();
    SessionController* ctl = h.controller->sessions();
    QVERIFY(gs->fastForwardAvailable());

    for (const QSize sz : {QSize(1440, 900), QSize(1280, 800), QSize(960, 600)}) {
      resize(h, sz.width(), sz.height());
      const QString tag = QStringLiteral("%1 px: ").arg(sz.width());
      const QString problem = zonesProblem(h, zoneNames());
      QVERIFY2(problem.isEmpty(), qPrintable(tag + problem));
      // The panel and the play area stay inside the window as well
      QVERIFY2(rectOf(h, "gamePanel").right() <= h.window->width() + 0.5, qPrintable(tag + QStringLiteral("panel outside the window") + chain(visibleItem(h, "gamePanel"))));
      QVERIFY2(rectOf(h, "playArea").width() > 300, qPrintable(tag + QStringLiteral("play area too narrow")));
      QCOMPARE(rectOf(h, "gamePanel").width(), 320.0);
      QCOMPARE(rectOf(h, "gameHeader").height(), 56.0);
      // Zones of the Session: no "YOUR GAME" label, no "Add tile", no Quit in the header
      QVERIFY(!shown(h, "yourGameLabel") && !shown(h, "addSessionToggle") && !shown(h, "quitButton"));
      QVERIFY(shown(h, "backToLibraryButton") && shown(h, "gameTitle") && shown(h, "sharePill") && shown(h, "moreButton"));
      if (sz.width() >= 1280) QVERIFY(shown(h, "pauseButton") && (shown(h, "fastForwardButton") || qEnvironmentVariableIsSet("UZUME_TEST_WIDE_FONT")));
      QCOMPARE(textOf(visibleItem(h, "gameTitle")), QStringLiteral("Lumen Drift"));
      QCOMPARE(textOf(visibleItem(h, "sharePill")), QStringLiteral("Not shared"));
    }

    resize(h, 1440, 900);
    // Panel: SHARING, then (with slots) SAVE, "Quit game" pinned at the bottom with its note; the removed parts are gone.
    {
      const QRectF panel = rectOf(h, "gamePanel"), share = rectOf(h, "panelShareState"), quit = rectOf(h, "endGameButton"), note = rectOf(h, "quitNote");
      QVERIFY(!share.isEmpty() && !quit.isEmpty() && !note.isEmpty());
      QVERIFY2(share.top() < quit.top(), "SHARING above Quit game");
      QVERIFY2(quit.bottom() <= panel.bottom() && note.bottom() <= panel.bottom() && panel.bottom() - note.bottom() < 100, "Quit game pinned to the bottom (above the hint row)");
      QCOMPARE(textOf(visibleItem(h, "endGameButton")), QStringLiteral("Quit game"));
      QCOMPARE(textOf(visibleItem(h, "quitNote")), QStringLiteral("Saves and syncs first, then returns to the Library."));
      for (const char* gone : {"speedSelect", "speedBlock", "newSlotButton", "slotSegment", "diagToggle", "readOnlyNote", "tabSegment", "tabDiagnostics"}) {
        QVERIFY2(!shown(h, gone), gone);
      }
      QVERIFY(shown(h, "visibilitySegment") && shown(h, "visibilityHint") && shown(h, "shareButton"));
      QCOMPARE(textOf(visibleItem(h, "shareButton")), QStringLiteral("Share this game…"));
    }

    // Panel collapses to the 56 px rail and back
    QVERIFY(clickHeader(h, "panelToggle", "panel"));
    QTRY_COMPARE(rectOf(h, "gamePanel").width(), 56.0);
    QVERIFY(shown(h, "panelRail") && shown(h, "panelExpandButton") && !shown(h, "endGameButton"));
    QVERIFY(h.click("panelExpandButton"));
    QTRY_COMPARE(rectOf(h, "gamePanel").width(), 320.0);

    // Game controls: Pause label/width stays, Speed-up toggles and shows the value, Space unchanged
    QVERIFY(clickHeader(h, "fastForwardButton", "speedup"));   // or "⋯ > Speed-up" when the bar gave way (wide fonts)
    QVERIFY(gs->fastForward());
    if (shown(h, "fastForwardButton")) QVERIFY(visibleItem(h, "fastForwardButton")->property("inverted").toBool());   // on = the ink plane
    QVERIFY(clickHeader(h, "fastForwardButton", "speedup"));
    QVERIFY(!gs->fastForward());

    // Reset asks first: a dialog over the stage (the header stays), danger action; Cancel keeps the game, Reset confirms
    QVERIFY(!shown(h, "resetConfirm"));
    QVERIFY(openReset(h));
    QTRY_VERIFY(shown(h, "resetConfirm"));
    QCOMPARE(textOf(visibleItem(h, "dialogTitle")), QStringLiteral("Reset Lumen Drift?"));
    QVERIFY(shown(h, "gameHeader"));
    {
      const QRectF scrim = rectOf(h, "dialogScrim"), play = rectOf(h, "playArea");
      QVERIFY2(scrim.right() <= play.right() + 0.5 && scrim.left() >= play.left() - 0.5, "the scrim covers the stage only");
    }
    QVERIFY(shown(h, "resetCancel") && shown(h, "resetConfirm"));
    QVERIFY(h.click("resetCancel"));
    QTRY_VERIFY(!shown(h, "resetConfirm"));
    QVERIFY(openReset(h));
    QTRY_VERIFY(shown(h, "resetConfirm"));
    // Esc closes the dialog first and does not pause
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "resetConfirm"));
    QVERIFY(!gs->isPaused());
    QVERIFY(openReset(h));
    QTRY_VERIFY(shown(h, "resetConfirm"));
    QVERIFY(h.click("resetConfirm"));
    QTRY_VERIFY(!shown(h, "resetConfirm"));

    // Fullscreen is an icon button in the header; the "..." menu holds Diagnostics, Screenshot, Hotkeys, Reset game and Report a problem
    resize(h, 1280, 800);
    QVERIFY(h.click("moreButton"));
    QTRY_VERIFY(shown(h, "menuItem_diagnostics"));
    QVERIFY(shown(h, "fullscreenButton") != shown(h, "menuItem_fullscreen"));   // the menu lists it only when the header is too tight for its button
    QVERIFY(shown(h, "menuItem_screenshot") && shown(h, "menuItem_reset") && shown(h, "menuItem_hotkeys") && shown(h, "menuItem_report"));
    QCOMPARE(textOf(visibleItem(h, "menuItem_reset")), QStringLiteral("Reset game…"));
    // 0.19: "Settings for this game…" in the menu opens the per-game dialog over the game; Done closes it.
    QVERIFY(shown(h, "menuItem_gameSettings"));
    QVERIFY(h.click("menuItem_gameSettings"));
    QTRY_VERIFY(shown(h, "gameSettingsDialog"));
    QVERIFY(shown(h, "gameSettingsFooter"));
    QVERIFY(h.click("gameSettingsDone"));
    QTRY_VERIFY(!shown(h, "gameSettingsDialog"));
    // The hold-Select sheet is created only while the hotkey button is held (rows: ui_emulation_controllers).
    QVERIFY(h.item("padHotkeySheetLoader") != nullptr);
    QVERIFY(!h.item("padHotkeySheetLoader")->property("active").toBool());
    QVERIFY(h.item("padHotkeySheet") == nullptr);
    // "⋯ > Hotkeys" opens the same sheet over the game without leaving it; "Change hotkeys" goes to Settings > Hotkeys.
    QVERIFY(h.click("moreButton"));
    QTRY_VERIFY(shown(h, "menuItem_hotkeys"));
    QVERIFY(h.click("menuItem_hotkeys"));
    QTRY_VERIFY(shown(h, "padHotkeySheet"));
    QCOMPARE(h.controller->screen(), QStringLiteral("game"));
    QVERIFY(shown(h, "hotkeySheetChange") && shown(h, "hotkeySheetClose"));
    QTest::keyClick(h.window, Qt::Key_Escape);   // Esc closes the sheet first; the game is not paused
    QTRY_VERIFY(!shown(h, "padHotkeySheet"));
    QVERIFY(!gs->isPaused());
    QVERIFY(h.click("moreButton"));
    QTRY_VERIFY(shown(h, "menuItem_hotkeys"));
    QVERIFY(h.click("menuItem_hotkeys"));
    QTRY_VERIFY(shown(h, "hotkeySheetClose"));
    QVERIFY(h.click("hotkeySheetClose"));
    QTRY_VERIFY(!shown(h, "padHotkeySheet"));
    QVERIFY(h.click("moreButton"));
    QTRY_VERIFY(shown(h, "menuItem_hotkeys"));
    QVERIFY(h.click("menuItem_hotkeys"));
    QTRY_VERIFY(shown(h, "hotkeySheetChange"));
    QVERIFY(h.click("hotkeySheetChange"));
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("settings"));
    QTRY_VERIFY(h.item("hotkeysInfo") != nullptr && h.item("hotkeysInfo")->isVisible());
    h.controller->resumeGame();
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("game"));
    QVERIFY(h.click("moreButton"));
    QTRY_VERIFY(shown(h, "menuItem_diagnostics"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "menuItem_diagnostics"));

    // Screen layout menu (this game only) with the built layouts
    QVERIFY(gs->screenLayouts().size() > 1);
    if (shown(h, "layoutSwitch")) {
      QVERIFY(h.click("layoutSwitch"));
    } else {
      QVERIFY(clickMore(h, "screens"));   // too tight for the Screens button: "⋯ > Screens" opens the same menu
    }
    QTRY_VERIFY(shown(h, "menuItem_layout_side"));
    QVERIFY(h.click("menuItem_layout_side"));
    QCOMPARE(ctl->screenLayout(), QStringLiteral("side"));
    QTRY_VERIFY(!shown(h, "menuItem_layout_side"));
    ctl->setScreenLayout(QStringLiteral("stacked"));

    // Hotkeys unchanged: F3 diagnostics, Space speed-up
    QVERIFY(!ctl->diagnostics()->isOpen());
    QTest::keyClick(h.window, Qt::Key_F3);
    QTRY_VERIFY(ctl->diagnostics()->isOpen());
    QTest::keyClick(h.window, Qt::Key_F3);
    QTRY_VERIFY(!ctl->diagnostics()->isOpen());
  }

  // "Settings for this game" of the running game: one height on every tab (no jumping), and the speed-up and layout rows show the
  // live in-game value with its source instead of the stored default.
  void gameSettingsShowLiveValues() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, true);
    startPreview(h, true);
    GameSession* gs = h.controller->gameSession();
    SessionController* ctl = h.controller->sessions();
    QVERIFY(h.click("moreButton"));
    QTRY_VERIFY(shown(h, "menuItem_gameSettings"));
    QVERIFY(h.click("menuItem_gameSettings"));
    QTRY_VERIFY(shown(h, "gameSettingsDialog"));
    {
      // One height on every tab (no jumping), and the rows show the live in-game values with their source.
      QQuickItem* host = h.item("gameSettingsContent");
      QVERIFY(host != nullptr);
      const QVariantList tabs = host->property("tabs").toList();
      QVERIFY(!tabs.isEmpty());
      const auto selectTab = [&](const QString& title) {
        host->setProperty("activeTab", title);
        QQuickTest::qWaitForPolish(h.window);
        QTest::qWait(30);
      };
      qreal first = -1;
      for (const QVariant& t : tabs) {
        selectTab(t.toMap().value("title").toString());
        QQuickItem* scroll = visibleItem(h, "gameSettingsScroll");
        QVERIFY(scroll != nullptr);
        if (first < 0) first = scroll->height();
        QCOMPARE(scroll->height(), first);
      }
      const auto tabWith = [&](const QString& key) {
        for (const QVariant& t : tabs)
          for (const QVariant& r : t.toMap().value("rows").toList())
            if (r.toMap().value("key").toString() == key) return t.toMap().value("title").toString();
        return QString();
      };
      const QString speedTab = tabWith(QStringLiteral("uzume.speedup_ratio"));
      QVERIFY(!speedTab.isEmpty());
      selectTab(speedTab);
      QQuickItem* row = visibleItem(h, "gameOption_uzume.speedup_ratio");
      QVERIFY(row != nullptr);
      QVERIFY(row->property("meta").toString().startsWith(QStringLiteral("Uses your default")));
      gs->setFastForwardRatio(6.0);   // the header's speed menu: a live value, not stored
      QTRY_COMPARE(visibleItem(h, "gameOption_uzume.speedup_ratio")->property("current").toString(), QStringLiteral("6"));
      QVERIFY(visibleItem(h, "gameOption_uzume.speedup_ratio")->property("meta").toString().startsWith(QStringLiteral("Set in the game now")));
      gs->setFastForwardRatio(2.0);
      QTRY_VERIFY(visibleItem(h, "gameOption_uzume.speedup_ratio")->property("meta").toString().startsWith(QStringLiteral("Uses your default")));
      const QString layoutTab = tabWith(QStringLiteral("uzume.screen_layout"));
      if (!layoutTab.isEmpty()) {   // two-screen systems only
        selectTab(layoutTab);
        ctl->chooseScreenLayout(QStringLiteral("side"));
        QTRY_COMPARE(visibleItem(h, "gameOption_uzume.screen_layout")->property("current").toString(), QStringLiteral("side"));
        QVERIFY(visibleItem(h, "gameOption_uzume.screen_layout")->property("meta").toString().startsWith(QStringLiteral("Set in the game now")));
        ctl->chooseScreenLayout(QStringLiteral("stacked"));
      }
      selectTab(tabs.first().toMap().value("title").toString());
    }
    QVERIFY(h.click("gameSettingsDone"));
    QTRY_VERIFY(!shown(h, "gameSettingsDialog"));
  }

  // The title keeps its full width while there is room; only the text next to it (share state) gives way.
  void headerTitleKeepsItsWidth() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false, QStringLiteral("Pokémon Omega Ruby Special Edition"));
    resize(h, 1440, 900);
    QQuickItem* title = visibleItem(h, "gameTitle");
    QVERIFY(title != nullptr);
    // (the very wide font of the widefont run leaves no room for the whole title, the header then elides it by design)
    if (qEnvironmentVariableIsSet("UZUME_TEST_WIDE_FONT")) QSKIP("the whole title does not fit the header at the wide font");
    QVERIFY2(title->width() >= title->implicitWidth() - 0.5, qPrintable(QStringLiteral("title %1 of %2").arg(title->width()).arg(title->implicitWidth())));
  }

  // Header (56 px) with a long title: ← Library, title (elided, full title as tooltip), Layout, Multiview, Panel, Fullscreen, Pause as
  // the primary, "⋯"; it fits at 1280 and with wide fonts (text size TV, letter spacing).
  void headerFitsALongTitleAndWideFonts() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    const QString longTitle = QStringLiteral("Coral Saga: The Long Tide of the Northern Archipelago");
    startPreview(h, false, longTitle);
    resize(h, 1280, 800);
    const auto check = [&](const QString& tag) {
      QCOMPARE(rectOf(h, "gameHeader").height(), 56.0);
      const QString problem = zonesProblem(h, zoneNames());
      QVERIFY2(problem.isEmpty(), qPrintable(tag + QStringLiteral(": ") + problem));
      QVERIFY2(shown(h, "backToLibraryButton") && shown(h, "gameTitle") && shown(h, "pauseButton") && shown(h, "moreButton"), qPrintable(tag));
      QQuickItem* title = visibleItem(h, "gameTitle");
      QCOMPARE(textOf(title), longTitle);
      QCOMPARE(tooltipOf(title), longTitle);   // the full title, also where it is elided
      QVERIFY2(sceneRect(title).right() <= sceneRect(visibleItem(h, "pauseButton")).left() + 0.5, qPrintable(tag + QStringLiteral(": title runs into Pause")));
    };
    check(QStringLiteral("100 %"));
    // Order and content of the header at 1280: Layout, Multiview, Panel, Fullscreen, Pause, "⋯" from left to right
    if (shown(h, "layoutSwitch") && shown(h, "tabMultiview") && shown(h, "panelToggle") && shown(h, "fullscreenButton")) {   // not at the very wide font of the widefont run
      QQuickItem* fs = visibleItem(h, "fullscreenButton");
      QCOMPARE(tooltipOf(fs), QStringLiteral("Fullscreen · F11"));
      QCOMPARE(textOf(visibleItem(h, "pauseButton")), QStringLiteral("❚❚ Pause"));
      QCOMPARE(visibleItem(h, "pauseButton")->property("kind").toString(), QStringLiteral("primary"));   // the one yellow button
      QCOMPARE(visibleItem(h, "moreButton")->property("kind").toString(), QString());
      QVERIFY(sceneRect(visibleItem(h, "panelToggle")).right() <= sceneRect(fs).left() + 0.5);
      QVERIFY(sceneRect(fs).right() <= sceneRect(visibleItem(h, "pauseButton")).left() + 0.5);
    }
    if (shown(h, "fullscreenButton")) {
      // The same as F11: the game screen goes fullscreen (the header goes away); the test puts it back
      QQuickItem* screen = visibleItem(h, "gameHeader");
      while (screen != nullptr && !QString::fromLatin1(screen->metaObject()->className()).startsWith(QLatin1String("GameScreen"))) screen = screen->parentItem();
      QVERIFY(screen != nullptr && !screen->property("fullscreen").toBool());
      QVERIFY(h.click("fullscreenButton"));
      QTRY_VERIFY(screen->property("fullscreen").toBool());
      screen->setProperty("fullscreen", false);
      QTRY_VERIFY(shown(h, "gameHeader"));
    }
    h.controller->setTextSize(QStringLiteral("tv"));
    resize(h, 1280, 800);
    check(QStringLiteral("tv text size"));
    widenFonts(h, 6.0);
    check(QStringLiteral("tv text size, letters spaced"));
    h.controller->setTextSize(QStringLiteral("100"));
    resize(h, 1280, 800);
    check(QStringLiteral("100 % again"));
    // A function that left the header for narrowness is in the menu: Fullscreen then is listed
    if (!shown(h, "fullscreenButton")) {
      QVERIFY(h.click("moreButton"));
      QTRY_VERIFY(shown(h, "menuItem_fullscreen"));
      QTest::keyClick(h.window, Qt::Key_Escape);
    }
  }

  // Pause is the primary: Resume while paused; the menu has Diagnostics, Screenshot, Hotkeys, Reset game and Report a problem.
  void pauseIsThePrimaryAndTheMenuIsShort() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    resize(h, 1440, 900);
    GameSession* gs = h.controller->gameSession();
    QCOMPARE(visibleItem(h, "pauseButton")->property("kind").toString(), QStringLiteral("primary"));
    QVERIFY(h.click("pauseButton"));
    QTRY_VERIFY(gs->isPaused());
    QTRY_COMPARE(textOf(visibleItem(h, "pauseButton")), QStringLiteral("▶ Resume"));
    QVERIFY(h.click("pauseButton"));
    QTRY_VERIFY(!gs->isPaused());
    QVERIFY(h.click("moreButton"));
    QTRY_VERIFY(shown(h, "menuItem_diagnostics"));
    const char* menuOrder[] = {"menuItem_diagnostics", "menuItem_screenshot", "menuItem_hotkeys", "menuItem_gameSettings", "menuItem_report", "menuItem_reset"};
    for (const char* n : menuOrder) QVERIFY2(shown(h, n), n);
    // The menu lays its rows out after it opens (on the Windows runner a frame later), so wait for the order.
    const auto inOrder = [&]() {
      for (int i = 1; i < 6; ++i) {
        if (!(sceneRect(visibleItem(h, menuOrder[i - 1])).top() < sceneRect(visibleItem(h, menuOrder[i])).top())) return false;
      }
      return true;
    };
    QTRY_VERIFY2(inOrder(), "menu order: Diagnostics, Screenshot, Hotkeys, Settings, Report, Reset");
    QCOMPARE(textOf(visibleItem(h, "menuItem_diagnostics")), QStringLiteral("Diagnostics"));
    QCOMPARE(textOf(visibleItem(h, "menuItem_report")), QStringLiteral("Report a problem…"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "menuItem_diagnostics"));
  }

  // The hold-Select sheet lists Diagnostics, and the gamepad hotkey toggles the diagnostics block like F3.
  void holdSelectSheetListsDiagnosticsAndTogglesIt() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    resize(h, 1280, 800);
    ControllersController* c = h.controller->controllers();
    bool listed = false;
    for (const QVariant& v : c->padSheetRows()) listed = listed || v.toMap().value(QStringLiteral("action")).toString() == QLatin1String("diagnostics");
    QVERIFY2(listed, "the sheet rows carry the diagnostics action");
    // The sheet is created while the hotkey button is held; the test holds it open by forcing the loader (the grid reads the same rows).
    QQuickItem* loader = h.item("padHotkeySheetLoader");
    QVERIFY(loader != nullptr);
    loader->setProperty("active", true);
    QTRY_VERIFY(h.item("padHotkeySheet") != nullptr);
    QQuickItem* sheet = h.item("padHotkeySheet");
    QVariantList keys = sheet->property("keys").toList();
    QString label;
    for (const QVariant& k : keys) {
      const QVariantList kv = k.toList();
      if (kv.size() >= 2 && kv.at(1).toString() == QLatin1String("Diagnostics")) label = kv.at(1).toString();
    }
    QCOMPARE(label, QStringLiteral("Diagnostics"));
    loader->setProperty("active", false);
    DiagnosticsModel* diag = h.controller->sessions()->diagnostics();
    QVERIFY(!diag->isOpen());
    emit c->padHotkeyTriggered(QStringLiteral("diagnostics"));
    QTRY_VERIFY(diag->isOpen());
    QTRY_VERIFY(shown(h, "diagnosticsOverlay"));
    emit c->padHotkeyTriggered(QStringLiteral("diagnostics"));
    QTRY_VERIFY(!diag->isOpen());
  }

  void singleDiagnosticsToggleAndOverlayPosition() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    SessionController* ctl = h.controller->sessions();
    DiagnosticsModel* diag = ctl->diagnostics();
    QVERIFY(!diag->isOpen());
    QVERIFY(!shown(h, "diagnosticsOverlay"));

    for (const QSize sz : {QSize(1440, 900), QSize(1280, 800), QSize(960, 600)}) {
      resize(h, sz.width(), sz.height());
      QVERIFY(clickMore(h, "diagnostics"));
      QTRY_VERIFY(diag->isOpen());
      QQuickTest::qWaitForPolish(h.window);
      QTRY_VERIFY(shown(h, "diagnosticsOverlay"));
      const QString problem = overlayProblem(h);
      QVERIFY2(problem.isEmpty(), qPrintable(QStringLiteral("%1 px: ").arg(sz.width()) + problem));
      QVERIFY(!shown(h, "diagDetails"));   // no session: no Details section (and no empty rows)
      // F3 hides it again, the menu entry reads "Hide diagnostics" meanwhile
      QVERIFY(h.click("moreButton"));
      QTRY_VERIFY(shown(h, "menuItem_diagnostics"));
      QCOMPARE(textOf(visibleItem(h, "menuItem_diagnostics")), QStringLiteral("Hide diagnostics"));
      QTest::keyClick(h.window, Qt::Key_Escape);
      QTRY_VERIFY(!shown(h, "menuItem_diagnostics"));
      QTest::keyClick(h.window, Qt::Key_F3);
      QTRY_VERIFY(!diag->isOpen());
    }
    // Section state is global (ADR 0014 D5): collapsing Emulation is kept when the overlay is reopened
    diag->setOpen(true);
    diag->setEmulationOpen(false);
    diag->setOpen(false);
    diag->setOpen(true);
    QVERIFY(!diag->emulationOpen());
    diag->setEmulationOpen(true);
    diag->setOpen(false);
  }

  // Fullscreen: the speed-up indicator (toast, bottom left) never overlaps the diagnostics overlay (top right).
  void speedUpIndicatorDoesNotOverlapDiagnosticsOverlay() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    GameSession* gs = h.controller->gameSession();
    DiagnosticsModel* diag = h.controller->sessions()->diagnostics();
    resize(h, 1440, 900);
    QTest::keyClick(h.window, Qt::Key_F11);
    QQuickTest::qWaitForPolish(h.window);
    gs->setFastForward(true);
    QTRY_VERIFY(shown(h, "fastForwardIndicator"));
    const QRectF alone = rectOf(h, "fastForwardIndicator");
    QVERIFY(!shown(h, "diagnosticsOverlay"));

    diag->setOpen(true);
    QTRY_VERIFY(shown(h, "diagnosticsOverlay") && shown(h, "fastForwardIndicator"));
    QQuickTest::qWaitForPolish(h.window);
    QRectF ov = rectOf(h, "diagnosticsOverlay"), ff = rectOf(h, "fastForwardIndicator");
    QVERIFY2(!ov.intersects(ff), "speed-up indicator overlaps the diagnostics overlay");
    // Toasts sit bottom left, the diagnostics top right: the indicator stays where it was
    QCOMPARE(ff.left(), alone.left());
    QVERIFY(ff.center().x() < rectOf(h, "playArea").center().x() && ff.center().y() > rectOf(h, "playArea").center().y());
    QVERIFY(ov.center().x() > rectOf(h, "playArea").center().x() && ov.center().y() < rectOf(h, "playArea").center().y());

    diag->setOpen(false);
    QTRY_VERIFY(!shown(h, "diagnosticsOverlay"));
    QTRY_COMPARE(rectOf(h, "fastForwardIndicator").left(), alone.left());
    gs->setFastForward(false);
    QTest::keyClick(h.window, Qt::Key_F11);
  }

  // ---- Multiview ----

  void multiviewHeaderTilesPanelAndPicker() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    GameSession* gs = h.controller->gameSession();
    SessionController* ctl = h.controller->sessions();
    resize(h, 1440, 900);
    QVERIFY(clickHeader(h, "tabMultiview", "multiview"));
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(ctl->tab(), QStringLiteral("multiview"));

    // Multiview with only your game: RunHeader "Multiview", "Add tile", Leave; the picker is closed and does not open on its own
    QVERIFY(shown(h, "addSessionToggle") && shown(h, "leaveMultiview"));
    QCOMPARE(textOf(visibleItem(h, "multiviewTitle")), QStringLiteral("Multiview"));
    if (shown(h, "multiSub")) QVERIFY(textOf(visibleItem(h, "multiSub")).endsWith(QStringLiteral("1 person")));  // optional when the header is tight
    QVERIFY(!shown(h, "multiviewSessionList") && !shown(h, "pickerPopover"));
    QCOMPARE(textOf(visibleItem(h, "addSessionToggle")), QStringLiteral("Add tile"));
    QCOMPARE(textOf(visibleItem(h, "panelTileLabel")), QStringLiteral("TILE 1 · YOUR GAME"));
    // Pause/Resume of your game from the Multiview header (the button, or the "..." menu when the header is tight)
    const auto togglePause = [&]() { return shown(h, "pauseButton") ? h.click("pauseButton") : clickMore(h, "pause"); };
    QVERIFY(togglePause());
    QTRY_VERIFY(gs->isPaused());
    QVERIFY(togglePause());
    QTRY_VERIFY(!gs->isPaused());

    // Anchored picker: opens only on click, under the button, closes on a click on the anchor
    QVERIFY(h.click("addSessionToggle"));
    QTRY_VERIFY(shown(h, "multiviewSessionList"));
    {
      const QRectF anchor = rectOf(h, "addSessionToggle");
      QQuickItem* pop = visibleItem(h, "multiviewListTitle");
      QVERIFY(pop != nullptr);
      const QRectF pr = sceneRect(pop);
      QVERIFY2(pr.top() >= anchor.bottom() - 0.5, qPrintable(QStringLiteral("picker top %1, anchor bottom %2").arg(pr.top()).arg(anchor.bottom())));
      QVERIFY(pr.right() <= h.window->width() + 0.5);
      QVERIFY(visibleItem(h, "addSessionToggle")->property("on").toBool());  // the anchor stays pressed
    }
    QVERIFY(h.click("addSessionToggle"));
    QTRY_VERIFY(!shown(h, "multiviewSessionList"));
    QTRY_VERIFY(!visibleItem(h, "addSessionToggle")->property("on").toBool());

    // Add Lena (picker), then pick a layout
    QVERIFY(h.click("addSessionToggle"));
    QVERIFY(h.clickWhenReady("multiviewAddButton_s1"));  // the picker content appears after the click, not with it
    QTRY_COMPARE(ctl->surfaceCount(), 2);
    QVERIFY(h.click("addSessionToggle"));   // close the picker (click on its anchor)
    QTRY_VERIFY(!shown(h, "multiviewSessionList"));
    QVERIFY(clickMode(h, "modeRow"));
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("row"));
    QVERIFY(!shown(h, "emptyTile_3") && !shown(h, "emptyTile_4"));  // no empty slots (3r-3)

    // Tile selection: your tile first, keys 1 and 2 select, the panel follows
    QCOMPARE(ctl->selectedSurface(), QStringLiteral("local"));
    QVERIFY(shown(h, "tileSelection_local") && !shown(h, "tileSelection_s1"));
    QCOMPARE(textOf(visibleItem(h, "panelTileLabel")), QStringLiteral("TILE 1 · YOUR GAME"));
    QVERIFY(shown(h, "endGameButton") && shown(h, "panelShareState") && !shown(h, "remotePanel"));
    QTest::keyClick(h.window, Qt::Key_2);
    QTRY_COMPARE(ctl->selectedSurface(), QStringLiteral("s1"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(shown(h, "tileSelection_s1") && !shown(h, "tileSelection_local"));
    QCOMPARE(textOf(visibleItem(h, "panelTileLabel")), QStringLiteral("TILE 2 · WATCHING"));
    QVERIFY(shown(h, "remotePanel") && !shown(h, "endGameButton") && !shown(h, "panelShareState"));
    QCOMPARE(textOf(visibleItem(h, "remoteWho")), QStringLiteral("Lena"));
    QCOMPARE(textOf(visibleItem(h, "remoteGame")), QStringLiteral("Harbor Rally"));
    QCOMPARE(textOf(visibleItem(h, "audioButton_s1")), QStringLiteral("Audio here"));
    QVERIFY(shown(h, "removeButton_s1") && shown(h, "selectLocalTile"));
    // Selecting a remote tile moves neither audio nor input; the game stays controllable from the header
    QCOMPARE(ctl->audioFocus(), QStringLiteral("local"));
    QVERIFY(shown(h, "pauseButton") || shown(h, "moreButton"));
    // The audio tile shows the AUDIO tag
    QVERIFY(shown(h, "audioChip_local") && !shown(h, "audioChip_s1"));
    QVERIFY(h.click("audioButton_s1"));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s1"));
    QTRY_VERIFY(shown(h, "audioChip_s1") && !shown(h, "audioChip_local"));
    QCOMPARE(textOf(visibleItem(h, "audioButton_s1")), QStringLiteral("Audio plays from this tile"));
    // "Select" in the remote panel goes back to your tile
    QVERIFY(h.click("selectLocalTile"));
    QCOMPARE(ctl->selectedSurface(), QStringLiteral("local"));
    // A click on a tile's margin selects it, too
    {
      QQuickItem* t = visibleItem(h, "surfaceTile_s1");
      const QPointF p = t->mapToScene(QPointF(t->width() / 2, 4));
      QTest::mouseClick(h.window, Qt::LeftButton, Qt::NoModifier, p.toPoint());
      QTRY_COMPARE(ctl->selectedSurface(), QStringLiteral("s1"));
    }
    ctl->selectSurface(QStringLiteral("local"));

    // Header and diagnostics in the Multiview at 1440, 1280 and 960 px
    for (const QSize sz : {QSize(1440, 900), QSize(1280, 800), QSize(960, 600)}) {
      resize(h, sz.width(), sz.height());
      const QString tag = QStringLiteral("multiview %1 px: ").arg(sz.width());
      const QString problem = zonesProblem(h, zoneNames());
      QVERIFY2(problem.isEmpty(), qPrintable(tag + problem));
      QVERIFY2(rectOf(h, "gamePanel").right() <= h.window->width() + 0.5, qPrintable(tag + QStringLiteral("panel outside the window")));
      QVERIFY2(shown(h, "modeSegment") || shown(h, "moreButton"), qPrintable(tag + QStringLiteral("no multiview mode control")));
      QVERIFY(shown(h, "moreButton") && shown(h, "addSessionToggle") && shown(h, "leaveMultiview"));
      QQuickItem* title = visibleItem(h, "multiviewTitle");
      QVERIFY2(title != nullptr && title->width() > 8, qPrintable(tag + QStringLiteral("no title in the Multiview header")));
      ctl->diagnostics()->setOpen(true);
      QQuickTest::qWaitForPolish(h.window);
      QVERIFY2(overlayProblem(h).isEmpty(), qPrintable(tag + overlayProblem(h)));
      QVERIFY(!shown(h, "diagnosticsPanel"));  // no bottom bar
      ctl->diagnostics()->setOpen(false);
    }

    // Panel in the rail: the tile badge stays; expanding restores the panel
    resize(h, 1440, 900);
    QVERIFY(clickHeader(h, "panelToggle", "panel"));
    QTRY_COMPARE(rectOf(h, "gamePanel").width(), 56.0);
    QVERIFY(shown(h, "panelExpandButton"));
    QVERIFY(h.click("panelExpandButton"));
    QTRY_COMPARE(rectOf(h, "gamePanel").width(), 320.0);

    // Remove from multiview in the panel of the remote tile
    ctl->selectSurface(QStringLiteral("s1"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.click("removeButton_s1"));
    QTRY_COMPARE(ctl->surfaceCount(), 1);
    QCOMPARE(ctl->selectedSurface(), QStringLiteral("local"));
  }

  // ---- watching only ----

  // R-P4: the header segments are reachable with Tab, show a focus ring, Left / Right move and pick, Enter / Space pick the focused option.
  void headerSegmentsWorkWithTheKeyboard() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    SessionController* ctl = h.controller->sessions();
    resize(h, 1440, 900);
    QQuickItem* multiBtn = visibleItem(h, "tabMultiview");
    if (multiBtn == nullptr && qEnvironmentVariableIsSet("UZUME_TEST_WIDE_FONT")) {   // the wide font moved Multiview into "⋯"
      QVERIFY(clickHeader(h, "tabMultiview", "multiview"));
    } else {
      QVERIFY(multiBtn != nullptr);
      QVERIFY(multiBtn->activeFocusOnTab());
      multiBtn->forceActiveFocus();
      QVERIFY(multiBtn->hasActiveFocus());
      QTest::keyClick(h.window, Qt::Key_Space);   // the focused header button is activated
    }
    QTRY_COMPARE(ctl->tab(), QStringLiteral("multiview"));
    QQuickTest::qWaitForPolish(h.window);
    // In the Multiview the mode segment (two surfaces) moves with Left / Right
    ctl->watch(QStringLiteral("s1"));
    QTRY_COMPARE(ctl->surfaceCount(), 2);
    QQuickTest::qWaitForPolish(h.window);
    QQuickItem* seg = visibleItem(h, "modeSegment");
    if (seg == nullptr && qEnvironmentVariableIsSet("UZUME_TEST_WIDE_FONT")) {   // the wide font moved the mode switch into "⋯"
      QVERIFY(clickMore(h, "modeRow"));
      QCOMPARE(ctl->multiviewMode(), QStringLiteral("row"));
      QVERIFY(clickMore(h, "modePip"));
      QCOMPARE(ctl->multiviewMode(), QStringLiteral("pip"));
      return;
    }
    QVERIFY(seg != nullptr);
    QVERIFY(seg->activeFocusOnTab());
    seg->forceActiveFocus();
    QVERIFY(seg->hasActiveFocus());
    QQuickTest::qWaitForPolish(h.window);
    bool ring = false;
    for (QQuickItem* c : seg->childItems()) ring = ring || (c->isVisible() && QByteArray(c->metaObject()->className()).contains("UzFocusRing"));
    QVERIFY2(ring, "the focused segment shows a focus ring");
    ctl->setMultiviewMode(QStringLiteral("pip"));
    QTest::keyClick(h.window, Qt::Key_Right);
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("row"));
    QTest::keyClick(h.window, Qt::Key_Left);
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("pip"));
    QTest::keyClick(h.window, Qt::Key_Left);   // already the first option: stays
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("pip"));
  }

  void watchingOnlyHeader() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    SessionController* ctl = h.controller->sessions();
    QTRY_COMPARE(ctl->sessions().size(), 2);
    ctl->watch(QStringLiteral("s1"));
    QTRY_VERIFY(ctl->watching());
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("game"));
    for (const QSize sz : {QSize(1440, 900), QSize(1280, 800), QSize(960, 600)}) {
      resize(h, sz.width(), sz.height());
      const QString problem = zonesProblem(h, zoneNames());
      QVERIFY2(problem.isEmpty(), qPrintable(QStringLiteral("watching %1 px: ").arg(sz.width()) + problem));
      QVERIFY(shown(h, "leaveButton") && !shown(h, "pauseButton") && !shown(h, "fastForwardButton") && !shown(h, "tabMultiview"));
      QVERIFY(shown(h, "moreButton"));   // F3 diagnostics and the help entries live in the "..." menu
      QVERIFY(!shown(h, "quickMenu"));   // nothing of yours to pause: no quick menu
    }
    QCOMPARE(textOf(visibleItem(h, "gameTitle")), QStringLiteral("Lena · Harbor Rally"));
    QVERIFY(h.click("leaveButton"));
    QTRY_VERIFY(!ctl->watching());
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));
  }

  // ---- sidebar: Now running ----

  // The row of the quick menu that has the keyboard focus ("" = none of the rows, e.g. Resume or Quit game).
  static QString focusedQuickRow(Harness& h) {
    for (const char* n : {"snapshot", "saves", "screenshot", "gameSettings", "reset", "speedup", "sound", "screens", "fullscreen", "diagnostics",
                          "share", "multiview", "hotkeys", "report"}) {
      QQuickItem* it = h.item((QByteArray("quickRow_") + n).constData());
      if (it != nullptr && it->hasActiveFocus()) return QString::fromLatin1(n);
    }
    return {};
  }

  // Esc always pauses and opens the quick menu (ADR 0026 D1, board 7-3), in a window and in fullscreen; F11 leaves fullscreen.
  void escPausesAndOpensTheQuickMenu() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    GameSession* gs = h.controller->gameSession();
    resize(h, 1440, 900);
    QVERIFY(!gs->isPaused());
    QVERIFY(!shown(h, "quickMenu"));

    // Window: Esc pauses and shows the menu with Resume focused; Esc again resumes
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(gs->isPaused());
    QTRY_VERIFY(shown(h, "quickMenu") && shown(h, "quickMenuResume"));
    QTRY_VERIFY(h.item("quickMenuResume")->hasActiveFocus());
    uitest::saveShot(h.window, QStringLiteral("7-3-quick-menu"));
    QCOMPARE(textOf(visibleItem(h, "quickMenuTitle")), QStringLiteral("Quick menu"));
    QCOMPARE(textOf(visibleItem(h, "quickMenuLabel")), QStringLiteral("Paused · Lumen Drift"));
    QVERIFY(textOf(visibleItem(h, "quickMenuStatus")).contains(QStringLiteral("Nobody is watching.")));
    QCOMPARE(rectOf(h, "quickMenu").width(), 720.0);
    QCOMPARE(textOf(visibleItem(h, "quickMenuResume")), QStringLiteral("▶ Resume"));
    QCOMPARE(visibleItem(h, "quickMenuResume")->property("kind").toString(), QStringLiteral("primary"));
    QVERIFY(!visibleItem(h, "quickRow_fullscreen")->property("row").toMap().value("label").toString().startsWith(QStringLiteral("Leave")));
    {
      // The menu covers the stage only: the header and the panel stay
      const QRectF menu = rectOf(h, "quickMenu"), play = rectOf(h, "playArea");
      QVERIFY(play.contains(menu.adjusted(0.5, 0.5, -0.5, -0.5)));
      QVERIFY(shown(h, "gameHeader") && shown(h, "gamePanel"));
      QCOMPARE(textOf(visibleItem(h, "pauseButton")), QStringLiteral("▶ Resume"));   // the header reads Resume as well
    }
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!gs->isPaused());
    QTRY_VERIFY(!shown(h, "quickMenu"));

    // Fullscreen: Esc pauses (it does not leave), the menu offers "Leave fullscreen" in the Play group, F11 leaves
    QTest::keyClick(h.window, Qt::Key_F11);
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(gs->isPaused());
    QTRY_VERIFY(shown(h, "quickMenu") && shown(h, "quickMenuResume"));
    QCOMPARE(visibleItem(h, "quickRow_fullscreen")->property("row").toMap().value("label").toString(), QStringLiteral("Leave fullscreen"));
    QVERIFY(h.click("quickMenuResume"));
    QTRY_VERIFY(!gs->isPaused());
    QTRY_VERIFY(!shown(h, "quickMenu"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(gs->isPaused());
    QVERIFY(h.click("quickRow_fullscreen"));
    QTRY_VERIFY(!shown(h, "fullscreenToolbar"));
    QCOMPARE(visibleItem(h, "quickRow_fullscreen")->property("row").toMap().value("label").toString(), QStringLiteral("Fullscreen"));
    QVERIFY(gs->isPaused());  // leaving fullscreen does not resume
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!gs->isPaused());
  }

  // The quick menu is a keyboard and pad menu: arrows move, Enter chooses, Esc resumes; the rows with a value open the header's menus
  // at the row (Esc closes those first and does not resume); "Quit game…" does what the panel's Quit does.
  void quickMenuIsReachableWithTheKeyboard() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, true);
    startPreview(h, true);
    GameSession* gs = h.controller->gameSession();
    resize(h, 1440, 900);
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(gs->isPaused());
    QTRY_VERIFY(h.item("quickMenuResume")->hasActiveFocus());
    // All four groups with their rows are there, in two columns
    for (const char* n : {"snapshot", "saves", "screenshot", "gameSettings", "reset", "speedup", "sound", "screens", "fullscreen", "diagnostics",
                          "share", "multiview", "hotkeys", "report"})
      QVERIFY2(shown(h, (QByteArray("quickRow_") + n).constData()), n);
    QVERIFY(rectOf(h, "quickRow_reset").left() < rectOf(h, "quickRow_speedup").left());
    QVERIFY(rectOf(h, "quickRow_share").left() == rectOf(h, "quickRow_reset").left() && rectOf(h, "quickRow_hotkeys").left() == rectOf(h, "quickRow_speedup").left());
    QCOMPARE(visibleItem(h, "quickRow_reset")->property("row").toMap().value("danger").toBool(), true);
    QCOMPARE(visibleItem(h, "quickRow_speedup")->property("row").toMap().value("value").toString(), QStringLiteral("Off · 2×"));
    QCOMPARE(visibleItem(h, "quickRow_share")->property("row").toMap().value("value").toString(), QStringLiteral("Not shared"));

    // Down leaves Resume for the first row of the left column, Right / Left switch the column, Up goes back to Resume
    QTest::keyClick(h.window, Qt::Key_Down);
    const QString first = focusedQuickRow(h);
    QVERIFY2(first == QLatin1String("snapshot") || first == QLatin1String("saves"), qPrintable(first));
    QTest::keyClick(h.window, Qt::Key_Right);
    QCOMPARE(focusedQuickRow(h), QStringLiteral("speedup"));
    QTest::keyClick(h.window, Qt::Key_Down);
    QCOMPARE(focusedQuickRow(h), QStringLiteral("sound"));
    QTest::keyClick(h.window, Qt::Key_Left);
    QVERIFY(!focusedQuickRow(h).isEmpty() && focusedQuickRow(h) != QLatin1String("sound"));
    QTest::keyClick(h.window, Qt::Key_Right);
    QCOMPARE(focusedQuickRow(h), QStringLiteral("sound"));

    // Enter on "Sound" opens the sound popover at the row; Esc closes it first, the game stays paused with the menu open
    QTest::keyClick(h.window, Qt::Key_Return);
    QTRY_VERIFY(shown(h, "soundVolumeSlider"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "soundVolumeSlider"));
    QVERIFY(gs->isPaused() && shown(h, "quickMenu"));
    // ... and so does the speed menu of "Speed-up"
    QVERIFY(h.click("quickRow_speedup"));
    QTRY_VERIFY(shown(h, "menuItem_speed_3"));
    QVERIFY(h.click("menuItem_speed_3"));
    QCOMPARE(gs->fastForwardRatio(), 3.0);
    QTRY_COMPARE(visibleItem(h, "quickRow_speedup")->property("row").toMap().value("value").toString(), QStringLiteral("Off · 3×"));
    gs->setFastForwardRatio(2.0);
    // Enter on a row with a key runs it: Diagnostics toggles the F3 block
    QVERIFY(!h.controller->sessions()->diagnostics()->isOpen());
    visibleItem(h, "quickRow_diagnostics")->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(h.window, Qt::Key_Return);
    QTRY_VERIFY(h.controller->sessions()->diagnostics()->isOpen());
    QTest::keyClick(h.window, Qt::Key_Return);
    QTRY_VERIFY(!h.controller->sessions()->diagnostics()->isOpen());

    // Up from the first row of a column goes to Resume; Enter on Resume resumes
    visibleItem(h, "quickRow_speedup")->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(h.window, Qt::Key_Up);
    QTRY_VERIFY(h.item("quickMenuResume")->hasActiveFocus());
    QTest::keyClick(h.window, Qt::Key_Return);
    QTRY_VERIFY(!gs->isPaused());
    QTRY_VERIFY(!shown(h, "quickMenu"));

    // Settings for this game… opens the dialog over the menu; Done goes back to the menu, which stays paused
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(shown(h, "quickMenu"));
    QVERIFY(h.click("quickRow_gameSettings"));
    QTRY_VERIFY(shown(h, "gameSettingsDialog"));
    QVERIFY(h.click("gameSettingsDone"));
    QTRY_VERIFY(!shown(h, "gameSettingsDialog"));
    QVERIFY(gs->isPaused() && shown(h, "quickMenu"));
    QTRY_VERIFY(!focusedQuickRow(h).isEmpty() || h.item("quickMenuResume")->hasActiveFocus());   // the focus is back in the menu
    // Reset game… asks first; Esc cancels and the menu is still there
    QVERIFY(h.click("quickRow_reset"));
    QTRY_VERIFY(shown(h, "resetConfirm"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "resetConfirm"));
    QVERIFY(gs->isPaused() && shown(h, "quickMenu"));

    // Quit game… saves and returns to the Library, like the panel's Quit game
    QVERIFY(shown(h, "quickMenuQuit"));
    QVERIFY(h.click("quickMenuQuit"));
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("library"));
    QCOMPARE(gs->state(), GameSession::Idle);
  }

  // The status line names the save state and who watches (and what they see).
  void quickMenuStatusNamesWhoWatches() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    GameSession* gs = h.controller->gameSession();
    resize(h, 1440, 900);
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(gs->isPaused());
    QTRY_VERIFY(shown(h, "quickMenuStatus"));
    QCOMPARE(textOf(visibleItem(h, "quickMenuStatus")), QStringLiteral("Nobody is watching."));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!gs->isPaused());
  }

  // Header values (board 7-1): Speed-up says Off / On with its speed, Sound its percent, Screens the layout, Panel Shown / Hidden; every
  // control is a framed button (no frameless icon buttons), the toggles that are on are the ink plane.
  void headerControlsShowTheirValues() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    GameSession* gs = h.controller->gameSession();
    SessionController* ctl = h.controller->sessions();
    resize(h, 1920, 1080);
    // At wide fonts the bar gives way (words, then values): a value is then read from the button's accessible name or tooltip.
    const bool wide = qEnvironmentVariableIsSet("UZUME_TEST_WIDE_FONT");
    const auto accName = [&](const char* n) {
      QQuickItem* it = visibleItem(h, n);
      const QQmlProperty p(it, QStringLiteral("Accessible.name"), qmlContext(it));
      return p.isValid() ? p.read().toString() : QString();
    };
    const auto shownValue = [&](const char* n) {
      QQuickItem* it = visibleItem(h, n);
      if (it == nullptr) return QStringLiteral("<hidden>");
      const QString v = it->property("value").toString();
      return v;
    };
    const auto readsValue = [&](const char* n, const QString& v) {
      QQuickItem* it = visibleItem(h, n);
      if (it == nullptr) return false;
      return it->property("value").toString() == v || (wide && (accName(n).contains(v) || tooltipOf(it).contains(v)));
    };
    const auto valueOf = [&](const char* n) { return shownValue(n); };
    QVERIFY(textOf(visibleItem(h, "fastForwardButton")) == QStringLiteral("» Speed-up") || (wide && textOf(visibleItem(h, "fastForwardButton")) == QStringLiteral("»")));
    QVERIFY(readsValue("fastForwardButton", QStringLiteral("Off · 2×")));
    QVERIFY(!visibleItem(h, "fastForwardButton")->property("inverted").toBool());
    QVERIFY(h.click("fastForwardButton"));
    QTRY_VERIFY(readsValue("fastForwardButton", QStringLiteral("On · 2×")));
    QVERIFY(visibleItem(h, "fastForwardButton")->property("inverted").toBool());   // on = the ink plane
    QVERIFY(h.click("fastForwardButton"));
    QTRY_VERIFY(readsValue("fastForwardButton", QStringLiteral("Off · 2×")));
    gs->setFastForwardRatio(3.0);
    QTRY_VERIFY(readsValue("fastForwardButton", QStringLiteral("Off · 3×")));
    gs->setFastForwardRatio(2.0);
    // the chevron half opens the speeds
    if (shown(h, "speedValueButton")) {
      QVERIFY(h.click("speedValueButton"));
      QTRY_VERIFY(shown(h, "menuItem_speed_6"));
      QTest::keyClick(h.window, Qt::Key_Escape);
      QTRY_VERIFY(!shown(h, "menuItem_speed_6"));
    }
    QVERIFY(!gs->isPaused());

    QVERIFY(textOf(visibleItem(h, "volumeButton")) == QStringLiteral("Sound") || (wide && textOf(visibleItem(h, "volumeButton")).isEmpty()));   // the word goes last
    QCOMPARE(valueOf("volumeButton"), QStringLiteral("%1 %").arg(h.controller->audio()->volume()));
    h.controller->audio()->setVolume(5);
    QTRY_VERIFY(readsValue("volumeButton", QStringLiteral("5 %")));
    QVERIFY(h.click("volumeButton"));
    QTRY_VERIFY(shown(h, "soundVolumeSlider"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "soundVolumeSlider"));

    if (shown(h, "layoutSwitch")) {   // (the wide font moves Screens into "⋯", where it is listed with its value)
      QCOMPARE(textOf(visibleItem(h, "layoutSwitch")), QStringLiteral("Screens"));
      QVERIFY(readsValue("layoutSwitch", ctl->layoutLabel(ctl->screenLayout())));
      ctl->chooseScreenLayout(QStringLiteral("side"));
      QTRY_VERIFY(readsValue("layoutSwitch", ctl->layoutLabel(QStringLiteral("side"))));
      ctl->chooseScreenLayout(QStringLiteral("stacked"));
    } else {
      QVERIFY(h.click("moreButton"));
      QTRY_VERIFY(shown(h, "menuItem_screens"));
      QCOMPARE(visibleItem(h, "menuItem_screens")->property("hint").toString(), ctl->layoutLabel(ctl->screenLayout()));
      QTest::keyClick(h.window, Qt::Key_Escape);
      QTRY_VERIFY(!shown(h, "menuItem_screens"));
    }

    QCOMPARE(textOf(visibleItem(h, "panelToggle")), QStringLiteral("Panel"));
    QVERIFY(readsValue("panelToggle", QStringLiteral("Shown")));
    QVERIFY(visibleItem(h, "panelToggle")->property("inverted").toBool());
    QVERIFY(h.click("panelToggle"));
    QTRY_VERIFY(readsValue("panelToggle", QStringLiteral("Hidden")));
    QVERIFY(!visibleItem(h, "panelToggle")->property("inverted").toBool());
    QVERIFY(h.click("panelToggle"));
    QTRY_VERIFY(readsValue("panelToggle", QStringLiteral("Shown")));

    if (!wide) QCOMPARE(textOf(visibleItem(h, "fullscreenButton")), QStringLiteral("⛶ Fullscreen"));
    QCOMPARE(textOf(visibleItem(h, "pauseButton")), QStringLiteral("❚❚ Pause"));
    QVERIFY(readsValue("pauseButton", QStringLiteral("Esc")));
    QVERIFY(visibleItem(h, "moreButton")->property("outlined").toBool());
    if (shown(h, "speedValueButton")) QVERIFY(visibleItem(h, "speedValueButton")->property("outlined").toBool());
    uitest::saveShot(h.window, QStringLiteral("7-1-control-bar"));
    // The sub line under the title: share state (and the save state when known)
    QVERIFY(textOf(visibleItem(h, "sharePill")).startsWith(QStringLiteral("Not shared")));
    QVERIFY(rectOf(h, "sharePill").top() >= rectOf(h, "gameTitle").bottom() - 1.0);
    // The bar keeps its order: Speed-up, Sound, Screens, Panel, Fullscreen, Pause, ⋯ (all there at 1920 px; wide fonts move some into "⋯")
    if (!wide) {
      QVERIFY(rectOf(h, "speedValueButton").right() <= rectOf(h, "volumeButton").left() + 0.5);
      QVERIFY(rectOf(h, "volumeButton").right() <= rectOf(h, "layoutSwitch").left() + 0.5);
      QVERIFY(rectOf(h, "layoutSwitch").right() <= rectOf(h, "panelToggle").left() + 0.5);
      QVERIFY(rectOf(h, "panelToggle").right() <= rectOf(h, "fullscreenButton").left() + 0.5);
    }

    // Narrow: words and values give way, then Screens and Fullscreen move into "⋯" (with their value); Pause never moves
    resize(h, 960, 600);
    QVERIFY(shown(h, "pauseButton") && shown(h, "moreButton"));
    QVERIFY2(rectOf(h, "gameTitle").width() >= qMin<qreal>(160.0, visibleItem(h, "gameTitle")->implicitWidth()) - 1.0, "the title keeps its room");
    if (!shown(h, "layoutSwitch")) {
      QVERIFY(h.click("moreButton"));
      QTRY_VERIFY(shown(h, "menuItem_screens"));
      QVERIFY(h.item("menuItem_screens") != nullptr);
      QTest::keyClick(h.window, Qt::Key_Escape);
    }
    if (!shown(h, "fullscreenButton")) {
      QVERIFY(h.click("moreButton"));
      QTRY_VERIFY(shown(h, "menuItem_fullscreen"));
      QTest::keyClick(h.window, Qt::Key_Escape);
      QTRY_VERIFY(!shown(h, "menuItem_fullscreen"));
    }
  }

  // The Game tab of the panel (board 7-5): Now rows, the four framed buttons, Sharing, Quit pinned at the bottom; no numbered labels.
  void gameTabHasNowRowsAndButtons() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    FakeCore core;
    startLiveGame(hub, h, &core);
    resize(h, 1440, 900);
    SaveHistoryController* hist = h.controller->saveHistory();
    QVERIFY(shown(h, "nowBlock"));
    QCOMPARE(textOf(visibleItem(h, "panelTabLabel")), QStringLiteral("Game"));
    QVERIFY(visibleItem(h, "panelTab_game")->property("open").toBool());
    QCOMPARE(rectOf(h, "panelTabBar").height(), 48.0);
    for (const char* n : {"nowRow_save", "nowRow_slot", "nowRow_watching"}) QVERIFY2(shown(h, n), n);
    QVERIFY(textOf(visibleItem(h, "nowValue_save")).startsWith(QStringLiteral("Saved ")));
    QVERIFY(textOf(visibleItem(h, "nowValue_save")).contains(QStringLiteral("synced")) || textOf(visibleItem(h, "nowValue_save")).contains(QStringLiteral("syncing"))
            || textOf(visibleItem(h, "nowValue_save")).contains(QStringLiteral("waiting")));
    QCOMPARE(textOf(visibleItem(h, "nowValue_slot")), hist->slotLabel());
    QCOMPARE(textOf(visibleItem(h, "nowValue_watching")), QStringLiteral("Not shared"));
    QVERIFY(shown(h, "nowSaveMark"));
    // The four buttons in a 2 x 2 grid of 36 px, each with its key
    const QRectF snap = rectOf(h, "gameSnapshotButton"), shot = rectOf(h, "gameScreenshotButton"), share = rectOf(h, "gameShareButton"), saves = rectOf(h, "manageSavesButton");
    QCOMPARE(snap.height(), 36.0);
    if (qEnvironmentVariableIsSet("UZUME_TEST_WIDE_FONT") && qAbs(snap.top() - shot.top()) >= 1) {
      // A label would elide in half the width: the grid is one column
      QVERIFY(shot.top() > snap.bottom() - 0.5 && share.top() > shot.bottom() - 0.5 && saves.top() > share.bottom() - 0.5);
      QVERIFY(qAbs(snap.left() - shot.left()) < 1 && qAbs(snap.left() - saves.left()) < 1);
    } else {
      QVERIFY(qAbs(snap.top() - shot.top()) < 1 && qAbs(share.top() - saves.top()) < 1 && share.top() > snap.bottom() - 0.5);
      QVERIFY(snap.left() < shot.left() && qAbs(snap.left() - share.left()) < 1);
    }
    QCOMPARE(textOf(visibleItem(h, "gameSnapshotButton")), QStringLiteral("Snapshot"));
    QCOMPARE(visibleItem(h, "gameSnapshotButton")->property("value").toString(), QStringLiteral("F5"));
    QCOMPARE(textOf(visibleItem(h, "gameScreenshotButton")), QStringLiteral("Screenshot"));
    QCOMPARE(visibleItem(h, "gameScreenshotButton")->property("value").toString(), QStringLiteral("F12"));
    QCOMPARE(textOf(visibleItem(h, "gameShareButton")), QStringLiteral("Share…"));
    QCOMPARE(textOf(visibleItem(h, "manageSavesButton")), QStringLiteral("Saves…"));
    // Sharing follows; the section labels carry no numbers; Quit is pinned below with its sentence
    QVERIFY(rectOf(h, "panelShareState").top() > saves.bottom());
    for (QQuickItem* l : h.items("sectionLabelText")) QVERIFY2(!l->isVisible() || !l->property("text").toString().contains(QStringLiteral(" — ")), qPrintable(l->property("text").toString()));
    QCOMPARE(textOf(visibleItem(h, "quitNote")), QStringLiteral("Saves and syncs first, then returns to the Library."));
    QVERIFY(rectOf(h, "endGameButton").top() > rectOf(h, "panelShareState").bottom());
    uitest::saveShot(h.window, QStringLiteral("7-5-game-tab"));
    // "Share…" scrolls to Sharing, "Saves…" opens the saves view in the panel
    QVERIFY(h.click("gameShareButton"));
    QVERIFY(h.click("manageSavesButton"));
    QTRY_VERIFY(shown(h, "savesView"));
  }

  // GameSettingsContent (board 7-6) works on its own, without a dialog frame and for a game that is not running: stored values only.
  void gameSettingsContentEmbedsStandalone() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, true);
    QQmlComponent comp(h.engine.get());
    comp.setData("import QtQuick\nimport Uzume.App\nGameSettingsContent { objectName: \"standaloneSettings\"; width: 520; z: 50 }", QUrl(QStringLiteral("qrc:/test/standalone.qml")));
    QVERIFY2(comp.isReady(), qPrintable(comp.errorString()));
    QObject* obj = comp.createWithInitialProperties({{QStringLiteral("player"), QVariant::fromValue(h.controller.get())}});
    QVERIFY(obj != nullptr);
    auto* content = qobject_cast<QQuickItem*>(obj);
    QVERIFY(content != nullptr);
    content->setParentItem(h.window->contentItem());
    QVERIFY(!h.controller->gameSession()->isActive());
    QVERIFY(h.controller->runningGameId().isEmpty());
    QMetaObject::invokeMethod(content, "showGame", Q_ARG(QString, QStringLiteral("g1")));
    QTRY_VERIFY(!content->property("tabs").toList().isEmpty());
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(content->property("gameId").toString(), QStringLiteral("g1"));
    QVERIFY(!content->property("runningHere").toBool());
    QVERIFY(content->isVisible() && content->width() == 520);
    // No dialog around it: no dialog frame or scrim exists for it, the footer is there, the live note is not
    QVERIFY(!shown(h, "dialogFrame") && !shown(h, "gameSettingsDialog"));
    QVERIFY(shown(h, "gameSettingsScroll") && shown(h, "gameSettingsFooter") && !shown(h, "gameSettingsLive"));
    const QVariantList tabs = content->property("tabs").toList();
    QString speedTab;
    for (const QVariant& t : tabs)
      for (const QVariant& r : t.toMap().value("rows").toList())
        if (r.toMap().value("key").toString() == QLatin1String("uzume.speedup_ratio")) speedTab = t.toMap().value("title").toString();
    QVERIFY(!speedTab.isEmpty());
    content->setProperty("activeTab", speedTab);
    QQuickTest::qWaitForPolish(h.window);
    QQuickItem* row = visibleItem(h, "gameOption_uzume.speedup_ratio");
    QVERIFY(row != nullptr);
    QVERIFY(row->property("meta").toString().startsWith(QStringLiteral("Uses your default")));
    // A stored value shows with its source; no live value (the game is not running)
    h.controller->emulation()->setGameOption(QStringLiteral("g1"), QStringLiteral("uzume.speedup_ratio"), QStringLiteral("3"));
    QTRY_VERIFY(visibleItem(h, "gameOption_uzume.speedup_ratio")->property("meta").toString().startsWith(QStringLiteral("Changed for this game")));
    QCOMPARE(visibleItem(h, "gameOption_uzume.speedup_ratio")->property("current").toString(), QStringLiteral("3"));
    QCOMPARE(content->property("changedCount").toInt(), 1);
    h.controller->emulation()->resetGameOption(QStringLiteral("g1"), QStringLiteral("uzume.speedup_ratio"));
    QTRY_VERIFY(visibleItem(h, "gameOption_uzume.speedup_ratio")->property("meta").toString().startsWith(QStringLiteral("Uses your default")));
    delete content;
  }

  // 0.19 package 6: F7 opens the layout menu (layouts, Swap screens, Bottom screen in its own window), Shift+F7 swaps, Select + Right
  // cycles; every change toasts; the bottom-screen window is a second top-level window that closes with the game.
  void layoutMenuHotkeysAndBottomWindow() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    SessionController* ctl = h.controller->sessions();
    resize(h, 1440, 900);
    const auto bottomWindows = [&]() {
      QList<QQuickWindow*> out;
      for (QWindow* w : QGuiApplication::allWindows()) {
        auto* qw = qobject_cast<QQuickWindow*>(w);
        if (qw && qw != h.window && qw->isVisible() && qw->title().contains(QStringLiteral("bottom screen"))) out.append(qw);
      }
      return out;
    };

    // F7 opens the menu below the Layout button; the layouts, Swap and the window item are in it.
    QVERIFY(!shown(h, "menuItem_layout_large"));
    QTest::keyClick(h.window, Qt::Key_F7);
    QTRY_VERIFY(shown(h, "menuItem_layout_large"));
    for (const char* n : {"menuItem_layout_stacked", "menuItem_layout_side", "menuItem_layout_top", "menuItem_layout_bottom", "menuItem_swap", "menuItem_bottomWindow"})
      QVERIFY2(shown(h, n), n);
    QVERIFY(h.click("menuItem_layout_large"));
    QCOMPARE(ctl->screenLayout(), QStringLiteral("large"));
    QTRY_VERIFY(shown(h, "layoutToast"));
    QCOMPARE(textOf(visibleItem(h, "layoutToast")), QStringLiteral("Layout: Large top / small bottom"));
    QTRY_VERIFY(!shown(h, "menuItem_layout_large"));

    // F7 again closes it (the game view has the focus again after the menu closed).
    QTest::keyClick(h.window, Qt::Key_F7);
    QTRY_VERIFY(shown(h, "menuItem_layout_large"));
    QTest::keyClick(h.window, Qt::Key_Escape);
    QTRY_VERIFY(!shown(h, "menuItem_layout_large"));
    QVERIFY(!h.controller->gameSession()->isPaused());  // Esc closed the menu, it did not pause

    // Shift+F7 swaps directly and toasts.
    QTest::keyClick(h.window, Qt::Key_F7, Qt::ShiftModifier);
    QTRY_VERIFY(ctl->screenSwap());
    QTRY_COMPARE(textOf(visibleItem(h, "layoutToast")), QStringLiteral("Screens swapped"));
    QTest::keyClick(h.window, Qt::Key_F7, Qt::ShiftModifier);
    QTRY_VERIFY(!ctl->screenSwap());

    // Gamepad Select + Right cycles through the layouts of the system and wraps.
    const QStringList all = ctl->watchLayouts();
    QCOMPARE(all.size(), 5);
    ctl->setScreenLayout(QStringLiteral("bottom"));
    emit h.controller->controllers()->padHotkeyTriggered(QStringLiteral("layout"));
    QTRY_COMPARE(ctl->screenLayout(), QStringLiteral("stacked"));
    emit h.controller->controllers()->padHotkeyTriggered(QStringLiteral("layout"));
    QTRY_COMPARE(ctl->screenLayout(), QStringLiteral("side"));
    QTRY_COMPARE(textOf(visibleItem(h, "layoutToast")), QStringLiteral("Layout: Side by side"));

    // Bottom screen in its own window: a second window, the main view shows the top screen only.
    QTest::keyClick(h.window, Qt::Key_F7);
    QTRY_VERIFY(shown(h, "menuItem_bottomWindow"));
    QVERIFY(h.click("menuItem_bottomWindow"));
    QVERIFY(ctl->bottomWindow());
    QTRY_COMPARE(bottomWindows().size(), 1);
    QCOMPARE(ctl->viewLayout(), QStringLiteral("top"));
    QCOMPARE(ctl->screenLayout(), QStringLiteral("side"));  // the chosen layout is kept for when the window closes
    QQuickWindow* second = bottomWindows().first();
    auto* bottomView = second->findChild<GameView*>(QStringLiteral("bottomScreenView"));
    QVERIFY(bottomView != nullptr);
    QVERIFY(bottomView->cpuOnly());
    QCOMPARE(bottomView->layout(), QStringLiteral("bottom"));
    QTRY_VERIFY(!bottomView->placement().targets.isEmpty() && !bottomView->placement().targets.at(1).isEmpty());
    // A touch there maps to the middle of the lower half of the frame.
    const auto touch = bottomView->touchTarget(bottomView->placement().targets.at(1).center(), false);
    QVERIFY(touch.has_value() && qAbs(touch->y() - 0.75) < 1e-6);
    // Closing the window turns the mode off again.
    second->close();
    QTRY_VERIFY(!ctl->bottomWindow());
    QTRY_VERIFY(bottomWindows().isEmpty());
    QCOMPARE(ctl->viewLayout(), QStringLiteral("side"));

    // Leaving the game closes it as well.
    ctl->chooseBottomWindow(true);
    QTRY_COMPARE(bottomWindows().size(), 1);
    ctl->gameEnded();
    QTRY_VERIFY(bottomWindows().isEmpty());
    QVERIFY(!ctl->bottomWindow());
  }

  // 0.19 audio stage 1: +/- change the volume by the step and show a replacing toast, M mutes, the header button opens the
  // Sound popover, "Sound settings" opens Settings > Audio & video with its rows.
  void volumeHotkeysToastPopoverAndSettings() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, false);
    startPreview(h, false);
    resize(h, 1440, 900);
    AudioController* audio = h.controller->audio();
    QCOMPARE(audio->volume(), 100);
    QVERIFY(!shown(h, "volumeToast"));

    QTest::keyClick(h.window, Qt::Key_Minus);
    QTRY_COMPARE(audio->volume(), 95);
    QTRY_VERIFY(shown(h, "volumeToast"));
    QCOMPARE(textOf(visibleItem(h, "volumeToast")), QStringLiteral("Volume 95 %"));
    audio->setVolumeStep(10);
    QTest::keyClick(h.window, Qt::Key_Minus);
    QTRY_COMPARE(audio->volume(), 85);
    QCOMPARE(textOf(visibleItem(h, "volumeToast")), QStringLiteral("Volume 85 %"));   // replaces itself
    QTest::keyClick(h.window, Qt::Key_Plus);
    QTRY_COMPARE(audio->volume(), 95);
    QTRY_VERIFY_WITH_TIMEOUT(!shown(h, "volumeToast"), 4000);                          // gone after 1.5 s

    QTest::keyClick(h.window, Qt::Key_M);
    QTRY_VERIFY(audio->muted());
    QTRY_COMPARE(textOf(visibleItem(h, "volumeToast")), QStringLiteral("Muted"));
    QCOMPARE(h.controller->gameSession()->audio().volume(), 0.0);   // the local output follows
    QTest::keyClick(h.window, Qt::Key_M);
    QTRY_VERIFY(!audio->muted());
    QTRY_COMPARE(textOf(visibleItem(h, "volumeToast")), QStringLiteral("Sound on · 95 %"));
    QCOMPARE(h.controller->gameSession()->audio().volume(), 0.95);
    QTest::keyClick(h.window, Qt::Key_M);   // muted; volume up unmutes
    QTRY_VERIFY(audio->muted());
    QTest::keyClick(h.window, Qt::Key_Plus);
    QTRY_VERIFY(!audio->muted());
    QCOMPARE(audio->volume(), 100);

    audio->setShowVolume(false);
    QTest::qWait(1700);
    QTest::keyClick(h.window, Qt::Key_Minus);
    QTRY_COMPARE(audio->volume(), 90);
    QVERIFY(!shown(h, "volumeToast"));
    audio->setShowVolume(true);

    // Header button -> Sound popover with the slider
    QVERIFY(shown(h, "volumeButton"));
    QVERIFY(h.click("volumeButton"));
    QTRY_VERIFY(shown(h, "soundVolumeSlider"));
    {   // the button's tooltip never shows while its popover is open (it sat over the close mark and caught clicks)
      QQuickItem* vb = visibleItem(h, "volumeButton");
      const QQmlProperty tipShown(vb, QStringLiteral("ToolTip.visible"), qmlContext(vb));
      QVERIFY(tipShown.isValid() && !tipShown.read().toBool());
    }
    QQuickItem* slider = visibleItem(h, "soundVolumeSlider");
    QVERIFY(slider != nullptr);
    QCOMPARE(slider->property("value").toInt(), 90);
    QVERIFY(h.click("soundMuteButton"));
    QTRY_VERIFY(audio->muted());
    QTRY_COMPARE(textOf(visibleItem(h, "soundMuteButton")), QStringLiteral("Unmute"));
    QVERIFY(h.click("soundMuteButton"));
    QTRY_VERIFY(!audio->muted());
    QVERIFY(textOf(visibleItem(h, "soundBackgroundLine")).contains(QStringLiteral("on")));

    // Sound settings -> Settings, Audio & video section
    QVERIFY(h.click("soundSettingsButton"));
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("settings"));
    QTRY_VERIFY(shown(h, "audioOutputSelect") && shown(h, "audioMuteBackgroundToggle") && shown(h, "audioVolumeStepSegment") && shown(h, "audioShowVolumeToggle"));
    QVERIFY(h.click("audioStep10"));
    QTRY_COMPARE(audio->volumeStep(), 10);
    QVERIFY(h.click("audioStep5"));
    QTRY_COMPARE(audio->volumeStep(), 5);
    QVERIFY(audio->muteInBackground());
    QVERIFY(h.click("audioMuteBackgroundToggle"));
    QTRY_VERIFY(!audio->muteInBackground());
    QVERIFY(h.item("audioOutputSelect") != nullptr);
  }

  void sidebarNowRunningStrip() {
    FakeHub hub(QStringLiteral("a"));
    Harness h;
    pair(hub, h, true);
    PlayerController* c = h.controller.get();
    GameSession* gs = c->gameSession();
    QVERIFY(!shown(h, "nowRunningStrip"));
    startPreview(h, true);
    c->leaveGameView();  // "← Library": paused, kept loaded
    QCOMPARE(c->screen(), QStringLiteral("library"));
    QTRY_VERIFY(gs->isPaused());
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(shown(h, "nowRunningStrip"));
    QCOMPARE(textOf(visibleItem(h, "nowRunningTitle")), QStringLiteral("Lumen Drift"));
    QCOMPARE(textOf(visibleItem(h, "nowRunningStatus")), QStringLiteral("Paused"));
    QVERIFY(shown(h, "nowRunningResume") && shown(h, "nowRunningQuit"));
    // Above the Hub switcher card, inside the sidebar, on every page
    for (const char* page : {"library", "emulation", "controllers", "settings"}) {
      if (QLatin1String(page) == QLatin1String("emulation")) c->showEmulation();
      else if (QLatin1String(page) == QLatin1String("controllers")) c->showControllers();
      else if (QLatin1String(page) == QLatin1String("settings")) c->showSettings();
      else c->showLibrary();
      QQuickTest::qWaitForPolish(h.window);
      QQuickItem* strip = visibleItem(h, "nowRunningStrip");
      QVERIFY2(strip != nullptr, page);
      QQuickItem* sidebar = strip->parentItem();
      for (; sidebar != nullptr && QString::fromLatin1(sidebar->metaObject()->className()).left(8) != QLatin1String("UzAppNav"); sidebar = sidebar->parentItem()) {}
      QVERIFY2(sidebar != nullptr, page);
      const QRectF sr = sceneRect(strip), br = sceneRect(sidebar);
      QVERIFY2(sr.left() >= br.left() && sr.right() <= br.right() && sr.bottom() <= br.bottom(), qPrintable(QLatin1String(page) + chain(strip)));
      QCOMPARE(sr.width(), 200.0);
    }
    c->showLibrary();
    resize(h, 960, 600);
    QQuickItem* strip = visibleItem(h, "nowRunningStrip");
    QVERIFY(strip != nullptr);
    QVERIFY2(sceneRect(strip).bottom() <= h.window->height(), qPrintable(chain(strip)));
    resize(h, 1280, 800);

    // Resume: back in the game, strip gone
    QVERIFY(h.click("nowRunningResume"));
    QCOMPARE(c->screen(), QStringLiteral("game"));
    QTRY_VERIFY(!gs->isPaused());
    c->leaveGameView();
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(shown(h, "nowRunningStrip"));
    // Quit: "Saving and syncing…" for a moment, then the game and the strip are gone
    QVERIFY(h.click("nowRunningQuit"));
    QVERIFY(c->quitting());
    QTRY_COMPARE(textOf(visibleItem(h, "nowRunningStatus")), QStringLiteral("Saving and syncing…"));
    QVERIFY(!shown(h, "nowRunningResume") && !shown(h, "nowRunningQuit"));
    QTRY_VERIFY(!gs->isActive());
    QVERIFY(!c->quitting());
    QTRY_VERIFY(!shown(h, "nowRunningStrip"));
  }
};

UITEST_MAIN(InGameUiTest)
#include "ingame_ui_test.moc"
