// Error fingerprints, the event queue and the unclean-exit marker (ADR 0035). Synthetic data only.
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "errorevents.h"
#include "errorfingerprint.h"
#include "errors_vectors.h"

using namespace uzume::errors;

namespace {

// Vectors written from the contract with a reference implementation (the rules in order, Python re); the shared fixture
// protocol/fixtures/error-fingerprint.json is checked as well when it exists.


ErrorEvent ev(const QString& fp, const QDateTime& at, const QString& kind = QStringLiteral("core_failure")) {
  ErrorEvent e;
  e.kind = kind;
  e.component = QStringLiteral("core:azahar");
  e.fingerprint = fp;
  e.title = QStringLiteral("Core failed");
  e.message = QStringLiteral("boom");
  e.appVersion = QStringLiteral("0.24.0");
  e.os = QStringLiteral("Linux");
  e.occurredAt = at;
  return e;
}

void checkVectors(const QJsonArray& arr) {
  QVERIFY(arr.size() >= 8);
  for (const QJsonValue& v : arr) {
    const QJsonObject o = v.toObject();
    const QString msg = o.value("message").toString();
    QCOMPARE(normalizeMessage(msg), o.value("normalized").toString());
    QCOMPARE(fingerprint(o.value("kind").toString(), o.value("component").toString(), msg), o.value("fingerprint").toString());
  }
}

}  // namespace

class ErrorsTest : public QObject {
  Q_OBJECT
 private slots:
  void contractVectors() { checkVectors(QJsonDocument::fromJson(kVectors).array()); }

  void sharedFixtureWhenPresent() {
    QFile f(QStringLiteral(UZUME_FIXTURE_DIR "/error-fingerprint.json"));
    if (!f.open(QIODevice::ReadOnly)) QSKIP("protocol/fixtures/error-fingerprint.json does not exist yet");
    checkVectors(QJsonDocument::fromJson(f.readAll()).array());
  }

  void rulesOneByOne() {
    QCOMPARE(normalizeMessage("See HTTPS://Example.com/a/b?c=1 now"), QString("see <url> now"));
    QCOMPARE(normalizeMessage("open C:\\Users\\Anna\\x.log failed"), QString("open <path> failed"));
    QCOMPARE(normalizeMessage("open /home/anna/x.log failed"), QString("open <path> failed"));
    QCOMPARE(normalizeMessage("open ~/x/y failed"), QString("open <path> failed"));
    QCOMPARE(normalizeMessage("a/b stays, /etc stays"), QString("a/b stays, /etc stays"));  // needs an inner slash
    QCOMPARE(normalizeMessage("from 10.1.2.3:8443 to 192.168.0.1"), QString("from <ip> to <ip>"));
    QCOMPARE(normalizeMessage("peer fe80::1:2:3:4 gone"), QString("peer <ip> gone"));
    QCOMPARE(normalizeMessage("id 9F1C2D3E-aaaa-bbbb and 0xDEADBEEF and abc12"), QString("id <hex>-aaaa-bbbb and <hex> and abc<n>"));
    QCOMPARE(normalizeMessage("line 42 col 7"), QString("line <n> col <n>"));
    QCOMPARE(normalizeMessage("  a \t b\n\n c  "), QString("a b c"));
  }

  void cutsAt300BytesOnACharacterBoundary() {
    const QString twoByte(200, QChar(0xE9));  // 400 bytes
    const QByteArray cut = normalizeMessage(twoByte).toUtf8();
    QCOMPARE(cut.size(), 300);
    QVERIFY(QString::fromUtf8(cut).size() == 150);
    const QString threeByte = QString("x") + QString(200, QChar(0x65E5));  // 1 + 600 bytes: byte 300 is inside a character
    const QByteArray cut3 = normalizeMessage(threeByte).toUtf8();
    QCOMPARE(cut3.size(), 298);
    QCOMPARE(QString::fromUtf8(cut3), threeByte.left(1 + 99));
  }

  void fingerprintShapeAndInputs() {
    const QString fp = fingerprint("core_failure", "core:azahar", "Failed 12");
    QVERIFY(validFingerprint(fp));
    QCOMPARE(fp, fingerprint("core_failure", "core:azahar", "FAILED 99"));       // numbers and case do not matter
    QVERIFY(fp != fingerprint("plugin_error", "core:azahar", "Failed 12"));      // kind does
    QVERIFY(fp != fingerprint("core_failure", "core:mgba", "Failed 12"));        // component does
    QVERIFY(!validFingerprint("uz-ABCDEF123456"));
    QVERIFY(!validFingerprint("uz-abcdef12345"));
    QVERIFY(!validFingerprint("uz-abcdef1234567"));
  }

