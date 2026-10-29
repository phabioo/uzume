// Library sort, "Ready first" grouping, counts and the empty-result data of LibraryModel (3c-2) without QML.
#include <QSignalSpy>
#include <QtTest>

#include "librarymodel.h"

using namespace uzume;
using namespace uzume::ui;

namespace {

const QString kShaBase(63, QLatin1Char('0'));

GameEntry game(const QString& id, const QString& title, qint64 size, const QString& system, const QString& addedAt) {
  GameEntry g;
  g.id = id;
  g.title = title;
  g.system = system;
  g.romSha256 = kShaBase + id.right(1);  // ids end in a hex digit
  g.romSize = size;
  g.romFilename = id + QStringLiteral(".nds");
  g.addedAt = addedAt;
  return g;
}

RomStatus st(RomState s) {
  RomStatus r;
  r.state = s;
  return r;
}

QStringList ids(const QAbstractItemModel& m) {
  QStringList out;
  for (int i = 0; i < m.rowCount(); ++i) {
    out << m.data(m.index(i, 0), LibraryModel::GameIdRole).toString();
  }
  return out;
}

// Fixture: 6 games. a1 Zelda-like (largest, oldest), ...; statuses make a1, a3, a5 ready.
//   id  title          size  system  added       state
//   a1  Zephyr Run     900   nds     2026-01-05  ready
//   a2  alpha Garden   100   gba     2026-03-01  download needed
//   a3  Moon Courier   500   nds     2026-02-01  ready
//   a4  Beta Blocks    300   nds     (invalid)   hash mismatch  -> attention
//   a5  Orbit 10       700   gba     2026-04-01  ready
//   a6  Orbit 2        200   nds     2026-01-20  download needed
void fill(LibraryModel& m) {
  const QList<GameEntry> games{game(QStringLiteral("a1"), QStringLiteral("Zephyr Run"), 900, QStringLiteral("nds"), QStringLiteral("2026-01-05T10:00:00Z")),
                               game(QStringLiteral("a2"), QStringLiteral("alpha Garden"), 100, QStringLiteral("gba"), QStringLiteral("2026-03-01T10:00:00Z")),
                               game(QStringLiteral("a3"), QStringLiteral("Moon Courier"), 500, QStringLiteral("nds"), QStringLiteral("2026-02-01T10:00:00Z")),
                               game(QStringLiteral("a4"), QStringLiteral("Beta Blocks"), 300, QStringLiteral("nds"), QString()),
                               game(QStringLiteral("a5"), QStringLiteral("Orbit 10"), 700, QStringLiteral("gba"), QStringLiteral("2026-04-01T10:00:00Z")),
                               game(QStringLiteral("a6"), QStringLiteral("Orbit 2"), 200, QStringLiteral("nds"), QStringLiteral("2026-01-20T10:00:00Z"))};
  m.setGames(games, [](const GameEntry& g) {
    if (g.id == QLatin1String("a1") || g.id == QLatin1String("a3") || g.id == QLatin1String("a5")) return st(RomState::Ready);
    if (g.id == QLatin1String("a4")) return st(RomState::HashMismatch);
    return st(RomState::DownloadNeeded);
  });
}

}  // namespace

