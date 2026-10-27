// Discord Rich Presence: a fake Discord IPC server (QLocalServer on a temp name) checks the handshake, SET_ACTIVITY fields,
// throttling and coalescing, the clear on stop and PING -> PONG.
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QUuid>
#include <QtTest>

#include "discordpresence.h"

using namespace uzume;

namespace {
struct Frame {
  quint32 op = 0;
  QJsonObject json;
};

quint32 le(const char* p) {
  const auto* u = reinterpret_cast<const uchar*>(p);
  return quint32(u[0]) | quint32(u[1]) << 8 | quint32(u[2]) << 16 | quint32(u[3]) << 24;
}
}  // namespace

class DiscordPresenceTest : public QObject {
  Q_OBJECT
  std::unique_ptr<QLocalServer> server_;
  QLocalSocket* client_ = nullptr;  // owned by the server
  QByteArray buf_;
  QList<Frame> frames_;
  QString name_;

  void startServer() {
    name_ = QStringLiteral("uzume-discord-test-") + QUuid::createUuid().toString(QUuid::Id128);
    QLocalServer::removeServer(name_);
    server_ = std::make_unique<QLocalServer>();
    QVERIFY(server_->listen(name_));
    connect(server_.get(), &QLocalServer::newConnection, this, [this]() {
      client_ = server_->nextPendingConnection();
      connect(client_, &QLocalSocket::readyRead, this, [this]() {
        buf_.append(client_->readAll());
        while (buf_.size() >= 8 && buf_.size() >= qsizetype(8 + le(buf_.constData() + 4))) {
          const quint32 len = le(buf_.constData() + 4);
          frames_.append({le(buf_.constData()), QJsonDocument::fromJson(buf_.mid(8, len)).object()});
          buf_.remove(0, int(8 + len));
        }
      });
    });
  }
  void serverSend(quint32 op, const QJsonObject& o) {
    client_->write(network_frame(op, QJsonDocument(o).toJson(QJsonDocument::Compact)));
    client_->flush();
  }
  static QByteArray network_frame(quint32 op, const QByteArray& json) { return DiscordPresence::encodeFrame(op, json); }
  QList<Frame> activityFrames() const {
    QList<Frame> out;
    for (const Frame& f : frames_)
      if (f.op == DiscordPresence::Frame && f.json.value(QStringLiteral("cmd")).toString() == QLatin1String("SET_ACTIVITY")) out.append(f);
    return out;
  }
  std::unique_ptr<DiscordPresence> makePresence() {
    auto p = std::make_unique<DiscordPresence>();
    p->setSocketNames({QStringLiteral("uzume-discord-missing"), name_});
    p->setReconnectMs(100);
    return p;
  }
  void connectAndReady(DiscordPresence& p) {
    p.setEnabled(true);
    p.setActivity(DiscordActivity{QStringLiteral("Homebrew Game"), QStringLiteral("Nintendo DS"), 1'700'000'000'000});
    QTRY_VERIFY(!frames_.isEmpty());
    serverSend(DiscordPresence::Frame, {{QStringLiteral("cmd"), QStringLiteral("DISPATCH")}, {QStringLiteral("evt"), QStringLiteral("READY")}});
    QTRY_VERIFY(p.connected());
  }

 private slots:
  void init() {
    frames_.clear();
    buf_.clear();
    client_ = nullptr;
    startServer();
  }

  void defaultNamesCoverTenSlots() {
    const QStringList n = DiscordPresence::defaultSocketNames();
    QVERIFY(n.size() >= 10);
    QVERIFY(n.first().endsWith(QLatin1String("discord-ipc-0")));
  }