  void kindsAndComponents() {
    for (const char* k : {"app_crash", "core_failure", "plugin_error", "sync_failure", "update_failure", "server_error"}) QVERIFY(validKind(k));
    QVERIFY(!validKind("crash"));
    for (const char* c : {"app", "server", "core:azahar", "core:melonds-ds_2", "plugin:uzume.soullink", "plugin:A_b-1.2"}) QVERIFY2(validComponent(c), c);
    for (const char* c : {"", "App", "core:", "core:Azahar", "core:-x", "plugin:", "plugin:a b", "plugin:a\n", "core:azahar\n", "other"}) QVERIFY2(!validComponent(c), c);
    QVERIFY(!validComponent("plugin:" + QString(97, QLatin1Char('a'))));
    QVERIFY(validComponent("plugin:" + QString(80, QLatin1Char('a'))));
  }

  void makeEventRedactsAndLimits() {
    const QDateTime at = QDateTime(QDate(2026, 10, 28), QTime(12, 0), Qt::UTC);
    const ErrorEvent e = makeEvent("sync_failure", "app", "  Save sync failed\nsecond line  ",
                                   "upload failed, Authorization: Bearer abcdefghijklmnop and password=hunter2", {"0.24.0-beta.3", "Windows 11"}, at);
    QCOMPARE(e.title, QString("Save sync failed"));
    QVERIFY(!e.message.contains("hunter2"));
    QVERIFY(!e.message.contains("abcdefghijklmnop"));
    QVERIFY(e.message.contains("[redacted]"));
    QCOMPARE(e.occurredAt, at);
    QVERIFY(validFingerprint(e.fingerprint));
    QCOMPARE(e.fingerprint, fingerprint("sync_failure", "app", e.message));
    const ErrorEvent big = makeEvent("app_crash", "app", QString(500, QLatin1Char('t')), QString(5000, QLatin1Char('m')), {QString(80, 'v'), QString(200, 'o')}, at);
    QCOMPARE(big.title.size(), kMaxTitle);
    QCOMPARE(big.message.size(), kMaxMessage);
    QCOMPARE(big.appVersion.size(), 40);
    QCOMPARE(big.os.size(), 80);
    QVERIFY(makeEvent("nope", "app", "t", "m", {}, at).fingerprint.isEmpty());  // invalid kind: nothing to queue
    QVERIFY(makeEvent("app_crash", "app", "", "", {}, at).title == "app_crash");
  }

  void jsonRoundTripAndWireShape() {
    const QDateTime at = QDateTime(QDate(2026, 10, 28), QTime(12, 0, 5), Qt::UTC);
    const ErrorEvent e = ev("uz-0123456789ab", at);
    const QJsonObject o = e.toJson();
    QCOMPARE(o.value("occurred_at").toString(), QString("2026-10-28T12:00:05Z"));
    QCOMPARE(o.value("app_version").toString(), QString("0.24.0"));
    QCOMPARE(o.keys().size(), 8);
    const auto back = ErrorEvent::fromJson(o);
    QVERIFY(back.has_value());
    QCOMPARE(back->fingerprint, e.fingerprint);
    QCOMPARE(back->occurredAt, at);
    QJsonObject bad = o;
    bad.insert("fingerprint", "nope");
    QVERIFY(!ErrorEvent::fromJson(bad).has_value());
  }

  void queueMergesSameFingerprintWithinTenMinutes() {
    QTemporaryDir dir;
    ErrorQueue q(dir.path());
    const QDateTime t0 = QDateTime(QDate(2026, 10, 28), QTime(12, 0), Qt::UTC);
    QVERIFY(q.add(ev("uz-000000000001", t0)));
    QVERIFY(!q.add(ev("uz-000000000001", t0.addSecs(599))));   // merged
    QVERIFY(q.add(ev("uz-000000000002", t0.addSecs(30))));     // another problem
    QVERIFY(q.add(ev("uz-000000000001", t0.addSecs(600))));    // 10 minutes later: queued again
    QCOMPARE(q.size(), 3);
    ErrorEvent invalid = ev("uz-000000000009", t0);
    invalid.component = "Core:Bad";
    QVERIFY(!q.add(invalid));
    QCOMPARE(q.size(), 3);
  }

