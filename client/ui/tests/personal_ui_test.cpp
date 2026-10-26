// Favourites, collections, play time and last played in the Library and on the game page (Uzume 0.19, boards 3c-0, 3c-1, 3c-4)
// against the fake server: the collection chips and the Played / Last played columns, the collection dialogs and menu, the
// Favourite button and the facts of the game page, "Add to collection", the empty states, and an older server without any of it.
// Offscreen; synthetic data only.
#include <QJsonArray>
#include <QtTest>

#include "fakehub.h"
#include "keyedrowsmodel.h"
#include "runscontroller.h"
#include "testsupport.h"

using namespace uzume;
using namespace uzume::ui;
using uitest::Harness;
using S = HubConnection::State;

namespace {
const QStringList kFeatures{QStringLiteral("saves_v1"), QStringLiteral("saves_v2"), QStringLiteral("sessions_v1")};

QJsonObject personal(qint64 secs, int sessions, const QString& last, bool fav, const QStringList& cols) {
  return {{QStringLiteral("play_seconds"), secs},
          {QStringLiteral("sessions"), sessions},
          {QStringLiteral("last_played_at"), last.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(last)},
          {QStringLiteral("favorite"), fav},
          {QStringLiteral("collection_ids"), QJsonArray::fromStringList(cols)}};
}
}  // namespace

class PersonalUiTest : public QObject {
  Q_OBJECT

