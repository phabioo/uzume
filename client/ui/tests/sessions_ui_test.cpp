// UI: sessions in the library, watching, Session panel, multiview, diagnostics (offscreen, FakeHub, no real network).
// Tests with a running game need the DS core and the homebrew test ROM (QSKIP without UZUME_MELONDS_DS_CORE; ui_sessions_testcore runs them against the Uzume test core).
#include <functional>

#include <QFile>
#include <QQmlContext>
#include <QQmlProperty>
#include <QSignalSpy>
#include <QtTest>

#include "fakehub.h"
#include "testsupport.h"

// Waits on real I/O, WebRTC, the encoder or the core must survive a loaded CI runner: every QTRY_* of this file
// waits up to 15 s (it still returns as soon as the condition holds, so passing runs are not slower).
#undef QTRY_VERIFY
#undef QTRY_VERIFY2
#undef QTRY_COMPARE
#define QTRY_VERIFY(expr) QTRY_VERIFY_WITH_TIMEOUT(expr, 15000)
#define QTRY_VERIFY2(expr, msg) QTRY_VERIFY2_WITH_TIMEOUT(expr, msg, 15000)
#define QTRY_COMPARE(actual, expected) QTRY_COMPARE_WITH_TIMEOUT(actual, expected, 15000)

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;

namespace {
const QString kViewerId = QStringLiteral("11111111-1111-4111-8111-111111111111");  // fixed by FakeHub join

QJsonObject sessionObj(const QString& id, const QString& who, const QString& game, const QString& vis, bool invited = false,
                       int minutesAgo = 5, bool owner = false) {
  return {{QStringLiteral("session_id"), id},
          {QStringLiteral("game_id"), QStringLiteral("t1")},
          {QStringLiteral("game_title"), game},
          {QStringLiteral("owner"), QJsonObject{{QStringLiteral("user_id"), QStringLiteral("u_x")},
                                                {QStringLiteral("display_name"), who},
                                                {QStringLiteral("device_name"), QStringLiteral("PC")}}},
          {QStringLiteral("visibility"), vis},
          {QStringLiteral("created_at"), QDateTime::currentDateTimeUtc().addSecs(-60 * minutesAgo).toString(Qt::ISODate)},
          {QStringLiteral("viewer_count"), 0},
          {QStringLiteral("is_owner"), owner},
          {QStringLiteral("invited"), invited}};
}

void pair(Harness& h, FakeHub& hub) {
  h.controller->addHub(hub.address());
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsTrustConfirmation, 8000);
  h.controller->confirmTrust();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), HubConnection::State::NeedsPairing, 8000);
  h.controller->requestPairing();
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("library"), 8000);
  QTRY_COMPARE_WITH_TIMEOUT(h.controller->sessions()->hubLink(), QStringLiteral("online"), 8000);
}

void prepareHub(FakeHub& hub) {
  hub.hubId = QStringLiteral("hub-sessions");
  hub.name = QStringLiteral("Home");
  hub.decision = FakeHub::Decision::Approve;
  hub.features = {QStringLiteral("saves_v1"), QStringLiteral("sessions_v1")};
}

bool anyVisible(Harness& h, const char* name) {
  for (QQuickItem* i : h.items(name)) {
    if (i->isVisible()) return true;
  }
  return false;
}
QQuickItem* visibleItem(Harness& h, const char* name) {
  for (QQuickItem* i : h.items(name)) {
    if (i->isVisible()) return i;
  }
  return nullptr;
}
QString textOf(QQuickItem* i) { return i ? i->property("text").toString() : QString(); }
// Every visible action button of the panel (Invite / Remove / Withdraw) must lie inside the panel's scene rect:
// a row wider than the panel (seen on Windows CI without fonts) would clip the button and make it unclickable.
void collectByPrefix(QQuickItem* root, const QString& prefix, QList<QQuickItem*>& out) {
  for (QQuickItem* c : root->childItems()) {
    if (c->objectName().startsWith(prefix) && c->isVisible()) out.append(c);
    collectByPrefix(c, prefix, out);
  }
}
// Failure diagnostics: every direct child of a layout ancestor with its implicit width and Layout.minimumWidth
// (the largest minimum decides the width every cell is laid out with, see SessionPanel.qml), one level deeper for nested layouts.
void dumpLayoutChildren(QQuickItem* layout, int depth = 0) {
  for (QQuickItem* c : layout->childItems()) {
    QVariant minW;
    if (QQmlContext* ctx = qmlContext(c)) {
      const QQmlProperty p(c, QStringLiteral("Layout.minimumWidth"), ctx);
      if (p.isValid()) minW = p.read();
    }
    qInfo().noquote() << QString(depth * 2 + 2, QLatin1Char(' ')) + "[layout-child]" << c->metaObject()->className() << c->objectName()
                      << "visible" << c->isVisible() << "w" << c->width() << "implicitW" << c->implicitWidth()
                      << "Layout.minimumWidth" << (minW.isValid() ? minW.toString() : QStringLiteral("n/a"));
    if (depth < 2 && qstrcmp(c->metaObject()->className(), "QQuickRowLayout") == 0) dumpLayoutChildren(c, depth + 1);
  }
}

QString panelButtonsOutside(Harness& h) {
  QQuickItem* panel = h.item("gamePanel");
  if (panel == nullptr) return QStringLiteral("no gamePanel");
  const QRectF pr = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
  QString bad;
  for (const char* prefix : {"invite_", "remove_", "withdraw_"}) {
    QList<QQuickItem*> found;
    collectByPrefix(h.window->contentItem(), QString::fromLatin1(prefix), found);
    for (QQuickItem* b : std::as_const(found)) {
      if (b->objectName() == QLatin1String("inviteBlock") || b->objectName() == QLatin1String("inviteField")) continue;
      const QRectF r = b->mapRectToScene(QRectF(0, 0, b->width(), b->height()));
      if (r.right() > pr.right() - 1 || r.left() < pr.left()) {
        for (QQuickItem* a = b->parentItem(); a != nullptr && a != panel; a = a->parentItem()) {
          qInfo().noquote() << "[layout]" << b->objectName() << "ancestor" << a->metaObject()->className() << a->objectName() << "w" << a->width() << "implicitW" << a->implicitWidth();
          if (qstrcmp(a->metaObject()->className(), "QQuickColumnLayout") == 0 || qstrcmp(a->metaObject()->className(), "QQuickRowLayout") == 0) dumpLayoutChildren(a);
        }
        bad += QStringLiteral("%1 [%2..%3] outside panel [%4..%5]; ").arg(b->objectName()).arg(r.left()).arg(r.right()).arg(pr.left()).arg(pr.right());
      }
    }
  }
  return bad;
}

int countContaining(const FakeHub& hub, const QString& part, const QByteArray& method = {}) {
  int n = 0;
  for (const FakeRequest& r : hub.requests) {
    if (r.path.contains(part) && (method.isEmpty() || r.method == method)) ++n;
  }
  return n;
}
QJsonObject lastBody(const FakeHub& hub, const QByteArray& method, const QString& pathPrefix) {
  for (qsizetype i = hub.requests.size() - 1; i >= 0; --i) {
    const QString& path = hub.requests.at(i).path;
    // A prefix ending in '/' matches sub-paths (PATCH /sessions/<id>); otherwise the path must match exactly.
    if (hub.requests.at(i).method == method && (pathPrefix.endsWith(QLatin1Char('/')) ? path.startsWith(pathPrefix) : path == pathPrefix)) {
      return QJsonDocument::fromJson(hub.requests.at(i).body).object();
    }
  }
  return {};
}
// Scene rect of the visible surface tile of a surface ("local" or a Session id) and of the multiview area.
QRectF tileRect(Harness& h, const QString& id) {
  QQuickItem* t = visibleItem(h, qPrintable(QStringLiteral("surfaceTile_") + id));
  return t ? t->mapRectToScene(QRectF(0, 0, t->width(), t->height())) : QRectF();
}
QRectF areaRect(Harness& h) {
  QQuickItem* a = h.item("multiviewArea");
  return a ? a->mapRectToScene(QRectF(0, 0, a->width(), a->height())) : QRectF();
}
bool approxEqual(qreal a, qreal b) { return std::abs(a - b) < 1.5; }

// Picks a Multiview mode through the header: the segment, or (collapsed when the header is tight, e.g. wide fonts) its cycle button.
bool clickMode(Harness& h, const char* modeName) {
  QQuickItem* seg = h.item(modeName);
  if (seg != nullptr && seg->isVisible()) return h.click(modeName);
  const QString want = QString::fromLatin1(modeName).mid(4).toLower();   // "modeRow" -> "row"
  QQuickItem* cycle = h.item("modeCycle");
  if (cycle == nullptr || !cycle->isVisible()) {   // absurdly wide fonts: the header has no room for the mode control at all
    h.controller->sessions()->setMultiviewMode(want);
    return h.controller->sessions()->multiviewMode() == want;
  }
  for (int i = 0; i < 4 && h.controller->sessions()->multiviewMode() != want; ++i)
    if (!h.click("modeCycle")) return false;
  return h.controller->sessions()->multiviewMode() == want;
}

// A function of the header's "..." menu (diagnostics, fullscreen, reset ...): opens the menu and picks the entry.
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

QString audibleTile(Harness& h, const QStringList& ids) {  // the surfaces with the audio chip
  QStringList on;
  for (const QString& id : ids) {
    if (visibleItem(h, qPrintable(QStringLiteral("audioChip_") + id)) != nullptr || visibleItem(h, qPrintable(QStringLiteral("pipAudioChip_") + id)) != nullptr) on << id;
  }
  return on.join(QLatin1Char(','));
}
// The panel follows the selected tile: select it, then press its "Audio here" / "Remove from multiview".
bool selectAndClick(Harness& h, SessionController* ctl, const QString& surface, const char* buttonPrefix) {
  ctl->selectSurface(surface);
  QQuickTest::qWaitForPolish(h.window);
  return h.click(qPrintable(QLatin1String(buttonPrefix) + surface));
}
}  // namespace

class SessionsUiTest : public QObject {
  Q_OBJECT
  QString ownSessionId(const FakeHub& hub) const {
    for (const QJsonObject& s : hub.sessions) {
      if (s.value(QStringLiteral("is_owner")).toBool()) return s.value(QStringLiteral("session_id")).toString();
    }
    return {};
  }

  public:
  struct GameRig {
    FakeHub hub{QStringLiteral("a")};
    Harness h;
    QByteArray rom;
  };