class LibrarySortTest : public QObject {
  Q_OBJECT
 private slots:
  void localOnlyGameShowsLocalAndMissingStates() {
    LibraryModel m;
    GameEntry local = game(QStringLiteral("b1"), QStringLiteral("Local Game"), 100, QStringLiteral("nds"), QString());
    local.onHub = false;
    GameEntry hub = game(QStringLiteral("b2"), QStringLiteral("Server Game"), 100, QStringLiteral("nds"), QString());
    m.setGames({local, hub}, [](const GameEntry& g) {
      return g.onHub ? st(RomState::DownloadNeeded) : st(RomState::Missing);  // not on this device
    });
    QCOMPARE(m.rowCount(), 2);
    const auto row = [&](const QString& id) {
      for (int i = 0; i < m.rowCount(); ++i)
        if (m.data(m.index(i, 0), LibraryModel::GameIdRole).toString() == id) return i;
      return -1;
    };
    const int l = row(QStringLiteral("b1")), h = row(QStringLiteral("b2"));
    QVERIFY(!m.data(m.index(l, 0), LibraryModel::OnHubRole).toBool());
    QVERIFY(m.data(m.index(h, 0), LibraryModel::OnHubRole).toBool());
    QCOMPARE(m.data(m.index(l, 0), LibraryModel::StateKindRole).toString(), QStringLiteral("missing"));
    QVERIFY(m.data(m.index(l, 0), LibraryModel::TileTextRole).toString().contains(QStringLiteral("missing")));
    QVERIFY(m.data(m.index(l, 0), LibraryModel::NeedsAttentionRole).toBool());
    // The local file shows up (folder scan): the same game turns ready and keeps the Local state
    m.setGames({local, hub}, [](const GameEntry& g) { return g.onHub ? st(RomState::DownloadNeeded) : st(RomState::Ready); });
    QCOMPARE(m.data(m.index(row(QStringLiteral("b1")), 0), LibraryModel::StateKindRole).toString(), QStringLiteral("ready"));
    QVERIFY(!m.data(m.index(row(QStringLiteral("b1")), 0), LibraryModel::OnHubRole).toBool());
  }

  void defaultIsRecentlyPlayedAndFallsBackToName() {
    LibraryModel m;
    fill(m);
    QCOMPARE(m.sortKey(), QStringLiteral("played"));
    // Nothing played: Name order. Case-insensitive, numeric-aware: "Orbit 2" before "Orbit 10"
    QCOMPARE(ids(m), (QStringList{"a2", "a4", "a3", "a6", "a5", "a1"}));
  }

  void everySortKeyBothDirections() {
    LibraryModel m;
    fill(m);
    QSignalSpy sorted(&m, &LibraryModel::sortChanged);
    const auto check = [&](const char* key, const QStringList& expected) {
      m.setSortKey(QString::fromLatin1(key));
      QCOMPARE(m.sortKey(), QString::fromLatin1(key));
      QVERIFY2(ids(m) == expected, qPrintable(QStringLiteral("%1: got %2").arg(QString::fromLatin1(key), ids(m).join(QLatin1Char(',')))));
    };
    check("name_desc", {"a1", "a5", "a6", "a3", "a4", "a2"});
    check("name_asc", {"a2", "a4", "a3", "a6", "a5", "a1"});
    // Date added; a4 has no valid date (0) and is the oldest
    check("added_desc", {"a5", "a2", "a3", "a6", "a1", "a4"});
    check("added_asc", {"a4", "a1", "a6", "a3", "a2", "a5"});
    // Size, both directions (decision ab)
    check("size_desc", {"a1", "a5", "a3", "a4", "a6", "a2"});
    check("size_asc", {"a2", "a6", "a4", "a3", "a5", "a1"});
    // System, then name: gba (alpha Garden, Orbit 10), nds (Beta, Moon, Orbit 2, Zephyr)
    check("system", {"a2", "a5", "a4", "a3", "a6", "a1"});
    QVERIFY(sorted.count() >= 7);
    m.setSortKey(QStringLiteral("bogus"));  // unknown keys are ignored
    QCOMPARE(m.sortKey(), QStringLiteral("system"));
  }

  void lastPlayedNeverPlayedComesLast() {
    LibraryModel m;
    fill(m);
    m.setSortKey(QStringLiteral("played"));
    // Nothing played yet: name order
    QCOMPARE(ids(m), (QStringList{"a2", "a4", "a3", "a6", "a5", "a1"}));
    m.setLastPlayed({{QStringLiteral("a6"), 3000}, {QStringLiteral("a1"), 9000}});
    QCOMPARE(ids(m), (QStringList{"a1", "a6", "a2", "a4", "a3", "a5"}));
    m.noteLastPlayed(QStringLiteral("a5"), 12000);  // a game starts: it jumps to the front
    QCOMPARE(ids(m), (QStringList{"a5", "a1", "a6", "a2", "a4", "a3"}));
    // Other sorts ignore it
    m.setSortKey(QStringLiteral("name_asc"));
    QCOMPARE(ids(m).first(), QStringLiteral("a2"));
  }