  static void pair(Harness& h, FakeHub& hub) {
    hub.decision = FakeHub::Decision::Approve;
    h.controller->addHub(hub.address());
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsTrustConfirmation, 8000);
    h.controller->confirmTrust();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->connection()->state(), S::NeedsPairing, 8000);
    h.controller->requestPairing();
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->libraryState(), QStringLiteral("ready"), 8000);
    h.window->resize(1400, 900);
  }
  // Three games: g1 12 h, favourite, in c1; g2 45 min, in c1 and c2; g3 never played. Collections c1 "Couch co-op", c2 "Nuzlocke".
  static void seed(FakeHub& hub, bool withPersonal = true) {
    const QByteArray rom = "homebrew-dummy-rom-personal";
    hub.features = kFeatures;
    hub.roms.insert(uitest::sha256Hex(rom), rom);
    QJsonArray games;
    for (const QString& id : {QStringLiteral("g1"), QStringLiteral("g2"), QStringLiteral("g3")}) {
      QJsonObject g = uitest::gameJson(id, id == QLatin1String("g1") ? QStringLiteral("Zephyr Run") : id == QLatin1String("g2") ? QStringLiteral("Alpha Garden") : QStringLiteral("Moon Courier"),
                                       uitest::sha256Hex(rom), rom.size());
      if (withPersonal) {
        g.insert(QStringLiteral("personal"),
                 id == QLatin1String("g1") ? personal(43200, 9, QStringLiteral("2026-10-20T10:00:00Z"), true, {"c1"})
                 : id == QLatin1String("g2") ? personal(2700, 2, QStringLiteral("2026-10-22T10:00:00Z"), false, {"c1", "c2"})
                                             : personal(0, 0, QString(), false, {}));
      }
      games.append(g);
    }
    hub.games = QJsonObject{{QStringLiteral("games"), games}};
    hub.collectionsUnsupported = !withPersonal;
    if (withPersonal) {
      hub.collections = QJsonArray{
          QJsonObject{{"id", "c1"}, {"name", "Couch co-op"}, {"game_ids", QJsonArray{"g1", "g2"}}, {"created_at", "2026-10-01T10:00:00Z"}},
          QJsonObject{{"id", "c2"}, {"name", "Nuzlocke"}, {"game_ids", QJsonArray{"g2"}}, {"created_at", "2026-10-02T10:00:00Z"}}};
      hub.favoriteGames = {QStringLiteral("g1")};
    } else {
      hub.collectionsUnsupported = true;
    }
  }
  static bool shown(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr && it->isVisible();
  }
  static QString textOf(Harness& h, const char* name) {
    QQuickItem* it = h.item(name);
    return it != nullptr ? it->property("text").toString() : QString();
  }
  static QStringList order(Harness& h) {
    QStringList out;
    LibraryModel* m = h.controller->library();
    for (int i = 0; i < m->rowCount(); ++i) out << m->data(m->index(i, 0), LibraryModel::GameIdRole).toString();
    return out;
  }
  // The collection control (3c-1-r6) is a select with a menu: open it and choose an entry by its value.
  static bool pickCollection(Harness& h, const char* value) {
    if (!h.click("collectionSelect")) return false;
    const QByteArray name = QByteArray("menuItem_collection_") + value;
    if (!QTest::qWaitFor([&]() { return shown(h, name.constData()); }, 3000)) return false;
    return h.click(name.constData());
  }
  static bool newCollectionEntry(Harness& h) {
    if (!h.click("collectionSelect")) return false;
    if (!QTest::qWaitFor([&]() { return shown(h, "menuItem_collectionNew"); }, 3000)) return false;
    return h.click("menuItem_collectionNew");
  }
  // "Label · count" of every entry of the collection menu.
  static QStringList collectionLabels(Harness& h) {
    QStringList out;
    QObject* picker = h.window->findChild<QObject*>(QStringLiteral("collectionPicker"));
    if (picker == nullptr) return out;
    for (const QVariant& v : picker->property("items").toList()) {
      if (v.toMap().contains(QStringLiteral("label")) && v.toMap().value(QStringLiteral("value")).toString() != QLatin1String("__new")) out << v.toMap().value(QStringLiteral("label")).toString();
    }
    return out;
  }
  static int requests(FakeHub& hub, const QString& path, const char* method) { return hub.count(path, method); }
  static void selectGame(Harness& h, const QString& id) {
    h.controller->selectGame(id);
    QTRY_COMPARE_WITH_TIMEOUT(h.controller->selectedGameId(), id, 3000);
  }

 private slots:
  void initTestCase() {
    qunsetenv("UZUME_MELONDS_DS_CORE");
    uitest::installWarningCounter();
  }
  void init() { uitest::warningCount() = 0; }
  void cleanup() { QCOMPARE(uitest::warningCount().load(), 0); }

  void chipsAndColumnsFollowTheServer() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "collectionSelect"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(collectionLabels(h).size() == 4, 5000);
    const QStringList labels = collectionLabels(h);
    QCOMPARE(labels.value(0), QStringLiteral("All collections · 3"));
    QCOMPARE(labels.value(1), QStringLiteral("Favourites · 1"));
    QVERIFY(labels.contains(QStringLiteral("Couch co-op · 2")));
    // Recently played is the default: g2 (Oct 22), g1 (Oct 20), then the never played game
    QCOMPARE(h.controller->library()->sortKey(), QStringLiteral("played"));
    QCOMPARE(order(h), (QStringList{"g2", "g1", "g3"}));
    // Columns: play time and last played
    const auto visibleTexts = [&](const char* name) {
      QMap<qreal, QString> byY;  // the rows top to bottom (the hidden second grid has no visible rows)
      for (QQuickItem* t : h.items(name)) {
        if (t->isVisible() && t->window() == h.window) byY.insert(t->mapToScene(QPointF(0, 0)).y(), t->property("text").toString());
      }
      return QStringList(byY.values());
    };
    QTRY_COMPARE(visibleTexts("tileLastPlayed").size(), 3);
    const QStringList last = visibleTexts("tileLastPlayed");
    QCOMPARE(last.last(), QStringLiteral("–"));
    QVERIFY(last.first() != QStringLiteral("–"));
    uitest::saveShot(h.window, QStringLiteral("library-3c-0"));

    // Chips filter
    QVERIFY(pickCollection(h, "favorites"));
    QTRY_COMPARE(order(h), QStringList{"g1"});
    QVERIFY(pickCollection(h, "c2"));
    QTRY_COMPARE(order(h), QStringList{"g2"});
    QVERIFY(shown(h, "collectionMenuButton"));
    QVERIFY(pickCollection(h, "all"));
    QTRY_COMPARE(order(h).size(), 3);
    QVERIFY(!shown(h, "collectionMenuButton"));

    // Sort by play time
    h.controller->library()->setSortKey(QStringLiteral("playtime"));
    QCOMPARE(order(h), (QStringList{"g1", "g2", "g3"}));
  }

  void newCollectionDialogCreatesAndReportsAUsedName() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "collectionSelect") && collectionLabels(h).size() == 4, 5000);
    QVERIFY(newCollectionEntry(h));
    QTRY_VERIFY(shown(h, "collectionNameDialog"));
    QVERIFY(!h.item("collectionNameConfirm")->isEnabled());
    h.item("collectionNameField")->setProperty("text", QStringLiteral("nuzlocke"));  // used (any case)
    QVERIFY(h.click("collectionNameConfirm"));
    QTRY_VERIFY(h.item("dialogError") != nullptr && h.item("dialogError")->isVisible());
    QCOMPARE(requests(hub, QStringLiteral("/api/v1/collections"), "POST"), 0);  // refused before any request
    h.item("collectionNameField")->setProperty("text", QStringLiteral("Speedruns"));
    QVERIFY(h.click("collectionNameConfirm"));
    QTRY_VERIFY(!shown(h, "collectionNameDialog"));
    QCOMPARE(requests(hub, QStringLiteral("/api/v1/collections"), "POST"), 1);
    QTRY_VERIFY(collectionLabels(h).contains(QStringLiteral("Speedruns · 0")));
    // The server says 409: shown in the dialog
    hub.collections.append(QJsonObject{{"id", "c9"}, {"name", "Taken elsewhere"}, {"game_ids", QJsonArray()}, {"created_at", "x"}});
    QVERIFY(newCollectionEntry(h));
    QTRY_VERIFY(shown(h, "collectionNameDialog"));
    h.item("collectionNameField")->setProperty("text", QStringLiteral("Taken elsewhere"));
    QVERIFY(h.click("collectionNameConfirm"));
    QTRY_VERIFY(h.item("dialogError") != nullptr && h.item("dialogError")->isVisible());
    QVERIFY(shown(h, "collectionNameDialog"));
    QVERIFY(h.click("collectionNameCancel"));
    QTRY_VERIFY(!shown(h, "collectionNameDialog"));
  }

  void collectionMenuRenamesAndDeletesWithoutDeletingGames() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(collectionLabels(h).contains(QStringLiteral("Couch co-op · 2")), 5000);
    QVERIFY(pickCollection(h, "c1"));
    QTRY_VERIFY(shown(h, "collectionMenuButton"));
    // Rename
    // The menu is a Popup: open it with its button, choose through its `chosen` signal.
    QVERIFY(h.click("collectionMenuButton"));
    QObject* menu = h.window->findChild<QObject*>(QStringLiteral("collectionMenu"));
    QVERIFY(menu != nullptr);
    QVariantMap renameItem{{"label", "Rename..."}};
    QVERIFY(QMetaObject::invokeMethod(menu, "chosen", Q_ARG(QVariant, renameItem), Q_ARG(int, 0)));
    QTRY_VERIFY(shown(h, "collectionNameDialog"));
    QCOMPARE(textOf(h, "collectionNameField"), QStringLiteral("Couch co-op"));
    h.item("collectionNameField")->setProperty("text", QStringLiteral("Couch"));
    QVERIFY(h.click("collectionNameConfirm"));
    QTRY_VERIFY(!shown(h, "collectionNameDialog"));
    QCOMPARE(requests(hub, QStringLiteral("/api/v1/collections/c1"), "PATCH"), 1);
    QTRY_VERIFY(collectionLabels(h).contains(QStringLiteral("Couch · 2")));
    // Delete: the confirm says games stay
    QVariantMap deleteItem{{"label", "Delete collection..."}};
    QVERIFY(QMetaObject::invokeMethod(menu, "chosen", Q_ARG(QVariant, deleteItem), Q_ARG(int, 2)));
    QTRY_VERIFY(shown(h, "collectionDeleteDialog"));
    QVERIFY(h.item("collectionDeleteDialog")->property("text").toString().contains(QStringLiteral("never deletes games")));
    QVERIFY(h.click("collectionDeleteConfirm"));
    QTRY_VERIFY(!shown(h, "collectionDeleteDialog"));
    QCOMPARE(requests(hub, QStringLiteral("/api/v1/collections/c1"), "DELETE"), 1);
    QTRY_VERIFY(!collectionLabels(h).contains(QStringLiteral("Couch · 2")));
    QCOMPARE(h.controller->library()->collectionFilter(), QStringLiteral("all"));  // the open chip went away: back to All
    QCOMPARE(order(h).size(), 3);                                                    // every game is still there
  }

  void emptyStates() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    hub.favoriteGames.clear();
    QJsonArray games = hub.games.value("games").toArray();
    for (int i = 0; i < games.size(); ++i) {
      QJsonObject g = games.at(i).toObject();
      QJsonObject p = g.value("personal").toObject();
      p.insert("favorite", false);
      g.insert("personal", p);
      games.replace(i, g);
    }
    hub.games = QJsonObject{{"games", games}};
    hub.collections.append(QJsonObject{{"id", "c3"}, {"name", "Empty one"}, {"game_ids", QJsonArray()}, {"created_at", "x"}});
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(collectionLabels(h).contains(QStringLiteral("Empty one · 0")), 5000);
    QVERIFY(pickCollection(h, "favorites"));
    QTRY_VERIFY(shown(h, "libraryEmptyText"));
    QCOMPARE(h.item("libraryEmptyText")->property("title").toString(), QStringLiteral("No favourites yet."));
    QVERIFY(textOf(h, "collectionEmptyHint").contains(QStringLiteral("Favourites are per person and follow you to every PC.")));
    QVERIFY(!shown(h, "collectionEmptyActions"));
    QVERIFY(pickCollection(h, "c3"));
    QTRY_COMPARE(h.item("libraryEmptyText")->property("title").toString(), QStringLiteral("This collection is empty."));
    QVERIFY(shown(h, "emptyCollectionRename") && shown(h, "emptyCollectionDelete"));
    QVERIFY(h.click("emptyCollectionDelete"));
    QTRY_VERIFY(shown(h, "collectionDeleteDialog"));
    QVERIFY(h.click("collectionDeleteCancel"));
    QTRY_VERIFY(!shown(h, "collectionDeleteDialog"));
    QVERIFY(h.click("emptyShowAllGames"));
    QTRY_COMPARE(order(h).size(), 3);
  }

  void gamePageFavouriteFactsAndAddToCollection() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    selectGame(h, QStringLiteral("g2"));
    QTRY_VERIFY(shown(h, "personalFacts"));
    QCOMPARE(textOf(h, "factPlayTime"), QStringLiteral("45 min"));
    QCOMPARE(textOf(h, "factSessions"), QStringLiteral("2"));
    QTRY_COMPARE(textOf(h, "factCollections"), QStringLiteral("Couch co-op, Nuzlocke"));
    QVERIFY(textOf(h, "factLastPlayed") != QStringLiteral("–"));
    uitest::saveShot(h.window, QStringLiteral("game-page-3c-1"));
    // Favourite toggle: PUT then DELETE, the state shows at once
    QVERIFY(shown(h, "favoriteButton"));
    QVERIFY(!h.item("favoriteButton")->property("on").toBool());
    QVERIFY(h.click("favoriteButton"));
    QTRY_VERIFY(h.item("favoriteButton")->property("on").toBool());
    QTRY_COMPARE(requests(hub, QStringLiteral("/api/v1/games/g2/favorite"), "PUT"), 1);
    QVERIFY(hub.favoriteGames.contains(QStringLiteral("g2")));
    QVERIFY(collectionLabels(h).contains(QStringLiteral("Favourites · 2")));
    QVERIFY(h.click("favoriteButton"));
    QTRY_VERIFY(!h.item("favoriteButton")->property("on").toBool());
    QTRY_COMPARE(requests(hub, QStringLiteral("/api/v1/games/g2/favorite"), "DELETE"), 1);
    // A refused change is taken back
    hub.collectionFailNext = 1;
    QVERIFY(h.click("favoriteButton"));
    QTRY_COMPARE(requests(hub, QStringLiteral("/api/v1/games/g2/favorite"), "PUT"), 2);
    QTRY_VERIFY(!h.item("favoriteButton")->property("on").toBool());

    // Add to collection: checkboxes, then New collection
    QVERIFY(h.click("addToCollection"));
    QTRY_VERIFY(shown(h, "collectionAddDialog"));
    QVERIFY(h.item("collectionCheck_c1")->property("checked").toBool());
    QVERIFY(h.item("collectionCheck_c2")->property("checked").toBool());
    QVERIFY(h.click("collectionCheck_c2"));
    QTRY_COMPARE(requests(hub, QStringLiteral("/api/v1/collections/c2/games/g2"), "DELETE"), 1);
    QTRY_VERIFY(!h.item("collectionCheck_c2")->property("checked").toBool());
    QTRY_COMPARE(textOf(h, "factCollections"), QStringLiteral("Couch co-op"));
    QVERIFY(h.click("collectionCheck_c2"));
    QTRY_COMPARE(requests(hub, QStringLiteral("/api/v1/collections/c2/games/g2"), "PUT"), 1);
    QVERIFY(h.click("collectionAddNew"));
    QTRY_VERIFY(shown(h, "collectionNameDialog"));
    h.item("collectionNameField")->setProperty("text", QStringLiteral("Favourites of May"));
    QVERIFY(h.click("collectionNameConfirm"));
    QTRY_VERIFY(!shown(h, "collectionNameDialog"));
    QTRY_VERIFY(h.item("collectionCheck_c3") != nullptr && h.item("collectionCheck_c3")->property("checked").toBool());  // the new one holds the game
    QCOMPARE(hub.collections.last().toObject().value("game_ids").toArray().at(0).toString(), QStringLiteral("g2"));
    QVERIFY(h.click("collectionAddDone"));
    QTRY_VERIFY(!shown(h, "collectionAddDialog"));
  }

  // Runs: play time of the run in the table, play time and last played per person on the run board.
  void runsShowPlayTime() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    hub.features = kFeatures + QStringList{QStringLiteral("runs_v1"), QStringLiteral("runs_v2")};
    QJsonObject run = hub.makeRun(QStringLiteral("Ember Route"), {hub.runsMe, QStringLiteral("u_lena")}, hub.runsMe);
    QJsonArray ps = run.value("participants").toArray();
    QJsonObject me = ps.at(0).toObject(), lena = ps.at(1).toObject();
    me.insert("play_seconds", 5400);
    me.insert("last_played_at", QStringLiteral("2026-10-22T10:00:00Z"));
    lena.insert("status", QStringLiteral("joined"));
    lena.insert("user_name", QStringLiteral("Lena"));
    lena.insert("color", 1);
    lena.insert("play_seconds", 1800);
    lena.insert("last_played_at", QJsonValue::Null);
    ps.replace(0, me);
    ps.replace(1, lena);
    run.insert("participants", ps);
    run.insert("play_seconds", 7200);
    const QString runId = run.value("run_id").toString();
    hub.runs.insert(runId, run);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    RunsController* runs = h.controller->runs();
    QTRY_VERIFY_WITH_TIMEOUT(runs->loaded() && runs->available(), 8000);
    QTRY_COMPARE_WITH_TIMEOUT(runs->active()->rowCount(), 1, 8000);
    auto* active = qobject_cast<KeyedRowsModel*>(runs->active());
    QVERIFY(active != nullptr);
    QCOMPARE(active->get(0).value("playTimeText").toString(), QStringLiteral("2 h"));
    // The runs table shows the play time of the run
    h.controller->showRuns();
    QTRY_VERIFY_WITH_TIMEOUT(shown(h, "runRowPlayTime"), 5000);
    QCOMPARE(textOf(h, "runRowPlayTime"), QStringLiteral("2 h"));
    runs->openRun(runId);
    auto* parts = qobject_cast<KeyedRowsModel*>(runs->participants());
    QVERIFY(parts != nullptr);
    QTRY_COMPARE(parts->count(), 2);
    QCOMPARE(parts->get(0).value("playTimeText").toString(), QStringLiteral("1.5 h"));
    QVERIFY(parts->get(0).value("lastPlayedText").toString() != QStringLiteral("–"));
    QCOMPARE(parts->get(1).value("playTimeText").toString(), QStringLiteral("30 min"));
    QCOMPARE(parts->get(1).value("lastPlayedText").toString(), QStringLiteral("–"));
  }

  // A run game reports its run with in_game presence; a plain game and the end of the game do not.
  void presenceCarriesTheRunOfARunGame() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    QTRY_VERIFY_WITH_TIMEOUT(h.controller->sessions()->socket()->isOpen(), 8000);
    const auto lastPresence = [&]() {
      for (int i = hub.wsReceived.size() - 1; i >= 0; --i) {
        if (hub.wsReceived.at(i).value("type").toString() == QLatin1String("presence_update")) return hub.wsReceived.at(i).value("payload").toObject();
      }
      return QJsonObject();
    };
    h.controller->sessions()->gameStarted(QStringLiteral("g1"), QStringLiteral("Zephyr Run"), QStringLiteral("run-1"));
    QTRY_COMPARE_WITH_TIMEOUT(lastPresence().value("state").toString(), QStringLiteral("in_game"), 5000);
    QCOMPARE(lastPresence().value("game_id").toString(), QStringLiteral("g1"));
    QCOMPARE(lastPresence().value("run_id").toString(), QStringLiteral("run-1"));
    h.controller->sessions()->gameEnded();
    QTRY_COMPARE_WITH_TIMEOUT(lastPresence().value("state").toString(), QStringLiteral("online"), 5000);
    QVERIFY(!lastPresence().contains(QStringLiteral("run_id")));
    h.controller->sessions()->gameStarted(QStringLiteral("g2"), QStringLiteral("Alpha Garden"));
    QTRY_COMPARE_WITH_TIMEOUT(lastPresence().value("state").toString(), QStringLiteral("in_game"), 5000);
    QVERIFY(!lastPresence().contains(QStringLiteral("run_id")));
    h.controller->sessions()->gameEnded();
  }

  void olderServerShowsNoneOfIt() {
    FakeHub hub(QStringLiteral("a"));
    seed(hub, false);
    QVERIFY(hub.start());
    Harness h;
    QVERIFY(h.start());
    pair(h, hub);
    selectGame(h, QStringLiteral("g1"));
    QTest::qWait(300);
    QVERIFY(!h.controller->personal()->supported());
    QVERIFY(!shown(h, "collectionSelect"));
    QVERIFY(!shown(h, "favoriteButton"));
    QVERIFY(!shown(h, "personalFacts"));
    QVERIFY(!shown(h, "tileLastPlayed"));
    // Today's local last played still orders the list
    h.controller->library()->setLastPlayed({{"g3", 9000}});
    QCOMPARE(order(h).first(), QStringLiteral("g3"));
  }
};

UITEST_MAIN(PersonalUiTest)
#include "personal_ui_test.moc"