  static bool startGame(GameRig& r, bool share) {
    if (qEnvironmentVariableIsEmpty("UZUME_MELONDS_DS_CORE")) {
      return false;
    }
    QFile f(QStringLiteral(FB_TEST_ROM_PATH));
    if (!f.open(QIODevice::ReadOnly)) {
      return false;
    }
    r.rom = f.readAll();
    const QString sha = uitest::sha256Hex(r.rom);
    prepareHub(r.hub);
    QJsonArray games;
    games.append(uitest::gameJson(QStringLiteral("t1"), QStringLiteral("Uzume Test"), sha, r.rom.size(), QStringLiteral("uzume_test.nds")));
    r.hub.games = QJsonObject{{QStringLiteral("games"), games}};
    r.hub.roms.insert(sha, r.rom);
    r.hub.fakeUsers = QJsonArray{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("u_test_1")}, {QStringLiteral("display_name"), QStringLiteral("Tester")}, {QStringLiteral("online"), true}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("u_sam")}, {QStringLiteral("display_name"), QStringLiteral("Sam")}, {QStringLiteral("online"), true}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("u_sarah")}, {QStringLiteral("display_name"), QStringLiteral("Sarah")}, {QStringLiteral("online"), false}}};
    r.hub.sessions.insert(QStringLiteral("s1"), sessionObj(QStringLiteral("s1"), QStringLiteral("Lena"), QStringLiteral("Harbor Rally"), QStringLiteral("hub_users")));
    if (!r.hub.start() || !r.h.start(true)) {
      return false;
    }
    pair(r.h, r.hub);
    if (QTest::currentTestFailed()) return false;
    if (!QTest::qWaitFor([&]() { return r.h.controller->libraryState() == QLatin1String("ready") && r.h.controller->selectedGameId() == QLatin1String("t1"); }, 20000)) {
      return false;
    }
    if (share) {
      r.h.controller->playAndShareSelected();
    } else {
      r.h.controller->playSelected();
    }
    return true;
  }

 private slots:
  void initTestCase() { uitest::installWarningCounter(); }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void handshakeReportsMediaCapabilitiesTruthfully() {
    FakeHub hub(QStringLiteral("a"));
    prepareHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QJsonObject video;
    for (const FakeRequest& r : hub.requests) {
      if (r.path.endsWith(QLatin1String("/handshake"))) {
        video = QJsonDocument::fromJson(r.body).object().value(QStringLiteral("video")).toObject();
      }
    }
    QVERIFY(video.value(QStringLiteral("h264_decode")).toBool());
    QVERIFY(video.value(QStringLiteral("h264_encode")).toBool());
    QVERIFY(!video.value(QStringLiteral("encoders")).toArray().isEmpty());
    // WSS: connected, hello with the device id, presence "online" announced.
    QTRY_VERIFY(hub.wsConnections >= 1);
    QTRY_VERIFY(!hub.wsReceived.isEmpty() && hub.wsReceived.first().value(QStringLiteral("type")).toString() == QLatin1String("hello"));
  }

  void legacyVisibilityFileIsMigrated() {
    Harness h;
    const QString legacy = h.dir.path() + QStringLiteral("/player-settings.json");
    QFile f(legacy);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"session_visibility\": \"private\"}");
    f.close();
    QVERIFY(h.start());
    QCOMPARE(h.controller->sessions()->visibility(), QStringLiteral("private"));
    QVERIFY(!QFile::exists(legacy));
    QCOMPARE(uzume::PlayerSettings(h.dir.path()).sessionVisibility(), QStringLiteral("private"));
  }

  void sessionsSectionRenderingAndActions() {
    FakeHub hub(QStringLiteral("a"));
    prepareHub(hub);
    hub.sessions.insert(QStringLiteral("s1"), sessionObj(QStringLiteral("s1"), QStringLiteral("Lena"), QStringLiteral("Harbor Rally"), QStringLiteral("hub_users"), false, 12));
    hub.sessions.insert(QStringLiteral("s2"), sessionObj(QStringLiteral("s2"), QStringLiteral("Jonas"), QStringLiteral("Lumen Drift"), QStringLiteral("invite_only"), true, 1));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SessionController* ctl = h.controller->sessions();
    QTRY_COMPARE(ctl->sessions().size(), 2);
    const QVariantList list = ctl->sessions();
    QCOMPARE(list.at(0).toMap().value(QStringLiteral("sessionId")).toString(), QStringLiteral("s2"));  // newest first
    QCOMPARE(list.at(0).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Jonas · Lumen Drift"));
    QCOMPARE(list.at(0).toMap().value(QStringLiteral("meta")).toString(), QStringLiteral("inviting you"));
    QCOMPARE(list.at(1).toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Lena · Harbor Rally"));
    QCOMPARE(list.at(1).toMap().value(QStringLiteral("meta")).toString(), QStringLiteral("Everyone here · for 12 min"));

    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.item("sessionsSection")->isVisible());
    QVERIFY(h.item("watch_s1") && h.item("watch_s1")->isVisible());
    QCOMPARE(textOf(h.item("watch_s1")), QStringLiteral("Watch Session"));
    QVERIFY(h.item("join_s2") && h.item("join_s2")->isVisible());
    QCOMPARE(textOf(h.item("join_s2")), QStringLiteral("Join"));
    QVERIFY(h.item("decline_s2") && h.item("decline_s2")->isVisible());
    QVERIFY(h.item("watch_s2") == nullptr);  // an invite shows Join/Decline, not Watch
    QVERIFY(h.item("decline_s1") == nullptr || !h.item("decline_s1")->isVisible());
    uitest::saveShot(h.window, QStringLiteral("p4-3c-sessions"));

    // Live updates: new Session, invite, ended, no_longer_visible
    hub.sendWs(QStringLiteral("session_update"), {{QStringLiteral("session"), sessionObj(QStringLiteral("s3"), QStringLiteral("Mia"), QStringLiteral("Paper Wizards"), QStringLiteral("hub_users"))}});
    QTRY_COMPARE(ctl->sessions().size(), 3);
    hub.sendWs(QStringLiteral("session_ended"), {{QStringLiteral("session_id"), QStringLiteral("s3")}, {QStringLiteral("reason"), QStringLiteral("ended")}});
    QTRY_COMPARE(ctl->sessions().size(), 2);
    hub.sendWs(QStringLiteral("session_ended"), {{QStringLiteral("session_id"), QStringLiteral("s1")}, {QStringLiteral("reason"), QStringLiteral("no_longer_visible")}});
    QTRY_COMPARE(ctl->sessions().size(), 1);
    hub.sessions.insert(QStringLiteral("s4"), sessionObj(QStringLiteral("s4"), QStringLiteral("Sam"), QStringLiteral("Stylus Knights"), QStringLiteral("invite_only"), true));
    hub.sendWs(QStringLiteral("session_invite"), {{QStringLiteral("session"), hub.sessions.value(QStringLiteral("s4"))}});
    QTRY_COMPARE(ctl->sessions().size(), 2);

    // A GET /sessions answer that is older than a live event must not undo the event: the request goes out, then
    // session_ended(s4) arrives, then the (stale) list that still contains s4 is answered.
    hub.sessions.remove(QStringLiteral("s1"));  // keep the fake Hub consistent with the events so far
    QCOMPARE(ctl->sessions().size(), 2);
    const int listRequests = countContaining(hub, QStringLiteral("/api/v1/sessions"), "GET");
    ctl->refreshSessions();
    emit ctl->socket()->sessionEnded(SessionEnded{QStringLiteral("s4"), QStringLiteral("ended")});
    QCOMPARE(ctl->sessions().size(), 1);
    QTRY_VERIFY(countContaining(hub, QStringLiteral("/api/v1/sessions"), "GET") > listRequests);  // request reached the Hub
    QTest::qWait(600);  // ... and its response (with s4) is delivered in this time
    QCOMPARE(ctl->sessions().size(), 1);
    // restore s4 for the steps below
    hub.sendWs(QStringLiteral("session_invite"), {{QStringLiteral("session"), hub.sessions.value(QStringLiteral("s4"))}});
    QTRY_COMPARE(ctl->sessions().size(), 2);

    // Decline
    QTRY_VERIFY(h.item("decline_s2") && h.item("decline_s2")->isVisible());
    QVERIFY(h.click("decline_s2"));
    QTRY_COMPARE(ctl->sessions().size(), 1);
    QCOMPARE(hub.count(QStringLiteral("/api/v1/sessions/s2/decline")), 1);

    // Empty: section hidden
    hub.sendWs(QStringLiteral("session_ended"), {{QStringLiteral("session_id"), QStringLiteral("s4")}, {QStringLiteral("reason"), QStringLiteral("ended")}});
    QTRY_COMPARE(ctl->sessions().size(), 0);
    QTRY_VERIFY(!h.item("sessionsSection")->isVisible());
  }

  void hubLinkLossShowsBannerAndRecovers() {
    FakeHub hub(QStringLiteral("a"));
    prepareHub(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SessionController* ctl = h.controller->sessions();
    ctl->socket()->setBackoffMs(100, 300);
    QVERIFY(!h.item("hubLinkBanner")->isVisible());
    hub.refuseWs = true;
    hub.closeWsClients();
    QTRY_VERIFY(ctl->hubLink() != QLatin1String("online"));
    QTRY_VERIFY(h.item("hubLinkBanner")->isVisible());
    // R-P3: the server line and the banner follow the link (OFFLINE while lost); the page banner explains it.
    QTRY_COMPARE(h.controller->hubLink(), QStringLiteral("offline"));
    QTRY_COMPARE(h.item("hubStatusDot")->property("status").toString(), QStringLiteral("offline"));
    QVERIFY(h.item("hubLinkBanner")->property("lost").toBool());
    hub.refuseWs = false;
    QTRY_COMPARE_WITH_TIMEOUT(ctl->hubLink(), QStringLiteral("online"), 10000);
    QTRY_VERIFY(!h.item("hubLinkBanner")->isVisible());
    QTRY_COMPARE(h.item("hubStatusDot")->property("status").toString(), QStringLiteral("online"));
    QVERIFY(!h.item("hubCardLinkNote")->isVisible());
  }

  void watchWithoutLocalGameShowsRemoteAlone() {
    FakeHub hub(QStringLiteral("a"));
    prepareHub(hub);
    hub.sessions.insert(QStringLiteral("s1"), sessionObj(QStringLiteral("s1"), QStringLiteral("Lena"), QStringLiteral("Harbor Rally"), QStringLiteral("hub_users")));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SessionController* ctl = h.controller->sessions();
    QTRY_COMPARE(ctl->sessions().size(), 1);
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.click("watch_s1"));
    QTRY_VERIFY(ctl->watching());
    QCOMPARE(hub.count(QStringLiteral("/api/v1/sessions/s1/join")), 1);
    QCOMPARE(h.controller->screen(), QStringLiteral("game"));
    QCOMPARE(ctl->tab(), QStringLiteral("multiview"));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s1"));  // only audible surface
    QCOMPARE(ctl->watchedWho(), QStringLiteral("Lena"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(visibleItem(h, "tabSession") == nullptr);  // no local game: no Session | Multiview choice, no game controls
    QVERIFY(visibleItem(h, "pauseButton") == nullptr && visibleItem(h, "resetButton") == nullptr);
    QVERIFY(visibleItem(h, "leaveButton") != nullptr);  // "Leave" instead
    QVERIFY(visibleItem(h, "remoteView") != nullptr);
    QCOMPARE(ctl->selectedSurface(), QStringLiteral("s1"));
    QVERIFY(visibleItem(h, "audioButton_s1") != nullptr);  // the panel of the selected remote tile
    QCOMPARE(textOf(visibleItem(h, "audioButton_s1")), QStringLiteral("Audio plays from this tile"));
    QCOMPARE(textOf(h.item("gameTitle")), QStringLiteral("Lena · Harbor Rally"));
    uitest::saveShot(h.window, QStringLiteral("p4-remote-alone"));

    // Remote frame arrives -> RemoteView redraws
    const quint64 nr = ctl->remoteFrameNumber(QStringLiteral("s1"));
    QImage img(256, 384, QImage::Format_RGB32);
    img.fill(QColor(10, 200, 30));
    QMetaObject::invokeMethod(ctl->viewer(QStringLiteral("s1")), "frameReady", Qt::DirectConnection, Q_ARG(QImage, img));
    QCOMPARE(ctl->remoteFrameNumber(QStringLiteral("s1")), nr + 1);
    QVERIFY(!ctl->remoteFrame(QStringLiteral("s1")).isNull());

    // Diagnostics (overlay toggled by the tab, 0.6): one participant per remote Session, "—" where unavailable
    SessionStats remote;
    remote.active = true;
    remote.fps = 59.9;
    remote.connectionType = QStringLiteral("direct (host)");
    remote.rttMs = 14.0;
    remote.videoBitrateKbps = 5800;
    remote.packetLossPercent = 0.1;
    remote.decoderName = QStringLiteral("h264");
    ctl->setStatsOverride(nullptr, {{QStringLiteral("s1"), remote}});
    QVERIFY(!ctl->diagnostics()->isOpen());
    QVERIFY(clickMore(h, "diagnostics"));  // the one toggle (the overlay replaces the old bottom panel)
    QVERIFY(ctl->diagnostics()->isOpen());
    QCOMPARE(ctl->tab(), QStringLiteral("multiview"));  // not a view of its own
    ctl->refreshDiagnostics();
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(visibleItem(h, "diagnosticsOverlay") != nullptr);
    QVERIFY(visibleItem(h, "diagnosticsPanel") == nullptr);
    auto firstParticipant = [&]() { return ctl->diagnostics()->streaming().at(0).toMap().value(QStringLiteral("participants")).toList().at(0).toMap(); };
    QCOMPARE(ctl->diagnostics()->participantCount(), 1);
    QCOMPARE(firstParticipant().value(QStringLiteral("pill")).toString(), QStringLiteral("Direct"));
    QCOMPARE(firstParticipant().value(QStringLiteral("line2")).toString(), QStringLiteral("Decoder h264 · H.264 · 5.8 Mbit/s · 59.9 fps"));
    QCOMPARE(firstParticipant().value(QStringLiteral("line3")).toString(), QStringLiteral("RTT 14 ms · Loss 0.1 %"));
    QVERIFY(ctl->diagnostics()->tiles().at(0).toMap().value(QStringLiteral("note")).toString().contains(QStringLiteral("not measured here")));
    remote.rttMs.reset();
    remote.packetLossPercent.reset();
    remote.connectionType = QStringLiteral("relay (udp)");
    ctl->setStatsOverride(nullptr, {{QStringLiteral("s1"), remote}});
    ctl->refreshDiagnostics();
    QCOMPARE(firstParticipant().value(QStringLiteral("line3")).toString(), QStringLiteral("RTT — · Loss —"));  // no TURN server in the FakeHub ack
    QCOMPARE(firstParticipant().value(QStringLiteral("pill")).toString(), QStringLiteral("Relayed (TURN)"));
    // Details (readable pass): decoder, bitrate and a row per person with path, RTT, loss and the reason for a relay in one sentence
    remote.rttMs = 64.0;
    remote.packetLossPercent = 1.2;
    remote.relayReason = RelayReason::DirectFailed;
    remote.width = 1024;
    remote.height = 768;
    ctl->setStatsOverride(nullptr, {{QStringLiteral("s1"), remote}});
    ctl->refreshDiagnostics();
    QQuickTest::qWaitForPolish(h.window);
    {
      QCOMPARE(firstParticipant().value(QStringLiteral("path")).toString(), QStringLiteral("Relay"));
      QCOMPARE(firstParticipant().value(QStringLiteral("rtt")).toString(), QStringLiteral("64 ms"));
      QCOMPARE(firstParticipant().value(QStringLiteral("loss")).toString(), QStringLiteral("1.2 %"));
      QVERIFY(firstParticipant().value(QStringLiteral("reason")).toString().startsWith(QStringLiteral("Relay because")));
      QTRY_VERIFY(visibleItem(h, "diagDetails") != nullptr);
      QVERIFY(visibleItem(h, "diagDetailsToggle") != nullptr);   // open by default
      QVERIFY(visibleItem(h, "diagDetailRows") != nullptr && visibleItem(h, "diagPeople") != nullptr);
      QVERIFY(visibleItem(h, "diagPath") != nullptr);
      QCOMPARE(textOf(visibleItem(h, "diagPath")), QStringLiteral("Relay"));
      QCOMPARE(textOf(visibleItem(h, "diagRtt")), QStringLiteral("64 ms"));
      QCOMPARE(textOf(visibleItem(h, "diagLoss")), QStringLiteral("1.2 %"));
      QVERIFY(textOf(visibleItem(h, "diagReason")).contains(QStringLiteral("no direct path")));
      QVariant text;
      QVERIFY(QMetaObject::invokeMethod(visibleItem(h, "diagnosticsOverlay"), "fullText", Q_RETURN_ARG(QVariant, text)));
      QVERIFY(text.toString().contains(QStringLiteral("Decoder h264")));
      const QVariantList rows = visibleItem(h, "diagnosticsOverlay")->property("detailRows").toList();
      QStringList labels;
      for (const QVariant& r : rows) labels << r.toList().at(0).toString();
      QVERIFY2(labels.contains(QStringLiteral("Decoder")) && labels.contains(QStringLiteral("Bitrate")) && labels.contains(QStringLiteral("Resolution")), qPrintable(labels.join(',')));
      // Collapsing the section hides the content, the header stays; the state is remembered
      QVERIFY(ctl->diagnostics()->streamingOpen());
      QVERIFY(h.click("diagDetailsToggle"));
      QTRY_VERIFY(!ctl->diagnostics()->streamingOpen());
      QTRY_VERIFY(visibleItem(h, "diagPeople") == nullptr && visibleItem(h, "diagDetailRows") == nullptr);
      QVERIFY(visibleItem(h, "diagDetailsToggle") != nullptr);
      QVERIFY(h.click("diagDetailsToggle"));
      QTRY_VERIFY(ctl->diagnostics()->streamingOpen());
      QTRY_VERIFY(visibleItem(h, "diagPeople") != nullptr);
    }
    uitest::saveShot(h.window, QStringLiteral("p4-diagnostics-remote"));
    ctl->setStatsOverride(nullptr, {});
    QVERIFY(clickMore(h, "diagnostics"));
    QVERIFY(!ctl->diagnostics()->isOpen());

    // Hub removes the viewer: closed with a short notice, back to the Library
    hub.sendWs(QStringLiteral("viewer_left"), {{QStringLiteral("session_id"), QStringLiteral("s1")}, {QStringLiteral("viewer_id"), kViewerId},
                                               {QStringLiteral("reason"), QStringLiteral("removed")}});
    QTRY_VERIFY(!ctl->watching());
    QCOMPARE(ctl->message(), QStringLiteral("You were removed from the Session."));
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));
    QTRY_VERIFY(h.item("sessionMessage")->isVisible());

    // Session ended while watching
    ctl->dismissMessage();
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.click("watch_s1"));
    QTRY_VERIFY(ctl->watching());
    hub.sendWs(QStringLiteral("session_ended"), {{QStringLiteral("session_id"), QStringLiteral("s1")}, {QStringLiteral("reason"), QStringLiteral("ended")}});
    QTRY_VERIFY(!ctl->watching());
    QCOMPARE(ctl->message(), QStringLiteral("The Session ended."));
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));

    // Leaving = DELETE viewer ("← Library")
    hub.sessions.insert(QStringLiteral("s5"), sessionObj(QStringLiteral("s5"), QStringLiteral("Lena"), QStringLiteral("Harbor Rally"), QStringLiteral("hub_users")));
    hub.sendWs(QStringLiteral("session_update"), {{QStringLiteral("session"), hub.sessions.value(QStringLiteral("s5"))}});
    QTRY_VERIFY(h.item("watch_s5") != nullptr);
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.click("watch_s5"));
    QTRY_VERIFY(ctl->watching());
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("game"));
    QQuickTest::qWaitForPolish(h.window);
    QTRY_VERIFY(h.item("backToLibraryButton")->isVisible());
    QVERIFY(h.click("backToLibraryButton"));
    QTRY_VERIFY(!ctl->watching());
    QTRY_VERIFY(hub.count(QStringLiteral("/api/v1/sessions/s5/viewers/")) >= 1);
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));
  }

  // ---- with a running game (real core) ----

  // A GET sent while a visibility PATCH is in flight (the withdraw's follow-up GET) can be served before the Hub
  // applied the PATCH and answered after the PATCH answer: it must not revert the visibility. Same for a ws update.
  void staleAnswerAfterPatchKeepsVisibility() {
    GameRig r;
    if (!startGame(r, true)) UITEST_SKIP_NO_CORE("core or test ROM not available");
    Harness& h = r.h;
    FakeHub& hub = r.hub;
    SessionController* ctl = h.controller->sessions();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("game"), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(ctl->shared(), 20000);
    QVERIFY(h.click("visInviteOnly"));
    QTRY_COMPARE(lastBody(hub, "PATCH", QStringLiteral("/api/v1/sessions/")).value(QStringLiteral("visibility")).toString(), QStringLiteral("invite_only"));
    QTRY_COMPARE(hub.sessions.value(ownSessionId(hub)).value(QStringLiteral("visibility")).toString(), QStringLiteral("invite_only"));

    hub.holdSessionGet = true;
    ctl->withdrawInvite(QStringLiteral("u_jonas"));
    // The follow-up GET is sent only when the DELETE answers. Wait until it is held (sent before the visibility change),
    // otherwise it could be sent after the PATCH and its answer would rightly be trusted.
    QTRY_COMPARE(hub.heldSessionGets(), 1);
    QVERIFY(h.click("visPrivate"));
    QCOMPARE(ctl->visibility(), QStringLiteral("private"));
    QTRY_COMPARE(hub.sessions.value(ownSessionId(hub)).value(QStringLiteral("visibility")).toString(), QStringLiteral("private"));
    // let the PATCH answer arrive (its request is answered before the held GET is released)
    QTRY_VERIFY(countContaining(hub, QStringLiteral("/invites/u_jonas"), "DELETE") == 1);
    hub.releaseHeldSessionGets(QStringLiteral("invite_only"));
    hub.holdSessionGet = false;
    // a later round trip on another connection: the held answer is processed by then at the latest
    ctl->searchUsers(QStringLiteral("x"));
    QTRY_VERIFY(countContaining(hub, QStringLiteral("/users"), "GET") >= 1);
    QCOMPARE(ctl->visibility(), QStringLiteral("private"));

    // a ws update emitted before the Hub applied the PATCH, arriving after its answer
    QJsonObject stale = hub.sessions.value(ownSessionId(hub));
    stale.insert(QStringLiteral("visibility"), QStringLiteral("invite_only"));
    hub.sendWs(QStringLiteral("session_update"), {{QStringLiteral("session"), stale}});
    // (ws updates arrive in order: once the following one is applied, the stale one has been processed)
    QJsonObject marker = hub.sessions.value(ownSessionId(hub));
    marker.insert(QStringLiteral("viewer_count"), 7);
    hub.sendWs(QStringLiteral("session_update"), {{QStringLiteral("session"), marker}});
    QTRY_COMPARE(ctl->viewerCount(), 7);
    QCOMPARE(ctl->visibility(), QStringLiteral("private"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(!h.item("inviteBlock")->isVisible());
  }

  void playAndShareThenPanelStates() {
    GameRig r;
    if (!startGame(r, true)) UITEST_SKIP_NO_CORE("core or test ROM not available");
    Harness& h = r.h;
    FakeHub& hub = r.hub;
    SessionController* ctl = h.controller->sessions();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("game"), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(ctl->shared(), 20000);
    QCOMPARE(lastBody(hub, "POST", QStringLiteral("/api/v1/sessions")).value(QStringLiteral("visibility")).toString(), QStringLiteral("hub_users"));
    QCOMPARE(lastBody(hub, "POST", QStringLiteral("/api/v1/sessions")).value(QStringLiteral("game_id")).toString(), QStringLiteral("t1"));
    // presence: in_game with the game id
    QTRY_VERIFY(std::any_of(hub.wsReceived.cbegin(), hub.wsReceived.cend(), [](const QJsonObject& e) {
      return e.value(QStringLiteral("type")).toString() == QLatin1String("presence_update") &&
             e.value(QStringLiteral("payload")).toObject().value(QStringLiteral("state")).toString() == QLatin1String("in_game") &&
             e.value(QStringLiteral("payload")).toObject().value(QStringLiteral("game_id")).toString() == QLatin1String("t1");
    }));
    QQuickTest::qWaitForPolish(h.window);

    // Shared, hub_users: pill, no invite block, Stop sharing
    QCOMPARE(ctl->tab(), QStringLiteral("session"));
    QVERIFY(h.item("sharePill")->isVisible());
    QCOMPARE(textOf(h.item("sharePill")), QStringLiteral("Shared · 0 watching"));
    QCOMPARE(ctl->viewerCount(), 0);
    QVERIFY(h.item("tabMultiview") && h.item("moreButton") && !anyVisible(h, "tabDiagnostics"));
    QVERIFY(h.item("visibilitySegment")->isVisible());
    QVERIFY(!h.item("inviteBlock")->isVisible());
    QCOMPARE(textOf(h.item("shareButton")), QStringLiteral("Stop sharing"));
    QVERIFY(textOf(h.item("visibilityHint")).contains(QStringLiteral("Viewers send no input")));
    QVERIFY(h.item("endGameButton")->isVisible());
    QCOMPARE(textOf(h.item("endGameButton")), QStringLiteral("Quit game"));
    uitest::saveShot(h.window, QStringLiteral("p4-3g-shared"));

    // A viewer joins: offer goes out through the Hub, the pill counts
    hub.sendWs(QStringLiteral("viewer_joined"), {{QStringLiteral("session_id"), ownSessionId(hub)}, {QStringLiteral("viewer_id"), QStringLiteral("v1")},
                                                 {QStringLiteral("display_name"), QStringLiteral("Lena")}, {QStringLiteral("device_name"), QStringLiteral("PC")}});
    QTRY_VERIFY_WITH_TIMEOUT(ctl->host()->hasViewer(QStringLiteral("v1")), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(hub.wsReceived.cbegin(), hub.wsReceived.cend(), [](const QJsonObject& e) {
      return e.value(QStringLiteral("type")).toString() == QLatin1String("signal") &&
             e.value(QStringLiteral("payload")).toObject().value(QStringLiteral("kind")).toString() == QLatin1String("offer");
    }), 20000);

    // Visibility "Invite only" (PATCH), persisted in the settings under the data dir
    QVERIFY(h.click("visInviteOnly"));
    QTRY_COMPARE(ctl->visibility(), QStringLiteral("invite_only"));
    QTRY_COMPARE(lastBody(hub, "PATCH", QStringLiteral("/api/v1/sessions/")).value(QStringLiteral("visibility")).toString(), QStringLiteral("invite_only"));
    QFile settings(h.dir.path() + QStringLiteral("/settings/player.json"));
    QVERIFY(settings.open(QIODevice::ReadOnly));
    QVERIFY(settings.readAll().contains("invite_only"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.item("inviteBlock")->isVisible());
    QVERIFY(h.item("visibilityHint")->isVisible());
    QCOMPARE(textOf(h.item("visibilityHint")), QStringLiteral("Only invited users can watch. They send no input and cannot invite others."));

    // A session_update still carrying the old visibility, arriving while a newer choice is being PATCHed, must not
    // revert the segment (the signal is emitted right after the click, before the PATCH answer can be read).
    {
      QJsonObject stale = hub.sessions.value(ownSessionId(hub));
      stale.insert(QStringLiteral("visibility"), QStringLiteral("invite_only"));
      const auto info = parseSession(stale);
      QVERIFY(info.has_value());
      QVERIFY(h.click("visHubUsers"));
      QCOMPARE(ctl->visibility(), QStringLiteral("hub_users"));
      emit ctl->socket()->sessionUpdated(*info);
      QCOMPARE(ctl->visibility(), QStringLiteral("hub_users"));
      QTRY_COMPARE(lastBody(hub, "PATCH", QStringLiteral("/api/v1/sessions/")).value(QStringLiteral("visibility")).toString(), QStringLiteral("hub_users"));
      QTest::qWait(200);
      QCOMPARE(ctl->visibility(), QStringLiteral("hub_users"));
      QVERIFY(h.click("visInviteOnly"));
      QTRY_COMPARE(lastBody(hub, "PATCH", QStringLiteral("/api/v1/sessions/")).value(QStringLiteral("visibility")).toString(), QStringLiteral("invite_only"));
      QTest::qWait(200);
      QCOMPARE(ctl->visibility(), QStringLiteral("invite_only"));
      QQuickTest::qWaitForPolish(h.window);
    }

    // Hub sends the Session with viewers and invites
    QJsonObject own = hub.sessions.value(ownSessionId(hub));
    own.insert(QStringLiteral("visibility"), QStringLiteral("invite_only"));
    own.insert(QStringLiteral("viewer_count"), 1);
    own.insert(QStringLiteral("viewers"), QJsonArray{QJsonObject{{QStringLiteral("viewer_id"), QStringLiteral("v1")}, {QStringLiteral("display_name"), QStringLiteral("Lena")}, {QStringLiteral("device_name"), QStringLiteral("PC")}}});
    own.insert(QStringLiteral("invites"), QJsonArray{
        QJsonObject{{QStringLiteral("user_id"), QStringLiteral("u_lena")}, {QStringLiteral("display_name"), QStringLiteral("Lena")}, {QStringLiteral("state"), QStringLiteral("joined")}, {QStringLiteral("online"), true}},
        QJsonObject{{QStringLiteral("user_id"), QStringLiteral("u_jonas")}, {QStringLiteral("display_name"), QStringLiteral("Jonas")}, {QStringLiteral("state"), QStringLiteral("invited")}, {QStringLiteral("online"), false}},
        QJsonObject{{QStringLiteral("user_id"), QStringLiteral("u_mia")}, {QStringLiteral("display_name"), QStringLiteral("Mia")}, {QStringLiteral("state"), QStringLiteral("declined")}, {QStringLiteral("online"), true}}});
    hub.sendWs(QStringLiteral("session_update"), {{QStringLiteral("session"), own}});
    QTRY_COMPARE(ctl->participants().size(), 3);
    QCOMPARE(ctl->participantsTitle(), QStringLiteral("INVITED · 3"));
    QCOMPARE(ctl->viewerCount(), 1);
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY2(panelButtonsOutside(h).isEmpty(), qPrintable(panelButtonsOutside(h)));
    QVERIFY(h.item("remove_v1")->isVisible());
    QVERIFY(h.item("withdraw_u_jonas")->isVisible());
    QVERIFY(h.item("withdraw_u_mia")->isVisible());
    const QVariantList parts = ctl->participants();
    QCOMPARE(parts.at(0).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("watching"));
    QCOMPARE(parts.at(1).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("invited · offline"));
    QCOMPARE(parts.at(2).toMap().value(QStringLiteral("status")).toString(), QStringLiteral("declined"));
    uitest::saveShot(h.window, QStringLiteral("p4-3g-invite-only"));

    // Invite search over GET /users (self excluded), invite = PUT
    h.item("inviteField")->setProperty("text", QStringLiteral("sa"));
    QTRY_COMPARE(ctl->userResults().size(), 2);  // Sam, Sarah; "Tester" is the own user
    QCOMPARE(ctl->userResults().at(1).toMap().value(QStringLiteral("hint")).toString(),
             QStringLiteral("offline · receives the invite as long as the Session is running"));
    QQuickTest::qWaitForPolish(h.window);
    QTRY_VERIFY(h.item("invite_u_sam") != nullptr && h.item("invite_u_sam")->isVisible());
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY2(panelButtonsOutside(h).isEmpty(), qPrintable(panelButtonsOutside(h)));
    QVERIFY(h.click("invite_u_sam"));
    QTRY_VERIFY2_WITH_TIMEOUT(countContaining(hub, QStringLiteral("/invites/u_sam"), "PUT") == 1, [&]() {
      QStringList l;
      for (const FakeRequest& rq : hub.requests) l << QString::fromLatin1(rq.method) + QLatin1Char(' ') + rq.path;
      return qPrintable(QStringLiteral("requests: ") + l.mid(l.size() - 8).join(QStringLiteral(" | ")) +
                        QStringLiteral(" ; results=") + QString::number(ctl->userResults().size()) + QStringLiteral(" msg=") + ctl->message());
    }(), 15000);

    // Empty states of the search: users exist but none match / no user besides the caller
    h.item("inviteField")->setProperty("text", QStringLiteral("zz"));
    QTRY_COMPARE(ctl->userSearchHint(), QStringLiteral("No matching users"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.item("userSearchHint")->isVisible());
    QCOMPARE(textOf(h.item("userSearchHint")), QStringLiteral("No matching users"));
    h.item("inviteField")->setProperty("text", QString());
    QTRY_VERIFY(ctl->userSearchHint().isEmpty());
    hub.fakeUsers = QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("u_test_1")}, {QStringLiteral("display_name"), QStringLiteral("Tester")}, {QStringLiteral("online"), true}}};
    h.item("inviteField")->setProperty("text", QStringLiteral("sa"));
    QTRY_COMPARE(ctl->userSearchHint(), QStringLiteral("Nobody else is here yet"));
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(textOf(h.item("userSearchHint")), QStringLiteral("Nobody else is here yet"));
    QVERIFY2(panelButtonsOutside(h).isEmpty(), qPrintable(panelButtonsOutside(h)));
    h.item("inviteField")->setProperty("text", QString());

    // (the fake PUT answers with a bare Session: the Hub's next update restores viewers and invites)
    hub.sendWs(QStringLiteral("session_update"), {{QStringLiteral("session"), own}});
    QTRY_COMPARE(ctl->participants().size(), 3);
    QQuickTest::qWaitForPolish(h.window);
    // Remove (DELETE viewer) and Withdraw (DELETE invite)
    QVERIFY(h.click("remove_v1"));
    QTRY_VERIFY(countContaining(hub, QStringLiteral("/viewers/v1"), "DELETE") == 1);
    QVERIFY(!ctl->host()->hasViewer(QStringLiteral("v1")));
    QVERIFY(h.click("withdraw_u_jonas"));
    QTRY_VERIFY(countContaining(hub, QStringLiteral("/invites/u_jonas"), "DELETE") == 1);

    // Visibility "Private": only watchers are listed, no invite block
    QVERIFY(h.click("visPrivate"));
    QTRY_COMPARE(ctl->visibility(), QStringLiteral("private"));
    // The local state flips at once; wait until the Hub applied the PATCH so no request of this step is still in flight.
    QTRY_COMPARE(lastBody(hub, "PATCH", QStringLiteral("/api/v1/sessions/")).value(QStringLiteral("visibility")).toString(), QStringLiteral("private"));
    QTRY_COMPARE(hub.sessions.value(ownSessionId(hub)).value(QStringLiteral("visibility")).toString(), QStringLiteral("private"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(!h.item("inviteBlock")->isVisible());
    QVERIFY(ctl->participantsTitle().startsWith(QStringLiteral("WATCHING")));

    // Diagnostics overlay in the Session tab (header button): Emulation and Streaming with the host row
    QVERIFY(!anyVisible(h, "diagnosticsOverlay"));
    QVERIFY(clickMore(h, "diagnostics"));
    QVERIFY(ctl->diagnostics()->isOpen());
    QQuickTest::qWaitForPolish(h.window);
    QTRY_VERIFY(anyVisible(h, "diagnosticsOverlay"));
    SessionStats local;
    local.active = true;
    local.encoderName = QStringLiteral("libx264");
    local.fps = 60.0;
    local.videoBitrateKbps = 2000;
    local.targetBitrateKbps = 2500;
    local.audioBitrateKbps = 96;
    ctl->setStatsOverride(&local, {});
    ctl->refreshDiagnostics();
    QVERIFY(ctl->diagnostics()->participantCount() >= 1);
    const QVariantMap host = ctl->diagnostics()->streaming().at(0).toMap().value(QStringLiteral("participants")).toList().at(0).toMap();
    QCOMPARE(host.value(QStringLiteral("role")).toString(), QStringLiteral("host"));
    QCOMPARE(host.value(QStringLiteral("line2")).toString(), QStringLiteral("Encoder libx264 · H.264 · 2.0 / target 2.5 Mbit/s · 60.0 fps"));
    QVERIFY(ctl->diagnostics()->emulation().value(QStringLiteral("valid")).toBool());
    ctl->setStatsOverride(nullptr, {});
    QVERIFY(clickMore(h, "diagnostics"));
    QVERIFY(!ctl->diagnostics()->isOpen());

    // Stop sharing -> DELETE session, pill back to "Not shared", button back to "Share this game…"
    QVERIFY(h.click("shareButton"));
    QTRY_VERIFY(!ctl->shared());
    QTRY_VERIFY(!hub.sessions.contains(QStringLiteral("00000000-0000-4000-8000-000000000002")) || ownSessionId(hub).isEmpty());
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(textOf(h.item("sharePill")), QStringLiteral("Not shared"));
    QCOMPARE(textOf(h.item("shareButton")), QStringLiteral("Share this game…"));
    // Share again with the remembered visibility, Hub ends it (replaced)
    QVERIFY(h.click("shareButton"));
    QTRY_VERIFY(ctl->shared());
    QCOMPARE(lastBody(hub, "POST", QStringLiteral("/api/v1/sessions")).value(QStringLiteral("visibility")).toString(), QStringLiteral("private"));
    hub.sendWs(QStringLiteral("session_ended"), {{QStringLiteral("session_id"), ownSessionId(hub)}, {QStringLiteral("reason"), QStringLiteral("replaced")}});
    QTRY_VERIFY(!ctl->shared());
    QVERIFY(ctl->message().contains(QStringLiteral("replaced")));

    // Quit ends the game (and a shared Session)
    QVERIFY(h.click("shareButton"));
    QTRY_VERIFY(ctl->shared());
    QVERIFY(h.click("endGameButton"));
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("library"));
    QVERIFY(!ctl->shared());
    QTRY_VERIFY(countContaining(hub, QStringLiteral("/api/v1/sessions/"), "DELETE") >= 2);  // stopped + ended with the game
  }

  // Up to four remote Sessions without a local game (ADR 0012 D8): adding from the list, the surface limit, layouts by
  // count, Swap, audio focus, Remove and per-Session end handling. Needs no core.
  void multipleRemoteSurfacesWithoutLocalGame() {
    FakeHub hub(QStringLiteral("a"));
    prepareHub(hub);
    const QStringList ids{QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3"), QStringLiteral("s4"), QStringLiteral("s5")};
    const QStringList who{QStringLiteral("Lena"), QStringLiteral("Mo"), QStringLiteral("Jo"), QStringLiteral("Ann"), QStringLiteral("Bob")};
    for (int i = 0; i < ids.size(); ++i) {
      hub.sessions.insert(ids.at(i), sessionObj(ids.at(i), who.at(i), QStringLiteral("Game %1").arg(who.at(i)), QStringLiteral("hub_users")));
    }
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SessionController* ctl = h.controller->sessions();
    QTRY_COMPARE(ctl->sessions().size(), 5);
    QQuickTest::qWaitForPolish(h.window);

    // First Session from the Library: a single surface fills the area, no layout to choose
    QVERIFY(h.click("watch_s1"));
    QTRY_VERIFY(ctl->watching());
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(ctl->surfaceCount(), 1);
    QCOMPARE(ctl->maxSurfaces(), 4);
    QVERIFY(ctl->availableLayouts().isEmpty());
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("pip"));  // default choice, nothing to choose with one surface
    QVERIFY(!h.item("modeSegment")->isVisible());
    QVERIFY(approxEqual(tileRect(h, QStringLiteral("s1")).width(), areaRect(h).width() - 48) && approxEqual(tileRect(h, QStringLiteral("s1")).height(), areaRect(h).height() - 48));  // padding 24
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s1"));
    QVERIFY(visibleItem(h, "multiviewSessionList") == nullptr);  // the picker never opens on its own
    QVERIFY(h.click("addSessionToggle"));
    QTRY_VERIFY(visibleItem(h, "multiviewSessionList") != nullptr);
    QVERIFY(!h.item("multiviewAddButton_s1")->isVisible());      // already shown: "✓ Added"
    QVERIFY(visibleItem(h, "multiviewAdded_s1") != nullptr);
    QVERIFY(h.item("multiviewAddButton_s2")->isEnabled());

    // Two surfaces: PiP, Row or Grid 2 x 2
    QVERIFY(h.clickWhenReady("multiviewAddButton_s2"));
    QTRY_COMPARE(ctl->surfaceCount(), 2);
    QCOMPARE(ctl->availableLayouts(), (QStringList{QStringLiteral("pip"), QStringLiteral("row"), QStringLiteral("grid")}));
    QVERIFY(!ctl->gridAvailable());  // Grid 2 x 2 only from four Sessions
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("pip"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.item("modeSegment") != nullptr && h.item("modePip") != nullptr && h.item("modeRow") != nullptr && h.item("modeGrid") != nullptr);   // (the segment itself gives way to "⋯ > Multiview: …" with very wide fonts)
    QVERIFY(h.item("multiviewSessionList")->isVisible());  // stays open until the anchor or the outside is clicked
    QVERIFY(h.click("addSessionToggle"));
    QTRY_VERIFY(!h.item("multiviewSessionList")->isVisible());
    QCOMPARE(hub.count(QStringLiteral("/api/v1/sessions/s2/join")), 1);
    QVERIFY(clickMode(h, "modeRow"));
    QQuickTest::qWaitForPolish(h.window);
    {
      const QRectF a = areaRect(h), t1 = tileRect(h, QStringLiteral("s1")), t2 = tileRect(h, QStringLiteral("s2"));
      QVERIFY(approxEqual(t1.left(), a.left() + 24) && approxEqual(t1.top(), a.top() + 24) && approxEqual(t1.height(), a.height() - 48));
      QVERIFY(approxEqual(t1.width() * 2 + 24, a.width() - 48) && approxEqual(t2.width(), t1.width()) && approxEqual(t2.top(), t1.top()) && approxEqual(t2.left(), t1.right() + 24));
    }

    // Three surfaces: Row stays (Grid is disabled below four, no empty slot), the list is reopened with "+ Add"
    QVERIFY(h.click("addSessionToggle"));
    QVERIFY(h.item("multiviewSessionList")->isVisible());
    QVERIFY(h.clickWhenReady("multiviewAddButton_s3"));
    QTRY_COMPARE(ctl->surfaceCount(), 3);
    QCOMPARE(ctl->availableLayouts(), (QStringList{QStringLiteral("pip"), QStringLiteral("row"), QStringLiteral("grid")}));
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("row"));
    ctl->setMultiviewMode(QStringLiteral("grid"));  // preference only: below four Sessions the Row is shown
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("row"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.item("modeGrid") != nullptr && h.item("modeRow") != nullptr);
    {  // greyed: the label says why
      bool found = false;
      std::function<void(QQuickItem*)> walk = [&](QQuickItem* it) {
        if (it->property("text").toString().contains(QStringLiteral("from 4"))) found = true;
        for (QQuickItem* c : it->childItems()) walk(c);
      };
      walk(h.item("modeGrid"));
      QVERIFY(found);
    }

    // Four surfaces: the limit; "Add" is disabled for every other Session, further adds are refused
    QVERIFY(h.clickWhenReady("multiviewAddButton_s4"));
    QTRY_COMPARE(ctl->surfaceCount(), 4);
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(!ctl->canAddSurface());
    QVERIFY(h.item("multiviewFullNote")->isVisible());
    for (const QString& id : ids) {
      QVERIFY2(!h.item(qPrintable(QStringLiteral("multiviewAddButton_") + id))->isEnabled(), qPrintable(id));
    }
    ctl->watch(QStringLiteral("s5"));
    QCOMPARE(ctl->surfaceCount(), 4);
    QVERIFY(ctl->messageIsError());
    QCOMPARE(hub.count(QStringLiteral("/api/v1/sessions/s5/join")), 0);
    ctl->dismissMessage();
    QVERIFY(h.click("addSessionToggle"));  // close the picker (click on its anchor)
    QTRY_VERIFY(!h.item("multiviewSessionList")->isVisible());
    {
      const QRectF a = areaRect(h);
      const QRectF t1 = tileRect(h, QStringLiteral("s1")), t2 = tileRect(h, QStringLiteral("s2")), t3 = tileRect(h, QStringLiteral("s3")), t4 = tileRect(h, QStringLiteral("s4"));
      QVERIFY(approxEqual(t1.left(), a.left() + 24) && approxEqual(t1.top(), a.top() + 24));
      QVERIFY(approxEqual(t2.top(), t1.top()) && approxEqual(t2.left(), t1.right() + 24));
      QVERIFY(approxEqual(t3.left(), t1.left()) && approxEqual(t3.top(), t1.bottom() + 24));
      QVERIFY(approxEqual(t4.left(), t2.left()) && approxEqual(t4.top(), t3.top()));
      QVERIFY(approxEqual(t4.right(), a.right() - 24) && approxEqual(t4.bottom(), a.bottom() - 24) && approxEqual(t1.width(), t4.width()) && approxEqual(t1.height(), t4.height()));
    }
    uitest::saveShot(h.window, QStringLiteral("p4-d8-grid"));

    // PiP: main surface fills the area, three tiles stacked bottom right
    QVERIFY(clickMode(h, "modePip"));
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("pip"));
    QQuickTest::qWaitForPolish(h.window);
    {
      const QRectF a = areaRect(h);
      QVERIFY(approxEqual(tileRect(h, QStringLiteral("s1")).width(), a.width() - 48) && approxEqual(tileRect(h, QStringLiteral("s1")).height(), a.height() - 48));
      QRectF above;
      for (const QString& id : {QStringLiteral("s2"), QStringLiteral("s3"), QStringLiteral("s4")}) {
        const QRectF t = tileRect(h, id);
        QVERIFY2(approxEqual(t.right(), a.right() - 41) && t.width() > 100 && t.left() >= a.left() && t.top() >= a.top(), qPrintable(id));
        if (id == QLatin1String("s2")) {
          QVERIFY(approxEqual(t.bottom(), a.bottom() - 41));
        } else {
          QVERIFY2(t.bottom() <= above.top() + 0.5, qPrintable(id));  // stacked upwards without overlap
        }
        // The window sits inside the main tile, clear of its 2 px border (P38, 3i-3)
        QVERIFY2(tileRect(h, QStringLiteral("s1")).adjusted(2, 2, -2, -2).contains(t), qPrintable(id));
        above = t;
      }
    }
    uitest::saveShot(h.window, QStringLiteral("p4-d8-pip4"));

    // Swap: the tile becomes the main surface (it takes the main position, the old main takes its place)
    QVERIFY(h.click("swapButton_s3"));
    QCOMPARE(ctl->mainSurface(), QStringLiteral("s3"));
    QCOMPARE(ctl->surfaceOrder(), (QStringList{QStringLiteral("s3"), QStringLiteral("s2"), QStringLiteral("s1"), QStringLiteral("s4")}));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(approxEqual(tileRect(h, QStringLiteral("s3")).width(), areaRect(h).width() - 48));
    QVERIFY(approxEqual(tileRect(h, QStringLiteral("s1")).right(), areaRect(h).right() - 41));
    QVERIFY(h.click("swapButton_s1"));
    QCOMPARE(ctl->surfaceOrder(), (QStringList{QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3"), QStringLiteral("s4")}));

    // Audio focus: exactly one audible surface (the first one without a local game); muted surfaces feed no audio
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s1"));
    QCOMPARE(audibleTile(h, ids.mid(0, 4)), QStringLiteral("s1"));
    QVERIFY(selectAndClick(h, ctl, QStringLiteral("s3"), "audioButton_"));  // "Audio here" in the panel of the selected tile
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s3"));
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(audibleTile(h, ids.mid(0, 4)), QStringLiteral("s3"));
    QTRY_VERIFY(ctl->audioFedFrames(QStringLiteral("s3")) > 0);
    const qint64 fed1 = ctl->audioFedFrames(QStringLiteral("s1"));
    QTest::qWait(150);
    QCOMPARE(ctl->audioFedFrames(QStringLiteral("s1")), fed1);  // s1 is muted now
    QCOMPARE(ctl->audioFedFrames(QStringLiteral("s2")), qint64(0));
    QCOMPARE(ctl->audioFedFrames(QStringLiteral("s4")), qint64(0));

    // Mixer popover (0.23 audio stage 2): a slider per watched person and "Mix all tiles" (off = the one audible tile as above)
    {
      AudioMixer* mx = ctl->audioMixer();
      if (visibleItem(h, "mixerButton") != nullptr) QVERIFY(h.click("mixerButton"));
      else QVERIFY(clickMore(h, "mixer"));  // a narrow header (wide fonts) moves it into "⋯"
      QTRY_VERIFY(visibleItem(h, "mixerSlider_s1") != nullptr);
      for (const char* sid : {"mixerSlider_s2", "mixerSlider_s3", "mixerSlider_s4"}) QVERIFY(visibleItem(h, sid) != nullptr);
      QVERIFY(visibleItem(h, "mixAllToggle") != nullptr);
      QVERIFY(!ctl->mixAllTiles() && !mx->mixAll());
      QCOMPARE(ctl->audibleSurfaces(), QStringList{QStringLiteral("s3")});
      QCOMPARE(visibleItem(h, "surfaceTile_s3")->property("audible").toBool(), true);
      QCOMPARE(visibleItem(h, "surfaceTile_s1")->property("audible").toBool(), false);
      uitest::saveShot(h.window, QStringLiteral("p6-mixer-popover"));

      QVERIFY(h.click("mixAllToggle"));
      QTRY_VERIFY(ctl->mixAllTiles());
      QVERIFY(mx->mixAll());
      QCOMPARE(ctl->audibleSurfaces().size(), 4);
      QTRY_COMPARE(visibleItem(h, "surfaceTile_s1")->property("audible").toBool(), true);
      QTRY_VERIFY(ctl->audioFedFrames(QStringLiteral("s2")) > 0);  // every routed tile is fed now

      QQuickItem* sl = visibleItem(h, "mixerSlider_s2");
      sl->setProperty("value", 40);
      QMetaObject::invokeMethod(sl, "moved");
      QTRY_COMPARE(mx->personGain(AudioMixer::Tiles, QStringLiteral("u_x")), 0.4);
      QCOMPARE(ctl->mixerTiles().at(0).toMap().value(QStringLiteral("gain")).toInt(), 40);
      QCOMPARE(h.controller->playerSettings()->audioTileGain(QStringLiteral("u_x")), 40);
      QVERIFY(h.controller->playerSettings()->audioMixAll());

      QVERIFY(h.click("mixAllToggle"));
      QTRY_VERIFY(!ctl->mixAllTiles());
      QCOMPARE(ctl->audibleSurfaces(), QStringList{QStringLiteral("s3")});
      QTest::keyClick(h.window, Qt::Key_Escape);
      QTRY_VERIFY(visibleItem(h, "mixerSlider_s1") == nullptr);
      ctl->setTileGain(QStringLiteral("s1"), 100);
    }

    // Remove the focused surface (leaves via the Hub): focus moves to the first remaining surface
    QVERIFY(selectAndClick(h, ctl, QStringLiteral("s3"), "removeButton_"));  // "Remove from multiview" (panel; PiP windows have their own ×)
    QTRY_COMPARE(ctl->surfaceCount(), 3);
    QCOMPARE(ctl->shownSessionIds(), (QStringList{QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s4")}));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s1"));
    QVERIFY(ctl->viewer(QStringLiteral("s3")) == nullptr && ctl->viewer(QStringLiteral("s1")) != nullptr);
    QTRY_VERIFY(hub.count(QStringLiteral("/api/v1/sessions/s3/viewers/")) >= 1);
    QCOMPARE(hub.count(QStringLiteral("/api/v1/sessions/s1/viewers/")), 0);
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(audibleTile(h, {QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s4")}), QStringLiteral("s1"));
    QVERIFY(ctl->canAddSurface());
    if (visibleItem(h, "multiviewSessionList") == nullptr) QVERIFY(h.click("addSessionToggle"));  // the click on the tile closed the popover
    QTRY_VERIFY(h.item("multiviewAddButton_s3") != nullptr);
    QVERIFY(h.item("multiviewAddButton_s3")->isEnabled());  // joinable again
    QVERIFY(h.click("addSessionToggle"));
    QTRY_VERIFY(visibleItem(h, "multiviewSessionList") == nullptr);

    // A Session that ends removes only its own surface
    QVERIFY(selectAndClick(h, ctl, QStringLiteral("s2"), "audioButton_"));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s2"));
    hub.sendWs(QStringLiteral("session_ended"), {{QStringLiteral("session_id"), QStringLiteral("s2")}, {QStringLiteral("reason"), QStringLiteral("ended")}});
    QTRY_COMPARE(ctl->surfaceCount(), 2);
    QCOMPARE(ctl->shownSessionIds(), (QStringList{QStringLiteral("s1"), QStringLiteral("s4")}));
    QVERIFY(ctl->watching());
    QCOMPARE(ctl->message(), QStringLiteral("The Session ended."));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s1"));  // focused surface ended: first remaining
    QCOMPARE(h.controller->screen(), QStringLiteral("game"));
    // ... and so does viewer_left (removed by the owner) for one Session
    hub.sendWs(QStringLiteral("viewer_left"), {{QStringLiteral("session_id"), QStringLiteral("s4")}, {QStringLiteral("viewer_id"), kViewerId},
                                               {QStringLiteral("reason"), QStringLiteral("removed")}});
    QTRY_COMPARE(ctl->surfaceCount(), 1);
    QCOMPARE(ctl->shownSessionIds(), QStringList{QStringLiteral("s1")});
    QCOMPARE(ctl->message(), QStringLiteral("You were removed from the Session."));
    QVERIFY(ctl->availableLayouts().isEmpty());
    QCOMPARE(h.controller->screen(), QStringLiteral("game"));

    // Focus on a later surface, then remove it: back to the first remaining
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.click("addSessionToggle"));
    QVERIFY(h.clickWhenReady("multiviewAddButton_s5"));
    QTRY_COMPARE(ctl->surfaceCount(), 2);
    QVERIFY(h.click("addSessionToggle"));
    QVERIFY(selectAndClick(h, ctl, QStringLiteral("s5"), "audioButton_"));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s5"));
    QVERIFY(selectAndClick(h, ctl, QStringLiteral("s5"), "removeButton_"));
    QTRY_COMPARE(ctl->surfaceCount(), 1);
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s1"));

    // "← Library" leaves the remaining Sessions
    QVERIFY(h.click("backToLibraryButton"));
    QTRY_VERIFY(!ctl->watching());
    QTRY_VERIFY(hub.count(QStringLiteral("/api/v1/sessions/s1/viewers/")) >= 1);
    QCOMPARE(h.controller->screen(), QStringLiteral("library"));
  }

  // Local game plus up to three remote Sessions (needs the core).
  // ADR 0026 D3: the host's pause signal reaches the viewer tile ("Paused by <name>"). The wire round trip itself is covered by
  // media_loopback and media_host_state; here the viewer's signal drives the controller and the tile.
  void pausedByHostShowsOnTheViewerTile() {
    FakeHub hub(QStringLiteral("a"));
    prepareHub(hub);
    hub.sessions.insert(QStringLiteral("s1"), sessionObj(QStringLiteral("s1"), QStringLiteral("Lena"), QStringLiteral("Game Lena"), QStringLiteral("hub_users")));
    hub.sessions.insert(QStringLiteral("s2"), sessionObj(QStringLiteral("s2"), QStringLiteral("Mo"), QStringLiteral("Game Mo"), QStringLiteral("hub_users")));
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    SessionController* ctl = h.controller->sessions();
    QTRY_COMPARE(ctl->sessions().size(), 2);
    QVERIFY(h.click("watch_s1"));
    QTRY_VERIFY(ctl->watching());
    QVERIFY(h.click("addSessionToggle"));
    QVERIFY(h.clickWhenReady("multiviewAddButton_s2"));
    QTRY_COMPARE(ctl->surfaceCount(), 2);
    QQuickTest::qWaitForPolish(h.window);
    SessionViewer* v1 = ctl->viewer(QStringLiteral("s1"));
    QVERIFY(v1 != nullptr);
    QVERIFY(!ctl->surfaceInfo().value(QStringLiteral("s1")).toMap().value(QStringLiteral("paused")).toBool());
    QVERIFY(visibleItem(h, "tilePausedBanner") == nullptr);

    emit v1->hostStateChanged(true, QStringLiteral("Lena"));
    QQuickTest::qWaitForPolish(h.window);
    const QVariantMap info = ctl->surfaceInfo().value(QStringLiteral("s1")).toMap();
    QVERIFY(info.value(QStringLiteral("paused")).toBool());
    QCOMPARE(info.value(QStringLiteral("pausedBy")).toString(), QStringLiteral("Lena"));
    QVERIFY(!ctl->surfaceInfo().value(QStringLiteral("s2")).toMap().value(QStringLiteral("paused")).toBool());  // only that tile
    QTRY_VERIFY(visibleItem(h, "tilePausedBanner") != nullptr);
    QVERIFY(visibleItem(h, "tilePausedBanner")->property("title").toString().contains(QStringLiteral("Paused by Lena")));

    emit v1->hostStateChanged(false, QString());
    QTRY_VERIFY(visibleItem(h, "tilePausedBanner") == nullptr);
    QVERIFY(!ctl->surfaceInfo().value(QStringLiteral("s1")).toMap().value(QStringLiteral("paused")).toBool());
  }

  void multiviewWithLocalGameAndAudioFocus() {
    GameRig r;
    if (!startGame(r, false)) UITEST_SKIP_NO_CORE("core or test ROM not available");
    Harness& h = r.h;
    SessionController* ctl = h.controller->sessions();
    GameSession* gs = h.controller->gameSession();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->screen(), QStringLiteral("game"), 20000);
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(ctl->tab(), QStringLiteral("session"));
    QVERIFY(!ctl->shared());
    QCOMPARE(textOf(h.item("shareButton")), QStringLiteral("Share this game…"));
    QCOMPARE(textOf(h.item("sharePill")), QStringLiteral("Not shared"));
    QVERIFY(!gs->audioMuted());

    // The local game alone: the Session list of the Multiview tab offers every Session of the Hub
    QTRY_COMPARE(ctl->sessions().size(), 1);
    QVERIFY(clickHeader(h, "tabMultiview", "multiview"));
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(ctl->surfaceCount(), 1);
    QVERIFY(visibleItem(h, "multiviewSessionList") == nullptr);  // the picker does not open by itself, not even with one tile
    QVERIFY(h.click("addSessionToggle"));
    QTRY_VERIFY(h.item("multiviewSessionList")->isVisible());
    QVERIFY(!h.item("multiviewNoSessions")->isVisible());
    QVERIFY(visibleItem(h, "gameViewMulti") != nullptr);
    QVERIFY(!h.item("modeSegment")->isVisible());
    {
      const QQuickItem* box = h.item("multiviewSessionList");
      const QRectF br = box->mapRectToScene(QRectF(0, 0, box->width(), box->height()));
      const QQuickItem* b0 = h.item("multiviewAddButton_s1");
      QVERIFY(b0 != nullptr && b0->isVisible() && b0->isEnabled());
      QCOMPARE(textOf(const_cast<QQuickItem*>(b0)), QStringLiteral("Add"));
      const QRectF r0 = b0->mapRectToScene(QRectF(0, 0, b0->width(), b0->height()));
      QVERIFY2(r0.left() >= br.left() && r0.right() <= br.right() + 1 && br.right() <= h.window->width() + 1, "add button outside the list");
    }
    // Live: the list follows the Hub (WSS), here more Sessions appear and end again
    r.hub.sessions.insert(QStringLiteral("s2"), sessionObj(QStringLiteral("s2"), QStringLiteral("Mo"), QStringLiteral("Other"), QStringLiteral("hub_users"), false, 1));
    r.hub.sendWs(QStringLiteral("session_update"), {{QStringLiteral("session"), r.hub.sessions.value(QStringLiteral("s2"))}});
    QTRY_COMPARE(ctl->sessions().size(), 2);
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.item("multiviewAddButton_s2") != nullptr && h.item("multiviewAddButton_s2")->isVisible());
    for (const QString& id : {QStringLiteral("s3"), QStringLiteral("s4"), QStringLiteral("s5"), QStringLiteral("s6")}) {
      r.hub.sessions.insert(id, sessionObj(id, QStringLiteral("Mo"), QStringLiteral("Other"), QStringLiteral("hub_users"), false, 1));
      r.hub.sendWs(QStringLiteral("session_update"), {{QStringLiteral("session"), r.hub.sessions.value(id)}});
    }
    QTRY_COMPARE(ctl->sessions().size(), 6);
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.item("multiviewAddButton_s6") != nullptr);
    {  // all Sessions are listed (scrollable), no pointer to the Library
      QQuickItem* view = h.item("multiviewSessionView");
      QVERIFY(view != nullptr);
      QVERIFY(view->property("contentHeight").toReal() >= view->height());
      QVERIFY(view->property("count").toInt() == 6);
    }
    for (const QString& id : {QStringLiteral("s5"), QStringLiteral("s6")}) {
      r.hub.sendWs(QStringLiteral("session_ended"), {{QStringLiteral("session_id"), id}, {QStringLiteral("reason"), QStringLiteral("ended")}});
    }
    QTRY_COMPARE(ctl->sessions().size(), 4);

    // Add the first remote Session: the game keeps running and keeps the audio, PiP with the local game as main
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.clickWhenReady("multiviewAddButton_s1"));
    QTRY_VERIFY(ctl->watching());
    QVERIFY(h.click("addSessionToggle"));  // close the picker (click on its anchor)
    QVERIFY(gs->isActive());
    QCOMPARE(ctl->surfaceCount(), 2);
    QCOMPARE(ctl->surfaceOrder(), (QStringList{QStringLiteral("local"), QStringLiteral("s1")}));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("local"));
    QVERIFY(!gs->audioMuted());
    QCOMPARE(ctl->tab(), QStringLiteral("multiview"));
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("pip"));
    QCOMPARE(h.controller->screen(), QStringLiteral("game"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(visibleItem(h, "gameViewMulti") != nullptr);
    QVERIFY(visibleItem(h, "remoteView") != nullptr);
    QCOMPARE(audibleTile(h, {QStringLiteral("local"), QStringLiteral("s1")}), QStringLiteral("local"));
    QVERIFY(visibleItem(h, "swapButton_s1") != nullptr && visibleItem(h, "removeButton_s1") != nullptr);
    QVERIFY(visibleItem(h, "removeButton_local") == nullptr);  // the local game is not a Session to leave
    QQuickItem* localView = visibleItem(h, "gameViewMulti");
    {  // PiP tile bottom right (inset 30, clear of the main tile border)
      const QRectF a = areaRect(h), t = tileRect(h, QStringLiteral("s1"));
      QVERIFY(approxEqual(t.right(), a.right() - 41) && approxEqual(t.bottom(), a.bottom() - 41));
    }
    uitest::saveShot(h.window, QStringLiteral("p4-3i-pip"));

    // Audio here (remote, in the panel of the selected tile): exactly one audible surface, the game is muted
    QVERIFY(selectAndClick(h, ctl, QStringLiteral("s1"), "audioButton_"));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s1"));
    QVERIFY(gs->audioMuted());
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(audibleTile(h, {QStringLiteral("local"), QStringLiteral("s1")}), QStringLiteral("s1"));
    QTRY_VERIFY(ctl->audioFedFrames(QStringLiteral("s1")) > 0);
    QVERIFY(selectAndClick(h, ctl, QStringLiteral("local"), "audioButton_"));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("local"));
    QVERIFY(!gs->audioMuted());
    {  // muted remote feeds nothing while the local game is audible
      const qint64 fed = ctl->audioFedFrames(QStringLiteral("s1"));
      QTest::qWait(150);
      QCOMPARE(ctl->audioFedFrames(QStringLiteral("s1")), fed);
    }

    // Swap: the remote Session becomes the main surface; the local view survives layout changes
    QVERIFY(h.click("swapButton_s1"));
    QCOMPARE(ctl->mainSurface(), QStringLiteral("s1"));
    QVERIFY(h.click("swapButton_local"));
    QCOMPARE(ctl->mainSurface(), QStringLiteral("local"));
    QVERIFY(visibleItem(h, "gameViewMulti") == localView);

    // Row (3r-3)
    QVERIFY(clickMode(h, "modeRow"));
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("row"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(approxEqual(tileRect(h, QStringLiteral("local")).width() * 2 + 24, areaRect(h).width() - 48));
    QCOMPARE(audibleTile(h, {QStringLiteral("local"), QStringLiteral("s1")}), QStringLiteral("local"));
    ctl->selectSurface(QStringLiteral("local"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(!visibleItem(h, "audioButton_local")->isEnabled() && textOf(visibleItem(h, "audioButton_local")) == QStringLiteral("Audio plays from this tile"));
    ctl->selectSurface(QStringLiteral("s1"));  // the panel follows the selected tile
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(visibleItem(h, "audioButton_s1")->isEnabled() && textOf(visibleItem(h, "audioButton_s1")) == QStringLiteral("Audio here"));
    QVERIFY(visibleItem(h, "gameViewMulti") == localView);
    uitest::saveShot(h.window, QStringLiteral("p4-3h-row"));
    QVERIFY(h.click("audioButton_s1"));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("s1"));
    QVERIFY(gs->audioMuted());
    QQuickTest::qWaitForPolish(h.window);
    QCOMPARE(audibleTile(h, {QStringLiteral("local"), QStringLiteral("s1")}), QStringLiteral("s1"));
    QVERIFY(!visibleItem(h, "audioButton_s1")->isEnabled());
    QVERIFY(selectAndClick(h, ctl, QStringLiteral("local"), "audioButton_"));
    QCOMPARE(ctl->audioFocus(), QStringLiteral("local"));
    QVERIFY(!gs->audioMuted());

    // Diagnostics tab expanded under the multiview: local row and one row per remote surface
    SessionStats local, remote;
    local.active = true;
    local.encoderName = QStringLiteral("h264_nvenc");
    local.fps = 60.0;
    local.videoBitrateKbps = 6000;
    local.audioBitrateKbps = 128;
    remote.active = true;
    remote.fps = 59.9;
    remote.connectionType = QStringLiteral("direct (srflx)");
    remote.rttMs = 14;
    remote.videoBitrateKbps = 5800;
    remote.packetLossPercent = 0.1;
    ctl->setStatsOverride(&local, {{QStringLiteral("s1"), remote}});
    QVERIFY(clickMore(h, "diagnostics"));  // multiview: the same 340 px overlay, a block per tile
    QVERIFY(ctl->diagnostics()->isOpen());
    ctl->refreshDiagnostics();
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(visibleItem(h, "diagnosticsOverlay") != nullptr);
    QVERIFY(visibleItem(h, "diagnosticsPanel") == nullptr);  // the bottom bar is gone
    QCOMPARE(ctl->diagnostics()->tiles().size(), 2);
    QVERIFY(ctl->diagnostics()->tiles().at(0).toMap().value(QStringLiteral("local")).toBool());
    QVERIFY(ctl->diagnostics()->tiles().at(1).toMap().value(QStringLiteral("note")).toString().contains(QStringLiteral("Emulation runs on Lena's PC")));
    {  // one row per remote tile says where its emulation runs (the Copy text carries every row)
      QVariant text;
      QVERIFY(QMetaObject::invokeMethod(visibleItem(h, "diagnosticsOverlay"), "fullText", Q_RETURN_ARG(QVariant, text)));
      QVERIFY(text.toString().contains(QStringLiteral("Emulation runs on Lena's PC")));
    }
    QCOMPARE(ctl->diagnostics()->streaming().size(), 2);  // own session + the watched one
    const QVariantMap remoteHost = ctl->diagnostics()->streaming().at(1).toMap().value(QStringLiteral("participants")).toList().at(0).toMap();
    QCOMPARE(remoteHost.value(QStringLiteral("pill")).toString(), QStringLiteral("Direct"));
    uitest::saveShot(h.window, QStringLiteral("p4-3h-diagnostics"));
    ctl->setStatsOverride(nullptr, {});
    QVERIFY(clickMore(h, "diagnostics"));
    QVERIFY(!ctl->diagnostics()->isOpen());
    QCOMPARE(ctl->tab(), QStringLiteral("multiview"));

    // Local game plus three remote Sessions = four surfaces: grid 2 x 2, limit, audio focus without losing the game
    QVERIFY(h.click("addSessionToggle"));
    QVERIFY(h.clickWhenReady("multiviewAddButton_s2"));
    QTRY_COMPARE(ctl->surfaceCount(), 3);
    QCOMPARE(ctl->multiviewMode(), QStringLiteral("row"));  // three surfaces stay a Row; Grid 2 x 2 starts at four
    QVERIFY(h.clickWhenReady("multiviewAddButton_s3"));
    QTRY_COMPARE(ctl->surfaceCount(), 4);
    QVERIFY(!ctl->canAddSurface());
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(!h.item("multiviewAddButton_s4")->isEnabled());
    QVERIFY(h.click("addSessionToggle"));  // close the picker
    QVERIFY(visibleItem(h, "gameViewMulti") == localView);
    {
      const QRectF a = areaRect(h), t1 = tileRect(h, QStringLiteral("local")), t4 = tileRect(h, QStringLiteral("s3"));
      QVERIFY(approxEqual(t1.left(), a.left() + 24) && approxEqual(t4.right(), a.right() - 24) && approxEqual(t4.bottom(), a.bottom() - 24) && approxEqual(t1.width(), t4.width()));
    }
    uitest::saveShot(h.window, QStringLiteral("p4-d8-grid4"));
    ctl->audioHere(QStringLiteral("s3"));
    QVERIFY(gs->audioMuted());
    QVERIFY(selectAndClick(h, ctl, QStringLiteral("s3"), "removeButton_"));  // removing the focused surface: the local game takes the focus back
    QTRY_COMPARE(ctl->surfaceCount(), 3);
    QCOMPARE(ctl->audioFocus(), QStringLiteral("local"));
    QVERIFY(!gs->audioMuted());
    ctl->audioHere(QStringLiteral("s2"));
    QVERIFY(gs->audioMuted());
    r.hub.sendWs(QStringLiteral("session_ended"), {{QStringLiteral("session_id"), QStringLiteral("s2")}, {QStringLiteral("reason"), QStringLiteral("ended")}});
    QTRY_COMPARE(ctl->surfaceCount(), 2);  // only that surface is gone; s1 and the game keep running
    QCOMPARE(ctl->shownSessionIds(), QStringList{QStringLiteral("s1")});
    QCOMPARE(ctl->audioFocus(), QStringLiteral("local"));
    QVERIFY(!gs->audioMuted());
    QVERIFY(gs->isActive());

    // Remove (PiP): leaves the Session (DELETE viewer), local game alone again
    QVERIFY(clickMode(h, "modePip"));
    QQuickTest::qWaitForPolish(h.window);
    QVERIFY(h.click("removeButton_s1"));
    QVERIFY(!ctl->watching());
    QTRY_VERIFY(r.hub.count(QStringLiteral("/api/v1/sessions/s1/viewers/")) >= 1);
    QCOMPARE(ctl->surfaceCount(), 1);
    QCOMPARE(ctl->audioFocus(), QStringLiteral("local"));
    QVERIFY(!gs->audioMuted());
    QCOMPARE(h.controller->screen(), QStringLiteral("game"));
    QCOMPARE(ctl->selectedSurface(), QStringLiteral("local"));  // the selection fell back to your game
    QVERIFY(h.click("endGameButton"));
    QTRY_COMPARE(h.controller->screen(), QStringLiteral("library"));
  }
};

UITEST_MAIN(SessionsUiTest)
#include "sessions_ui_test.moc"
