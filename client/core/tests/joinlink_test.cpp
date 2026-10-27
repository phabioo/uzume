#include <QtTest>

#include "joinlink.h"

using namespace uzume;
using Kind = JoinInput::Kind;

namespace {
const QString kFp = QStringLiteral("2F:E5:AD:C0:A7:7D:0E:CE:5E:05:AC:C5:CA:A2:C7:6C:E2:8E:63:A9:CB:22:A5:D8:3D:F2:09:85:79:E8:B2:D2");
QString plainFp() {
  QString f = kFp;
  return f.remove(QLatin1Char(':'));
}
}  // namespace

class JoinLinkTest : public QObject {
  Q_OBJECT
 private slots:
  void inviteCodeInAnyFormat_data() {
    QTest::addColumn<QString>("text");
    QTest::newRow("canonical") << QStringLiteral("UZ-8XK2-M4QD");
    QTest::newRow("lower case") << QStringLiteral("uz-8xk2-m4qd");
    QTest::newRow("no hyphens") << QStringLiteral("UZ8XK2M4QD");
    QTest::newRow("spaces") << QStringLiteral("uz 8xk2 m4qd");
    QTest::newRow("mixed and padded") << QStringLiteral("  Uz-8XK2 m4Qd \n");
  }
  void inviteCodeInAnyFormat() {
    QFETCH(QString, text);
    const JoinInput in = classifyJoinInput(text);
    QCOMPARE(in.kind, Kind::InviteCode);
    QCOMPARE(in.inviteCode, QStringLiteral("UZ-8XK2-M4QD"));
    QVERIFY(in.ok());
    QVERIFY(in.error.isEmpty());
  }

  void mistypedInviteCodeIsNotAnAddress() {
    const JoinInput in = classifyJoinInput(QStringLiteral("UZ-8XK2-M4Q"));  // one character short
    QCOMPARE(in.kind, Kind::Invalid);
    QCOMPARE(in.error, QStringLiteral("invalid_invite_code"));
    QCOMPARE(classifyJoinInput(QStringLiteral("UZ-8XK2-M4QU")).kind, Kind::Invalid);  // U is not Crockford
  }

  void addressGetsTheDefaultPort() {
    JoinInput in = classifyJoinInput(QStringLiteral("server.example.com"));
    QCOMPARE(in.kind, Kind::Address);
    QCOMPARE(in.address, QStringLiteral("server.example.com:8443"));
    in = classifyJoinInput(QStringLiteral(" 192.0.2.20:9000 "));
    QCOMPARE(in.address, QStringLiteral("192.0.2.20:9000"));
    in = classifyJoinInput(QStringLiteral("[::1]"));
    QCOMPARE(in.address, QStringLiteral("[::1]:8443"));
    in = classifyJoinInput(QStringLiteral("[::1]:9443"));
    QCOMPARE(in.address, QStringLiteral("[::1]:9443"));
    in = classifyJoinInput(QStringLiteral("https://pi.local"));
    QCOMPARE(in.kind, Kind::Address);
    QCOMPARE(in.address, QStringLiteral("https://pi.local:8443"));
    in = classifyJoinInput(QStringLiteral("uz-pi"));  // a host name that only starts like an invite code
    QCOMPARE(in.kind, Kind::Address);
    QCOMPARE(kDefaultHubPort, 8443);
  }

  void invalidInput() {
    QCOMPARE(classifyJoinInput(QString()).kind, Kind::Empty);
    QCOMPARE(classifyJoinInput(QString()).error, QStringLiteral("empty"));
    QCOMPARE(classifyJoinInput(QStringLiteral("   ")).kind, Kind::Empty);
    for (const QString& bad : {QStringLiteral("two words"), QStringLiteral("host:notaport"), QStringLiteral("host:0"),
                               QStringLiteral("host:70000"), QStringLiteral("ftp://host"), QStringLiteral("a/b"),
                               QStringLiteral("[::1")}) {
      const JoinInput in = classifyJoinInput(bad);
      QCOMPARE(in.kind, Kind::Invalid);
      QCOMPARE(in.error, QStringLiteral("invalid_address"));
      QVERIFY(!in.ok());
    }
  }

  void linkWithFingerprint() {
    const QString text = QStringLiteral("uzume://join/192.0.2.20:8443/UZ-8XK2-M4QD#") + kFp;
    JoinInput in = classifyJoinInput(text);
    QCOMPARE(in.kind, Kind::Link);
    QCOMPARE(in.address, QStringLiteral("192.0.2.20:8443"));
    QCOMPARE(in.inviteCode, QStringLiteral("UZ-8XK2-M4QD"));
    QCOMPARE(in.fingerprint, kFp);
    // Fingerprint without colons, lower case, scheme in capitals, invite without hyphens, no port, trailing slash.
    in = classifyJoinInput(QStringLiteral("UZUME://join/server.example.com/uz8xk2m4qd/#") + plainFp().toLower());
    QCOMPARE(in.kind, Kind::Link);
    QCOMPARE(in.address, QStringLiteral("server.example.com:8443"));
    QCOMPARE(in.inviteCode, QStringLiteral("UZ-8XK2-M4QD"));
    QCOMPARE(in.fingerprint, kFp);
    in = classifyJoinInput(QStringLiteral("uzume://join/[::1]:9443/UZ-8XK2-M4QD#") + plainFp());
    QCOMPARE(in.kind, Kind::Link);
    QCOMPARE(in.address, QStringLiteral("[::1]:9443"));
  }