  void addedTextRole() {
    LibraryModel m;
    fill(m);
    const int row = m.rowOfGame(QStringLiteral("a2"));
    const QString text = m.data(m.index(row), LibraryModel::AddedTextRole).toString();
    QVERIFY2(QRegularExpression(QStringLiteral("^\\d\\d\\.\\d\\d\\.2026$")).match(text).hasMatch(), qPrintable(text));
    QVERIFY(m.data(m.index(m.rowOfGame(QStringLiteral("a4"))), LibraryModel::AddedTextRole).toString().isEmpty());
  }

  void readyFirstGroupsAndKeepsTheSortInsideGroups() {
    LibraryModel m;
    fill(m);
    QVERIFY(!m.readyFirst());
    QVERIFY(!m.grouped());
    m.setReadyFirst(true);
    // Ready: a3 Moon, a5 Orbit 10, a1 Zephyr (name order); not ready: a2, a4, a6 (name order)
    QCOMPARE(ids(m), (QStringList{"a3", "a5", "a1", "a2", "a4", "a6"}));
    QVERIFY(m.grouped());
    QCOMPARE(m.readyGroupCount(), 3);
    QCOMPARE(m.notReadyGroupCount(), 3);
    // Same sort inside both groups
    m.setSortKey(QStringLiteral("size_desc"));
    QCOMPARE(ids(m), (QStringList{"a1", "a5", "a3", "a4", "a6", "a2"}));
    m.setSortKey(QStringLiteral("name_desc"));
    QCOMPARE(ids(m), (QStringList{"a1", "a5", "a3", "a6", "a4", "a2"}));
    // Group roles
    QVERIFY(m.data(m.index(0), LibraryModel::ReadyGroupRole).toBool());
    QVERIFY(!m.data(m.index(3), LibraryModel::ReadyGroupRole).toBool());
    m.setReadyFirst(false);
    QVERIFY(!m.grouped());
    QCOMPARE(ids(m), (QStringList{"a1", "a5", "a6", "a3", "a4", "a2"}));
  }

  void readyFirstWithFilterAndSearch() {
    LibraryModel m;
    fill(m);
    m.setReadyFirst(true);
    // Search "i": Moon Courier (a3, ready), Orbit 10 (a5, ready), Orbit 2 (a6, not ready)
    m.setFilterText(QStringLiteral("i"));
    QCOMPARE(ids(m), (QStringList{"a3", "a5", "a6"}));
    QVERIFY(m.grouped());
    QCOMPARE(m.readyGroupCount(), 2);
    QCOMPARE(m.notReadyGroupCount(), 1);
    // Chip "Ready": every result is ready, grouping is not applicable and no divider is shown
    m.setFilter(QStringLiteral("ready"));
    QVERIFY(!m.readyFirstApplicable());
    QVERIFY(!m.grouped());
    QCOMPARE(ids(m), (QStringList{"a3", "a5"}));
    // Chip "Not downloaded": only one group, no divider although Ready first is on
    m.setFilter(QStringLiteral("download"));
    QVERIFY(m.readyFirstApplicable());
    QVERIFY(!m.grouped());
    QCOMPARE(ids(m), (QStringList{"a6"}));
    // Chip "All" again with the search cleared
    m.setFilter(QStringLiteral("all"));
    m.setFilterText(QString());
    QVERIFY(m.grouped());
    QCOMPARE(ids(m).size(), 6);
  }

  void syncPendingIsNotReady() {
    LibraryModel m;
    fill(m);
    m.setReadyFirst(true);
    QVERIFY(m.isReady(QStringLiteral("a3")));
    m.setSyncKind(QStringLiteral("a3"), QStringLiteral("pending"));  // decision aa
    QVERIFY(!m.isReady(QStringLiteral("a3")));
    QCOMPARE(ids(m), (QStringList{"a5", "a1", "a2", "a4", "a3", "a6"}));
    QCOMPARE(m.notReadyGroupCount(), 4);
    m.setSyncKind(QStringLiteral("a3"), QStringLiteral("synced"));
    QVERIFY(m.isReady(QStringLiteral("a3")));
    QCOMPARE(m.readyGroupCount(), 3);
    // A status change moves a game between the groups too
    m.setStatus(m.game(QStringLiteral("a2"))->romSha256, st(RomState::Ready));
    QCOMPARE(ids(m), (QStringList{"a2", "a3", "a5", "a1", "a4", "a6"}));
  }