  void queueKeepsAHundredAndDropsTheOldest() {
    QTemporaryDir dir;
    ErrorQueue q(dir.path());
    const QDateTime t0 = QDateTime(QDate(2026, 10, 28), QTime(12, 0), Qt::UTC);
    for (int i = 0; i < 105; ++i) QVERIFY(q.add(ev(QString("uz-%1").arg(i, 12, 16, QLatin1Char('0')), t0.addSecs(i))));
    QCOMPARE(q.size(), kMaxQueued);
    QCOMPARE(q.events().first().fingerprint, QString("uz-000000000005"));
    QCOMPARE(q.events().last().fingerprint, QString("uz-000000000068").replace("68", "68"));  // 104 = 0x68
    QCOMPARE(q.front().size(), kBatchSize);
    QCOMPARE(q.front(5).size(), 5);
  }

  void queueIsPersistedAndRemoveFrontShrinksIt() {
    QTemporaryDir dir;
    const QDateTime t0 = QDateTime(QDate(2026, 10, 28), QTime(12, 0), Qt::UTC);
    {
      ErrorQueue q(dir.path());
      for (int i = 0; i < 5; ++i) q.add(ev(QString("uz-%1").arg(i, 12, 16, QLatin1Char('0')), t0.addSecs(i)));
    }
    QVERIFY(QFile::exists(ErrorQueue::fileName(dir.path())));
    ErrorQueue q(dir.path());
    QCOMPARE(q.size(), 5);
    QCOMPARE(q.events().at(2).fingerprint, QString("uz-000000000002"));
    q.removeFront(3);
    ErrorQueue again(dir.path());
    QCOMPARE(again.size(), 2);
    QCOMPARE(again.events().first().fingerprint, QString("uz-000000000003"));
    again.removeFront(10);
    QVERIFY(again.isEmpty());
    QVERIFY(!QFile::exists(ErrorQueue::fileName(dir.path())));  // an empty queue leaves no file
  }

  void damagedQueueFileIsIgnored() {
    QTemporaryDir dir;
    QFile f(ErrorQueue::fileName(dir.path()));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{not json");
    f.close();
    ErrorQueue q(dir.path());
    QVERIFY(q.isEmpty());
    QVERIFY(q.add(ev("uz-000000000001", QDateTime::currentDateTimeUtc())));
  }

  void lastErrorLineOfALog() {
    QTemporaryDir dir;
    const QString log = dir.filePath("player.1.log");
    QFile f(log);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("2026-10-28T10:00:00.000 [I] hub: connected\n"
            "2026-10-28T10:00:01.000 [E] core: first problem\n"
            "2026-10-28T10:00:02.000 [W] plugins: a warning\n"
            "2026-10-28T10:00:03.000 [F] emu: load failed token=abc12345secret\n"
            "2026-10-28T10:00:04.000 [I] hub: bye\n");
    f.close();
    const QString line = lastErrorLine(log);
    QVERIFY2(line.startsWith("emu: load failed"), qPrintable(line));
    QVERIFY(!line.contains("abc12345secret"));
    QVERIFY(lastErrorLine(dir.filePath("missing.log")).isEmpty());
    QFile g(dir.filePath("quiet.log"));
    QVERIFY(g.open(QIODevice::WriteOnly));
    g.write("2026-10-28T10:00:00.000 [I] hub: connected\n2026-10-28T10:00:01.000 [W] x: y\n");
    g.close();
    QVERIFY(lastErrorLine(dir.filePath("quiet.log")).isEmpty());
  }

  void runMarkerLifecycle() {
    QTemporaryDir dir;
    QVERIFY(!RunMarker::exists(dir.path()));
    {
      RunMarker m(dir.path());
      QVERIFY(RunMarker::exists(dir.path()));
      QVERIFY(QFile::exists(QDir(dir.path()).filePath("running.marker")));
    }
    QVERIFY(!RunMarker::exists(dir.path()));  // a clean exit removes it
    {
      RunMarker m(dir.path());
      m.release();
      QVERIFY(!RunMarker::exists(dir.path()));
    }
    // A marker left behind (crash) is found by the next start.
    QFile f(RunMarker::fileName(dir.path()));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();
    QVERIFY(RunMarker::exists(dir.path()));
    RunMarker next(dir.path());
    QVERIFY(RunMarker::exists(dir.path()));
  }
};

QTEST_APPLESS_MAIN(ErrorsTest)
#include "errors_test.moc"