  void brokenLinks_data() {
    QTest::addColumn<QString>("text");
    QTest::newRow("no fingerprint") << QStringLiteral("uzume://join/192.0.2.20:8443/UZ-8XK2-M4QD");
    QTest::newRow("short fingerprint") << (QStringLiteral("uzume://join/192.0.2.20/UZ-8XK2-M4QD#") + plainFp().left(62));
    QTest::newRow("fingerprint not hex") << (QStringLiteral("uzume://join/192.0.2.20/UZ-8XK2-M4QD#") + QString(64, QLatin1Char('g')));
    QTest::newRow("no invite") << (QStringLiteral("uzume://join/192.0.2.20/#") + plainFp());
    QTest::newRow("bad invite") << (QStringLiteral("uzume://join/192.0.2.20/UZ-8XK2#") + plainFp());
    QTest::newRow("no host") << (QStringLiteral("uzume://join//UZ-8XK2-M4QD#") + plainFp());
    QTest::newRow("bad host") << (QStringLiteral("uzume://join/bad host/UZ-8XK2-M4QD#") + plainFp());
    QTest::newRow("bad port") << (QStringLiteral("uzume://join/host:x/UZ-8XK2-M4QD#") + plainFp());
    QTest::newRow("extra segment") << (QStringLiteral("uzume://join/host/UZ-8XK2-M4QD/more#") + plainFp());
    QTest::newRow("other action") << (QStringLiteral("uzume://open/host/UZ-8XK2-M4QD#") + plainFp());
    QTest::newRow("only scheme") << QStringLiteral("uzume://");
    QTest::newRow("truncated") << QStringLiteral("uzume:");
  }
  void brokenLinks() {
    QFETCH(QString, text);
    const JoinInput in = classifyJoinInput(text);
    QCOMPARE(in.kind, Kind::BrokenLink);
    QCOMPARE(in.error, QStringLiteral("broken_link"));
    QVERIFY(!in.ok());
  }

  void runLinks() {
    const QString id = QStringLiteral("11111111-2222-3333-4444-555555555555");
    const QString link = makeRunLink(QStringLiteral("https://192.0.2.20:8443/"), id.toUpper());
    QCOMPARE(link, QStringLiteral("uzume://run/192.0.2.20:8443/") + id + QStringLiteral("/recap"));
    QCOMPARE(makeRunLink(QStringLiteral("pi.local"), id), QStringLiteral("uzume://run/pi.local:8443/") + id + QStringLiteral("/recap"));
    const RunLink parsed = parseRunLink(link);
    QVERIFY(parsed.ok);
    QCOMPARE(parsed.address, QStringLiteral("192.0.2.20:8443"));
    QCOMPARE(parsed.runId, id);
    QVERIFY(parseRunLink(QStringLiteral("UZUME://RUN/pi.local/") + id + QStringLiteral("/Recap/")).ok);
    QCOMPARE(parseRunLink(QStringLiteral("uzume://run/pi.local/") + id + QStringLiteral("/recap")).address, QStringLiteral("pi.local:8443"));
    QVERIFY(makeRunLink(QStringLiteral("bad host"), id).isEmpty());
    QVERIFY(makeRunLink(QStringLiteral("pi.local"), QStringLiteral("not-a-run")).isEmpty());
    QVERIFY(!parseRunLink(QStringLiteral("uzume://run/pi.local/") + id).ok);                       // no view
    QVERIFY(!parseRunLink(QStringLiteral("uzume://run/pi.local/") + id + QStringLiteral("/chat")).ok);
    QVERIFY(!parseRunLink(QStringLiteral("uzume://run/pi.local/xyz/recap")).ok);
    QVERIFY(!parseRunLink(QStringLiteral("uzume://join/pi.local/UZ-8XK2-M4QD")).ok);
    QVERIFY(!parseRunLink(QStringLiteral("pi.local")).ok);
  }

  void makeLinkRoundTrips() {
    const QString link = makeJoinLink(QStringLiteral("https://192.0.2.20:8443/"), QStringLiteral("uz 8xk2 m4qd"), kFp);
    QCOMPARE(link, QStringLiteral("uzume://join/192.0.2.20:8443/UZ-8XK2-M4QD#") + plainFp());
    const JoinInput in = classifyJoinInput(link);
    QCOMPARE(in.kind, Kind::Link);
    QCOMPARE(in.address, QStringLiteral("192.0.2.20:8443"));
    QCOMPARE(in.inviteCode, QStringLiteral("UZ-8XK2-M4QD"));
    QCOMPARE(in.fingerprint, kFp);
    QCOMPARE(makeJoinLink(QStringLiteral("pi.local"), QStringLiteral("UZ-8XK2-M4QD"), kFp),
             QStringLiteral("uzume://join/pi.local:8443/UZ-8XK2-M4QD#") + plainFp());
    QVERIFY(makeJoinLink(QStringLiteral("bad host"), QStringLiteral("UZ-8XK2-M4QD"), kFp).isEmpty());
    QVERIFY(makeJoinLink(QStringLiteral("pi.local"), QStringLiteral("nope"), kFp).isEmpty());
    QVERIFY(makeJoinLink(QStringLiteral("pi.local"), QStringLiteral("UZ-8XK2-M4QD"), QStringLiteral("AA")).isEmpty());
  }
};

QTEST_GUILESS_MAIN(JoinLinkTest)
#include "joinlink_test.moc"