  void progressUpdatesDoNotRebuildTheModel() {
    LibraryModel m;
    fill(m);
    const QString sha = m.game(QStringLiteral("a2"))->romSha256;
    RomStatus d = st(RomState::Downloading);
    d.totalBytes = 100;
    d.receivedBytes = 10;
    m.setStatus(sha, d);  // state change: may rebuild
    QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&m, &QAbstractItemModel::dataChanged);
    QSignalSpy count(&m, &LibraryModel::countChanged);
    d.receivedBytes = 50;
    m.setStatus(sha, d);  // progress only
    QCOMPARE(reset.count(), 0);
    QCOMPARE(count.count(), 0);
    QCOMPARE(changed.count(), 1);
    QVERIFY(changed.first().at(2).value<QList<int>>().contains(LibraryModel::ProgressRole));
    const int row = m.rowOfGame(QStringLiteral("a2"));
    QCOMPARE(m.data(m.index(row, 0), LibraryModel::ProgressRole).toDouble(), 0.5);
  }

  void setSyncKindsRebuildsOnce() {
    LibraryModel m;
    fill(m);
    m.setReadyFirst(true);
    QSignalSpy reset(&m, &QAbstractItemModel::modelReset);
    QHash<QString, QString> kinds;
    kinds.insert(QStringLiteral("a1"), QStringLiteral("pending"));
    kinds.insert(QStringLiteral("a3"), QStringLiteral("pending"));
    kinds.insert(QStringLiteral("a5"), QStringLiteral("synced"));
    m.setSyncKinds(kinds);
    QVERIFY(reset.count() <= 1);
    QCOMPARE(m.syncKind(QStringLiteral("a1")), QStringLiteral("pending"));
    QCOMPARE(m.syncKind(QStringLiteral("a5")), QStringLiteral("synced"));
    QVERIFY(!m.isReady(QStringLiteral("a3")));
    QCOMPARE(m.readyGroupCount(), 1);
  }

  void continueEntriesFollowLastPlayed() {
    LibraryModel m;
    const QDate today(2026, 10, 23);
    const auto at = [](int y, int mo, int d) { return QDateTime(QDate(y, mo, d), QTime(12, 0)).toMSecsSinceEpoch(); };
    QVERIFY(m.continueEntriesAt(today).isEmpty());
    fill(m);
    QVERIFY(m.continueEntriesAt(today).isEmpty());  // never played: no block

    m.setLastPlayed({{QStringLiteral("a3"), at(2026, 10, 16)}, {QStringLiteral("a1"), at(2026, 10, 22)},
                     {QStringLiteral("a5"), at(2026, 10, 23)}, {QStringLiteral("a2"), at(2026, 10, 1)},
                     {QStringLiteral("gone"), at(2026, 10, 23) + 1000}});
    QSignalSpy changed(&m, &LibraryModel::continueChanged);
    QVariantList e = m.continueEntriesAt(today);
    QCOMPARE(e.size(), 3);  // 1 + 2, "gone" is not in the library
    QCOMPARE(e[0].toMap().value(QStringLiteral("gameId")).toString(), QStringLiteral("a5"));
    QCOMPARE(e[0].toMap().value(QStringLiteral("meta")).toString(), QStringLiteral("GBA"));
    QCOMPARE(e[1].toMap().value(QStringLiteral("gameId")).toString(), QStringLiteral("a1"));
    QCOMPARE(e[1].toMap().value(QStringLiteral("dateText")).toString(), QStringLiteral("YESTERDAY"));
    QCOMPARE(e[2].toMap().value(QStringLiteral("gameId")).toString(), QStringLiteral("a3"));
    QCOMPARE(e[2].toMap().value(QStringLiteral("dateText")).toString(), QStringLiteral("16 OCT"));
    QCOMPARE(e[0].toMap().value(QStringLiteral("dateText")).toString(), QStringLiteral("TODAY"));

    // The newest save of a game joins the system in the meta line (issue 291); a game without one keeps the system only.
    m.setSaveMeta({{QStringLiteral("a5"), QStringLiteral("Main r14 · Desktop-LivingRoom")}});
    QVERIFY(changed.count() >= 1);
    e = m.continueEntriesAt(today);
    QCOMPARE(e[0].toMap().value(QStringLiteral("meta")).toString(), QStringLiteral("GBA · Main r14 · Desktop-LivingRoom"));
    QCOMPARE(e[1].toMap().value(QStringLiteral("meta")).toString(), e[1].toMap().value(QStringLiteral("system")).toString());
    m.setSaveMeta({});

    // Launching a game moves it to the front.
    m.noteLastPlayed(QStringLiteral("a3"), at(2026, 10, 23) + 5000);
    QVERIFY(changed.count() >= 1);
    e = m.continueEntriesAt(today);
    QCOMPARE(e[0].toMap().value(QStringLiteral("gameId")).toString(), QStringLiteral("a3"));
    QCOMPARE(e.size(), 3);
  }

  void groupProxySplitsTheModel() {
    LibraryModel m;
    fill(m);
    m.setReadyFirst(true);
    LibraryGroupModel ready, rest, all;
    ready.setLibrary(&m);
    rest.setLibrary(&m);
    all.setLibrary(&m);
    ready.setGroup(0);
    rest.setGroup(1);
    all.setGroup(-1);
    QCOMPARE(ids(ready), (QStringList{"a3", "a5", "a1"}));
    QCOMPARE(ids(rest), (QStringList{"a2", "a4", "a6"}));
    QCOMPARE(ids(all).size(), 6);
    m.setSortKey(QStringLiteral("size_asc"));
    QCOMPARE(ids(ready), (QStringList{"a3", "a5", "a1"}));
    QCOMPARE(ids(rest), (QStringList{"a2", "a6", "a4"}));
    m.setStatus(m.game(QStringLiteral("a6"))->romSha256, st(RomState::Ready));
    QCOMPARE(ids(ready), (QStringList{"a6", "a3", "a5", "a1"}));
    QCOMPARE(ids(rest), (QStringList{"a2", "a4"}));
  }

  void countsAndToolbarTextFollowFilterAndSearch() {
    LibraryModel m;
    fill(m);
    m.setSystemLabel(QStringLiteral("Nintendo DS"));
    QCOMPARE(m.countText(), QStringLiteral("6 games · Nintendo DS"));
    m.setFilter(QStringLiteral("ready"));
    QCOMPARE(m.countText(), QStringLiteral("3 of 6 games"));  // decision ac
    m.setFilterText(QStringLiteral("zzz"));
    QCOMPARE(m.countText(), QStringLiteral("0 of 3 ready games"));
    // Chips recount to the search result
    QCOMPARE(m.counts().value(QStringLiteral("all")).toInt(), 0);
    m.setFilterText(QStringLiteral("beta"));
    QCOMPARE(m.counts().value(QStringLiteral("all")).toInt(), 1);
    QCOMPARE(m.counts().value(QStringLiteral("ready")).toInt(), 0);
    QCOMPARE(m.counts().value(QStringLiteral("attention")).toInt(), 1);
    QCOMPARE(m.countText(), QStringLiteral("0 of 3 ready games"));
    // The only match is outside the filter
    const QVariantMap out = m.outsideFilter();
    QCOMPARE(out.value(QStringLiteral("count")).toInt(), 1);
    QCOMPARE(out.value(QStringLiteral("title")).toString(), QStringLiteral("Beta Blocks"));
    QCOMPARE(out.value(QStringLiteral("reason")).toString(), QStringLiteral("needs attention · hash mismatch"));
    m.setFilter(QStringLiteral("all"));
    QVERIFY(m.outsideFilter().isEmpty());
    QCOMPARE(m.countText(), QStringLiteral("1 of 6 games"));
    m.setFilterText(QString());
    QCOMPARE(m.countText(), QStringLiteral("6 games · Nintendo DS"));
    m.setSystemLabel(QString());
    QCOMPARE(m.countText(), QStringLiteral("6 games"));
  }

  // ---- per person (Uzume 0.19) ----

  // a1 12 h, last played Oct 20, favourite, in c1; a2 45 min, Oct 22, c1 + c2; a3 never; a4 2 h (7200 s), Oct 21, favourite;
  // a5/a6 never played.
  static void fillPersonal(LibraryModel& m) {
    const auto pers = [](qint64 secs, int sessions, const char* last, bool fav, QStringList cols) {
      GamePersonal p;
      p.known = true;
      p.playSeconds = secs;
      p.sessions = sessions;
      p.lastPlayedAt = QString::fromLatin1(last);
      p.favorite = fav;
      p.collectionIds = cols;
      return p;
    };
    QList<GameEntry> games{game(QStringLiteral("a1"), QStringLiteral("Zephyr Run"), 900, QStringLiteral("nds"), QString()),
                           game(QStringLiteral("a2"), QStringLiteral("alpha Garden"), 100, QStringLiteral("gba"), QString()),
                           game(QStringLiteral("a3"), QStringLiteral("Moon Courier"), 500, QStringLiteral("nds"), QString()),
                           game(QStringLiteral("a4"), QStringLiteral("Beta Blocks"), 300, QStringLiteral("nds"), QString()),
                           game(QStringLiteral("a5"), QStringLiteral("Orbit 10"), 700, QStringLiteral("gba"), QString()),
                           game(QStringLiteral("a6"), QStringLiteral("Orbit 2"), 200, QStringLiteral("nds"), QString())};
    games[0].personal = pers(43200, 9, "2026-10-20T10:00:00Z", true, {"c1"});
    games[1].personal = pers(2700, 2, "2026-10-22T10:00:00Z", false, {"c1", "c2"});
    games[2].personal = pers(0, 0, "", false, {});
    games[3].personal = pers(7200, 3, "2026-10-21T10:00:00Z", true, {});
    games[4].personal = pers(0, 0, "", false, {});
    games[5].personal = pers(0, 0, "", false, {});
    m.setGames(games, [](const GameEntry&) { return st(RomState::Ready); });
    m.setCollections({CollectionInfo{"c1", "Couch co-op", {"a1", "a2"}, {}}, CollectionInfo{"c2", "Nuzlocke", {"a2"}, {}}});
  }

  void personalRolesAndFormats() {
    LibraryModel m;
    fillPersonal(m);
    QVERIFY(m.personalSupported());
    const auto role = [&](const QString& id, int r) {
      for (int i = 0; i < m.rowCount(); ++i)
        if (m.data(m.index(i, 0), LibraryModel::GameIdRole).toString() == id) return m.data(m.index(i, 0), r);
      return QVariant();
    };
    QCOMPARE(role("a1", LibraryModel::PlaySecondsRole).toLongLong(), qint64(43200));
    QCOMPARE(role("a1", LibraryModel::PlayTimeTextRole).toString(), QStringLiteral("12 h"));
    QCOMPARE(role("a2", LibraryModel::PlayTimeTextRole).toString(), QStringLiteral("45 min"));
    QCOMPARE(role("a3", LibraryModel::PlayTimeTextRole).toString(), QStringLiteral("–"));
    QCOMPARE(role("a1", LibraryModel::SessionsRole).toInt(), 9);
    QVERIFY(role("a1", LibraryModel::FavoriteRole).toBool());
    QCOMPARE(role("a2", LibraryModel::CollectionIdsRole).toStringList(), (QStringList{"c1", "c2"}));
    QCOMPARE(role("a3", LibraryModel::LastPlayedTextRole).toString(), QStringLiteral("–"));
    QCOMPARE(LibraryModel::formatPlayTime(30), QStringLiteral("< 1 min"));
    QCOMPARE(LibraryModel::formatPlayTime(5400), QStringLiteral("1.5 h"));
    QCOMPARE(LibraryModel::formatPlayTime(0), QStringLiteral("–"));
    const qint64 now = QDateTime(QDate(2026, 10, 23), QTime(12, 0), Qt::LocalTime).toMSecsSinceEpoch();
    const auto at = [](int d, int h) { return QDateTime(QDate(2026, 10, d), QTime(h, 0), Qt::LocalTime).toMSecsSinceEpoch(); };
    QCOMPARE(LibraryModel::formatLastPlayed(at(23, 1), now), QStringLiteral("Today"));
    QCOMPARE(LibraryModel::formatLastPlayed(at(22, 23), now), QStringLiteral("Yesterday"));
    QCOMPARE(LibraryModel::formatLastPlayed(at(20, 12), now), QStringLiteral("3 days ago"));
    QCOMPARE(LibraryModel::formatLastPlayed(at(10, 12), now), QStringLiteral("10.10.2026"));
    QCOMPARE(LibraryModel::formatLastPlayed(0, now), QStringLiteral("–"));
    const QVariantMap p = m.personalFor(QStringLiteral("a2"));
    QCOMPARE(p.value("collectionNames").toStringList(), (QStringList{"Couch co-op", "Nuzlocke"}));
    QVERIFY(m.personalFor(QStringLiteral("nope")).isEmpty());
  }

  void lastPlayedIsTheNewerOfServerAndLocal() {
    LibraryModel m;
    fillPersonal(m);
    m.setSortKey(QStringLiteral("played"));
    // Server only: a2 (Oct 22), a4 (Oct 21), a1 (Oct 20), then never played in name order
    QCOMPARE(ids(m), (QStringList{"a2", "a4", "a1", "a3", "a6", "a5"}));
    // A local play newer than the server's puts a1 first; an older local play changes nothing
    m.noteLastPlayed(QStringLiteral("a1"), QDateTime(QDate(2026, 10, 23), QTime(9, 0), Qt::UTC).toMSecsSinceEpoch());
    QCOMPARE(ids(m).first(), QStringLiteral("a1"));
    m.noteLastPlayed(QStringLiteral("a2"), QDateTime(QDate(2026, 1, 1), QTime(9, 0), Qt::UTC).toMSecsSinceEpoch());
    QCOMPARE(ids(m).mid(0, 2), (QStringList{"a1", "a2"}));
    // Local only (a game the server never heard of playing): counts too
    m.noteLastPlayed(QStringLiteral("a6"), QDateTime(QDate(2026, 10, 24), QTime(9, 0), Qt::UTC).toMSecsSinceEpoch());
    QCOMPARE(ids(m).first(), QStringLiteral("a6"));
    QCOMPARE(m.data(m.index(0, 0), LibraryModel::LastPlayedMsRole).toLongLong(),
             QDateTime(QDate(2026, 10, 24), QTime(9, 0), Qt::UTC).toMSecsSinceEpoch());
  }

  // The Continue block uses the merged last played: a game played on another PC (server only) shows, and a server update
  // (the reload after a game) announces it.
  void continueUsesServerLastPlayed() {
    LibraryModel m;
    fill(m);
    QSignalSpy spy(&m, &LibraryModel::continueChanged);
    QVERIFY(m.continueEntriesAt(QDate(2026, 10, 23)).isEmpty());  // nothing played anywhere
    QList<GameEntry> games;
    for (int i = 0; i < m.rowCount(); ++i) games.append(*m.game(m.data(m.index(i, 0), LibraryModel::GameIdRole).toString()));
    for (GameEntry& g : games) {
      if (g.id == QLatin1String("a3")) {
        g.personal.known = true;
        g.personal.lastPlayedAt = QStringLiteral("2026-10-22T12:00:00Z");
      }
    }
    spy.clear();
    m.setGames(games, [](const GameEntry&) { return st(RomState::Ready); });
    QVERIFY(spy.count() >= 1);
    const QVariantList e = m.continueEntriesAt(QDate(2026, 10, 23));
    QCOMPARE(e.size(), 1);
    QCOMPARE(e.first().toMap().value("gameId").toString(), QStringLiteral("a3"));
    // A newer local play comes first
    m.noteLastPlayed(QStringLiteral("a1"), QDateTime(QDate(2026, 10, 23), QTime(9, 0), Qt::UTC).toMSecsSinceEpoch());
    QCOMPARE(m.continueEntriesAt(QDate(2026, 10, 23)).first().toMap().value("gameId").toString(), QStringLiteral("a1"));
  }

  void sortByPlayTimeAndPlayedFallsBackToName() {
    LibraryModel m;
    fillPersonal(m);
    m.setSortKey(QStringLiteral("playtime"));
    QCOMPARE(ids(m), (QStringList{"a1", "a4", "a2", "a3", "a6", "a5"}));  // 12 h, 2 h, 45 min, then name
    // Nothing played at all: Recently played is the Name order
    LibraryModel n;
    fill(n);
    n.setSortKey(QStringLiteral("played"));
    QCOMPARE(ids(n), (QStringList{"a2", "a4", "a3", "a6", "a5", "a1"}));
  }

  void collectionFilterAndChips() {
    LibraryModel m;
    fillPersonal(m);
    m.setSortKey(QStringLiteral("name_asc"));
    const auto chips = [&] {
      QStringList out;
      for (const QVariant& v : m.collectionChips()) out << v.toMap().value("label").toString() + " " + v.toMap().value("count").toString();
      return out;
    };
    QCOMPARE(chips(), (QStringList{"All 6", "Favourites 2", "Couch co-op 2", "Nuzlocke 1"}));
    m.setCollectionFilter(QStringLiteral("favorites"));
    QCOMPARE(ids(m), (QStringList{"a4", "a1"}));
    m.setCollectionFilter(QStringLiteral("c1"));
    QCOMPARE(ids(m), (QStringList{"a2", "a1"}));
    // Combined with the status chip and the search
    m.setFilterText(QStringLiteral("zephyr"));
    QCOMPARE(ids(m), QStringList{"a1"});
    m.setFilterText(QString());
    m.setFilter(QStringLiteral("download"));
    QVERIFY(ids(m).isEmpty());
    m.setFilter(QStringLiteral("all"));
    // An unknown collection falls back to All
    m.setCollectionFilter(QStringLiteral("nope"));
    QCOMPARE(m.collectionFilter(), QStringLiteral("all"));
    m.setCollectionFilter(QStringLiteral("c1"));
    // Deleting the open collection returns to All; the games stay
    QSignalSpy filterSpy(&m, &LibraryModel::filterChanged);
    m.setCollections({CollectionInfo{"c2", "Nuzlocke", {"a2"}, {}}});
    QCOMPARE(m.collectionFilter(), QStringLiteral("all"));
    QVERIFY(filterSpy.count() >= 1);
    QCOMPARE(m.rowCount(), 6);
    QVERIFY(m.data(m.index(m.rowOfGame("a1"), 0), LibraryModel::CollectionIdsRole).toStringList().isEmpty());
  }

  void optimisticFavoriteAndMembership() {
    LibraryModel m;
    fillPersonal(m);
    m.setCollectionFilter(QStringLiteral("favorites"));
    QCOMPARE(m.rowCount(), 2);
    m.setFavorite(QStringLiteral("a3"), true);
    QCOMPARE(m.rowCount(), 3);
    m.setFavorite(QStringLiteral("a1"), false);
    QCOMPARE(m.rowCount(), 2);
    m.setCollectionFilter(QStringLiteral("c2"));
    QCOMPARE(m.rowCount(), 1);
    m.setCollectionMember(QStringLiteral("a3"), QStringLiteral("c2"), true);
    QCOMPARE(m.rowCount(), 2);
    QCOMPARE(m.personalFor(QStringLiteral("a3")).value("collectionNames").toStringList(), QStringList{"Nuzlocke"});
    m.setCollectionMember(QStringLiteral("a3"), QStringLiteral("c2"), false);
    QCOMPARE(m.rowCount(), 1);
  }

  void serverWithoutPersonalKeepsLocalLastPlayed() {
    LibraryModel m;
    fill(m);  // no `personal`
    QVERIFY(!m.personalSupported());
    QVERIFY(m.collectionChips().size() == 2);  // only the built-in ones; the screen hides the row
    m.setLastPlayed({{"a5", 5000}, {"a3", 9000}});
    m.setSortKey(QStringLiteral("played"));
    QCOMPARE(ids(m).mid(0, 2), (QStringList{"a3", "a5"}));
    QCOMPARE(m.data(m.index(0, 0), LibraryModel::PlayTimeTextRole).toString(), QStringLiteral("–"));
  }
};

QTEST_GUILESS_MAIN(LibrarySortTest)
#include "library_sort_test.moc"
