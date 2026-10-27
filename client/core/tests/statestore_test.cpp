#include <QTemporaryDir>
#include <QtTest>

#include "profilestore.h"
#include "statestore.h"

using namespace uzume;

namespace {
StateRecord rec(int n, const QString& game = QStringLiteral("g1"), const QString& slot = QStringLiteral("default")) {
  StateRecord r;
  r.gameId = game;
  r.saveSlot = slot;
  r.n = n;
  r.coreId = QStringLiteral("core");
  r.coreVersion = QStringLiteral("1.0");
  return r;
}
}  // namespace

class StateStoreTest : public QObject {
  Q_OBJECT
 private slots:
  void pathsSeparatedByHubAndUser() {
    QTemporaryDir tmp;
    ProfileStore ps(tmp.path());
    const QString a = StateStore::userStatesDir(ps, QStringLiteral("hub-a"), QStringLiteral("u1"));
    const QString b = StateStore::userStatesDir(ps, QStringLiteral("hub-b"), QStringLiteral("u1"));
    QVERIFY(a.endsWith(QStringLiteral("hubs/hub-a/users/u1/states")));
    QVERIFY(a != b);
    QVERIFY(StateStore::userStatesDir(ps, QStringLiteral("hub-a"), QStringLiteral("../x")).isEmpty());
  }

  void slotLogic() {
    QCOMPARE(StateStore::label(0), QStringLiteral("A"));
    QCOMPARE(StateStore::label(3), QStringLiteral("3"));
    QCOMPARE(StateStore::nextSlot(1), 2);
    QCOMPARE(StateStore::nextSlot(4), 1);
    QCOMPARE(StateStore::nextSlot(0), 1);
    QVERIFY(StateStore::validNumber(0) && StateStore::validNumber(4));
    QVERIFY(!StateStore::validNumber(5) && !StateStore::validNumber(-1));
    QList<StateRecord> have{rec(1), rec(3)};
    QCOMPARE(StateStore::firstFreeSlot(have), 2);
    have << rec(2) << rec(4);
    QCOMPARE(StateStore::firstFreeSlot(have), 0);
    have[1].deletePending = true;  // the third entry (n=3) no longer counts
    QCOMPARE(StateStore::firstFreeSlot(have), 3);
  }

  void putListRecordRemove() {
    QTemporaryDir tmp;
    StateStore s(tmp.path());
    QVERIFY(s.put(rec(2), QByteArray("state-two"), QByteArray("png")));
    QVERIFY(s.put(rec(0), QByteArray("auto")));
    const QList<StateRecord> l = s.list(QStringLiteral("g1"), QStringLiteral("default"));
    QCOMPARE(l.size(), 2);
    QCOMPARE(l[0].n, 0);
    QCOMPARE(l[1].n, 2);
    QCOMPARE(l[1].sizeBytes, 9);
    QVERIFY(!l[1].sha256.isEmpty());
    QVERIFY(!l[1].createdAt.isEmpty());
    QVERIFY(l[1].hasPreview);
    QVERIFY(!l[0].hasPreview);
    QCOMPARE(s.state(QStringLiteral("g1"), QStringLiteral("default"), 2), QByteArray("state-two"));
    QCOMPARE(s.preview(QStringLiteral("g1"), QStringLiteral("default"), 2), QByteArray("png"));
    QVERIFY(QFileInfo::exists(s.previewPath(QStringLiteral("g1"), QStringLiteral("default"), 2)));
    QVERIFY(s.previewPath(QStringLiteral("g1"), QStringLiteral("default"), 0).isEmpty());
    // another save slot and another game stay apart
    QVERIFY(s.list(QStringLiteral("g1"), QStringLiteral("run-1")).isEmpty());
    QVERIFY(s.list(QStringLiteral("g2"), QStringLiteral("default")).isEmpty());
    QVERIFY(s.remove(QStringLiteral("g1"), QStringLiteral("default"), 2));
    QVERIFY(!s.remove(QStringLiteral("g1"), QStringLiteral("default"), 2));
    QVERIFY(!s.record(QStringLiteral("g1"), QStringLiteral("default"), 2).has_value());
    QVERIFY(!s.hasBlob(QStringLiteral("g1"), QStringLiteral("default"), 2));
  }

  void putReplacesAndRejectsBadInput() {
    QTemporaryDir tmp;
    StateStore s(tmp.path());
    QVERIFY(s.put(rec(1), QByteArray("one")));
    QVERIFY(s.put(rec(1), QByteArray("uno!")));
    QCOMPARE(s.state(QStringLiteral("g1"), QStringLiteral("default"), 1), QByteArray("uno!"));
    QVERIFY(!s.put(rec(11), QByteArray("x")));
    QVERIFY(!s.put(rec(-1), QByteArray("x")));
    QVERIFY(!s.put(rec(1, QStringLiteral("../evil")), QByteArray("x")));
    QVERIFY(!s.put(rec(1, QStringLiteral("g1"), QStringLiteral("a/b")), QByteArray("x")));
    QVERIFY(!StateStore().put(rec(1), QByteArray("x")));
  }