  void handshakeAndActivity() {
    auto p = makePresence();
    connectAndReady(*p);
    QCOMPARE(frames_.first().op, quint32(DiscordPresence::Handshake));
    QCOMPARE(frames_.first().json.value(QStringLiteral("v")).toInt(), 1);
    QCOMPARE(frames_.first().json.value(QStringLiteral("client_id")).toString(), QStringLiteral("1564614458993545296"));
    QTRY_COMPARE(activityFrames().size(), 1);
    const QJsonObject m = activityFrames().first().json;
    QVERIFY(!m.value(QStringLiteral("nonce")).toString().isEmpty());
    const QJsonObject args = m.value(QStringLiteral("args")).toObject();
    QCOMPARE(args.value(QStringLiteral("pid")).toInteger(), qint64(QCoreApplication::applicationPid()));
    const QJsonObject act = args.value(QStringLiteral("activity")).toObject();
    QCOMPARE(act.value(QStringLiteral("details")).toString(), QStringLiteral("Homebrew Game"));
    QCOMPARE(act.value(QStringLiteral("state")).toString(), QStringLiteral("Nintendo DS"));
    QCOMPARE(act.value(QStringLiteral("timestamps")).toObject().value(QStringLiteral("start")).toInteger(), qint64(1'700'000'000));
    QCOMPARE(act.keys().size(), 3);  // details, state, timestamps: no assets, no address
    QCOMPARE(p->lastDetails(), QStringLiteral("Homebrew Game"));
    QVERIFY(p->stateJson().value(QStringLiteral("connected")).toBool());
  }

  void throttleCoalescesLatestState() {
    auto p = makePresence();
    p->setThrottleMs(400);
    connectAndReady(*p);
    QTRY_COMPARE(activityFrames().size(), 1);
    p->setActivity(DiscordActivity{QStringLiteral("Homebrew Game"), QStringLiteral("Nintendo DS · Session"), 1'700'000'000'000});
    p->setActivity(DiscordActivity{QStringLiteral("Homebrew Game"), QStringLiteral("Nintendo DS · Run A"), 1'700'000'000'000});
    p->setActivity(DiscordActivity{QStringLiteral("Homebrew Game"), QStringLiteral("Nintendo DS · Run B"), 1'700'000'000'000});
    QTest::qWait(150);
    QCOMPARE(activityFrames().size(), 1);  // held back
    QTRY_COMPARE_WITH_TIMEOUT(activityFrames().size(), 2, 2000);
    const QJsonObject act = activityFrames().last().json.value(QStringLiteral("args")).toObject().value(QStringLiteral("activity")).toObject();
    QCOMPARE(act.value(QStringLiteral("state")).toString(), QStringLiteral("Nintendo DS · Run B"));
    QTest::qWait(150);
    QCOMPARE(activityFrames().size(), 2);
  }

  void clearOnStopAndSwitchOff() {
    auto p = makePresence();
    p->setThrottleMs(300);
    connectAndReady(*p);
    QTRY_COMPARE(activityFrames().size(), 1);
    p->setActivity(std::nullopt);
    QTRY_COMPARE(activityFrames().size(), 2);
    QVERIFY(activityFrames().last().json.value(QStringLiteral("args")).toObject().value(QStringLiteral("activity")).isNull());
    QVERIFY(p->lastDetails().isEmpty());

    p->setActivity(DiscordActivity{QStringLiteral("Game 2"), QStringLiteral("GBA"), 1'700'000'100'000});  // throttled, then sent
    QTRY_COMPARE_WITH_TIMEOUT(activityFrames().size(), 3, 2000);
    p->setEnabled(false);  // the switch: clear and close
    QTRY_COMPARE(activityFrames().size(), 4);
    QVERIFY(activityFrames().last().json.value(QStringLiteral("args")).toObject().value(QStringLiteral("activity")).isNull());
    QVERIFY(!p->connected());
    QVERIFY(!p->enabled());
  }

  void pingIsAnsweredWithPong() {
    auto p = makePresence();
    connectAndReady(*p);
    serverSend(DiscordPresence::Ping, {{QStringLiteral("n"), 7}});
    QTRY_VERIFY(std::any_of(frames_.cbegin(), frames_.cend(), [](const Frame& f) { return f.op == DiscordPresence::Pong; }));
    for (const Frame& f : std::as_const(frames_))
      if (f.op == DiscordPresence::Pong) QCOMPARE(f.json.value(QStringLiteral("n")).toInt(), 7);
  }

  void silentWithoutDiscordThenRetries() {
    server_.reset();  // no Discord
    DiscordPresence p;
    p.setSocketNames({QStringLiteral("uzume-discord-none-1"), QStringLiteral("uzume-discord-none-2")});
    p.setReconnectMs(50);
    p.setEnabled(true);
    p.setActivity(DiscordActivity{QStringLiteral("G"), QStringLiteral("NDS"), 1000});
    QTest::qWait(200);
    QVERIFY(!p.connected());
    // Discord starts later: the retry finds it.
    name_ = QStringLiteral("uzume-discord-late-") + QUuid::createUuid().toString(QUuid::Id128);
    QLocalServer::removeServer(name_);
    server_ = std::make_unique<QLocalServer>();
    QVERIFY(server_->listen(name_));
    connect(server_.get(), &QLocalServer::newConnection, this, [this]() {
      client_ = server_->nextPendingConnection();
      connect(client_, &QLocalSocket::readyRead, this, [this]() { client_->readAll(); client_->write(DiscordPresence::encodeFrame(DiscordPresence::Frame, R"({"cmd":"DISPATCH","evt":"READY"})")); });
    });
    p.setSocketNames({name_});
    QTRY_VERIFY_WITH_TIMEOUT(p.connected(), 3000);
  }

  void disabledDoesNothing() {
    auto p = makePresence();
    p->setActivity(DiscordActivity{QStringLiteral("G"), QStringLiteral("NDS"), 1000});
    QTest::qWait(150);
    QVERIFY(frames_.isEmpty());
    QVERIFY(!server_->hasPendingConnections());
  }
};

QTEST_MAIN(DiscordPresenceTest)
#include "discord_presence_test.moc"