  void putRecordKeepsBlobWhenShaUnchanged() {
    QTemporaryDir tmp;
    StateStore s(tmp.path());
    QVERIFY(s.put(rec(1), QByteArray("blob"), QByteArray("png")));
    StateRecord r = *s.record(QStringLiteral("g1"), QStringLiteral("default"), 1);
    r.onServer = true;
    r.deviceName = QStringLiteral("Laptop");
    QVERIFY(s.putRecord(r));
    QVERIFY(s.hasBlob(QStringLiteral("g1"), QStringLiteral("default"), 1));
    QVERIFY(!s.previewPath(QStringLiteral("g1"), QStringLiteral("default"), 1).isEmpty());
    QVERIFY(s.record(QStringLiteral("g1"), QStringLiteral("default"), 1)->onServer);
    r.sha256 = QStringLiteral("0000");
    QVERIFY(s.putRecord(r));
    QVERIFY(!s.hasBlob(QStringLiteral("g1"), QStringLiteral("default"), 1));
    QVERIFY(s.previewPath(QStringLiteral("g1"), QStringLiteral("default"), 1).isEmpty());
  }

  void pendingAndFlags() {
    QTemporaryDir tmp;
    StateStore s(tmp.path());
    StateRecord a = rec(1);
    a.pendingUpload = true;
    QVERIFY(s.put(a, QByteArray("a")));
    QVERIFY(s.put(rec(2), QByteArray("b")));
    StateRecord d = rec(3, QStringLiteral("g2"));
    d.deletePending = true;
    QVERIFY(s.putRecord(d));
    QCOMPARE(s.pending().size(), 2);
    StateRecord upd = *s.record(QStringLiteral("g1"), QStringLiteral("default"), 1);
    upd.pendingUpload = false;
    upd.onServer = true;
    QVERIFY(s.updateRecord(upd));
    QCOMPARE(s.pending().size(), 1);
  }

  void orphansAndMeasure() {
    QTemporaryDir tmp;
    StateStore s(tmp.path());
    QVERIFY(s.put(rec(1, QStringLiteral("keep")), QByteArray("1234")));
    QVERIFY(s.put(rec(1, QStringLiteral("gone")), QByteArray("12345678")));
    QVERIFY(s.put(rec(2, QStringLiteral("gone")), QByteArray("12345678")));
    QVERIFY(s.bytes() > 0);
    const QSet<QString> lib{QStringLiteral("keep")};
    const StateStore::Orphans o = s.orphans(lib);
    QCOMPARE(o.games, 1);
    QCOMPARE(o.states, 2);
    QVERIFY(o.bytes >= 16);
    const StateStore::Orphans r = s.removeOrphans(lib);
    QCOMPARE(r.states, 2);
    QCOMPARE(s.orphans(lib).states, 0);
    QVERIFY(s.record(QStringLiteral("keep"), QStringLiteral("default"), 1).has_value());
  }

  void measureAllSumsEveryServerAndPerson() {
    QTemporaryDir tmp;
    ProfileStore ps(tmp.path());
    StateStore a(StateStore::userStatesDir(ps, QStringLiteral("hub-a"), QStringLiteral("u1")));
    StateStore b(StateStore::userStatesDir(ps, QStringLiteral("hub-b"), QStringLiteral("u2")));
    QVERIFY(a.put(rec(1), QByteArray(1000, 'x')));
    QVERIFY(b.put(rec(1), QByteArray(2000, 'y')));
    QVERIFY(StateStore::measureAll(tmp.path()) >= 3000);
    QCOMPARE(StateStore::measureAll(tmp.path() + QStringLiteral("/none")), 0);
  }

  void recordJsonRoundTrip() {
    StateRecord r = rec(2);
    r.sizeBytes = 5;
    r.sha256 = QStringLiteral("ab");
    r.createdAt = QStringLiteral("2026-01-01T00:00:00Z");
    r.hasPreview = true;
    r.pendingUpload = true;
    const auto back = StateRecord::fromJson(r.toJson());
    QVERIFY(back.has_value());
    QCOMPARE(back->n, 2);
    QCOMPARE(back->coreVersion, QStringLiteral("1.0"));
    QVERIFY(back->pendingUpload && back->hasPreview);
  }
};

QTEST_APPLESS_MAIN(StateStoreTest)
#include "statestore_test.moc"
