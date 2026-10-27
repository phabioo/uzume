// RunClient against the fake Hub: feature gate runs_v1, REST calls, run_updated / run_state / run_event, catch-up with
// hello `runs` and paging, dedup by client_id, idempotent event retry, state coalescing and 409 state_conflict, board
// rate limit, local refusal of writes in paused runs. Dummy data only.
#include <QDirIterator>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "credentialstore.h"
#include "fakehub.h"
#include "hubconnection.h"
#include "hubsocket.h"
#include "profilestore.h"
#include "runclient.h"

using namespace uzume;
using State = HubConnection::State;

namespace {
const QString kPlugin = QStringLiteral("net.test.plugin");
const QString kPlugin2 = QStringLiteral("net.test.second");
const QString kOther = QStringLiteral("u_other_1");
}  // namespace

class RunClientTest : public QObject {
  Q_OBJECT
  std::unique_ptr<QTemporaryDir> dir_;
  std::unique_ptr<ProfileStore> profiles_;
  std::unique_ptr<MemoryCredentialStore> creds_;
  std::unique_ptr<FakeHub> hub_;
  std::unique_ptr<HubConnection> conn_;
  std::unique_ptr<HubSocket> socket_;
  std::unique_ptr<RunClient> client_;

  void connectHub(const QStringList& features = {QStringLiteral("saves_v1"), QStringLiteral("runs_v1")}) {
    dir_ = std::make_unique<QTemporaryDir>();
    profiles_ = std::make_unique<ProfileStore>(dir_->filePath(QStringLiteral("data")));
    creds_ = std::make_unique<MemoryCredentialStore>();
    hub_ = std::make_unique<FakeHub>(QStringLiteral("a"));
    hub_->features = features;
    QVERIFY(hub_->start());
    conn_ = std::make_unique<HubConnection>(profiles_.get(), creds_.get());
    conn_->setPollIntervalMs(50);
    conn_->connectToAddress(hub_->address());
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsTrustConfirmation, 8000);
    conn_->confirmTrust();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::NeedsPairing, 8000);
    hub_->decision = FakeHub::Decision::Approve;
    conn_->requestPairing();
    QTRY_VERIFY_WITH_TIMEOUT(conn_->state() == State::Connected, 8000);
  }

  // Client + socket; the client exists before the socket connects so that hello carries the cache.
  void startClient() {
    socket_ = std::make_unique<HubSocket>(conn_.get());
    client_ = std::make_unique<RunClient>(conn_.get(), socket_.get(), dir_->filePath(QStringLiteral("runs-data")));
    client_->setStateIntervalMs(60);
    client_->setBoardIntervalMs(150);
    client_->setRetryBaseMs(40);
    client_->setCatchUpDelayMs(60);
    QSignalSpy acked(socket_.get(), &HubSocket::helloAcked);
    socket_->start();
    QTRY_VERIFY_WITH_TIMEOUT(socket_->isOpen() && !acked.isEmpty(), 8000);
  }

  QJsonArray pluginRefs() const {
    return QJsonArray{QJsonObject{{"plugin_id", kPlugin}, {"version", "1.0.0"}, {"sha256", QString(64, QLatin1Char('a'))}},
                      QJsonObject{{"plugin_id", kPlugin2}, {"version", "1.0.0"}, {"sha256", QString(64, QLatin1Char('b'))}}};
  }

  // A run on the Hub with me joined, `kOther` invited, and `events` extra events from me.
  QString seedRun(int events = 0, const QString& status = QStringLiteral("active")) {
    QJsonObject r = hub_->makeRun(QStringLiteral("Test run"), {hub_->runsMe, kOther}, hub_->runsMe, pluginRefs());
    const QString id = r.value("run_id").toString();
    r.insert("status", status);
    hub_->runs.insert(id, r);
    for (int i = 0; i < events; ++i) hub_->addRunEvent(id, QStringLiteral("test.event"), QStringLiteral("e%1").arg(i), hub_->runsMe, kPlugin, {}, false);
    return id;
  }

  QString waitForRun(const QString& id) {
    [&] { QTRY_VERIFY_WITH_TIMEOUT(client_->run(id).has_value(), 8000); }();
    return id;
  }

 private slots:
  void cleanup() {
    client_.reset();
    socket_.reset();
    conn_.reset();
    hub_.reset();
    creds_.reset();
    profiles_.reset();
    dir_.reset();
  }

  // 0.20: the run's core rules are parsed leniently; anything that is not "logged" / true means off.
  void runRulesParse() {
    const RunRules none = parseRunRules({});
    QVERIFY(!none.states && !none.rewind && !none.cheatsAllowed() && !none.memoryWriteAllowed());
    const RunRules r = parseRunRules({{"states", true}, {"rewind", false}, {"cheats", "logged"}, {"memory_write", "weird"}});
    QVERIFY(r.states && !r.rewind && r.cheatsAllowed() && !r.memoryWriteAllowed());
    QCOMPARE(parseRunRules(runRulesToJson(r)), r);
  }

  // run_rules_v1: rules go with the create call only when the server knows them; PUT replaces the whole object; 403 is reported.
  void createWithRulesAndSetRules() {
    connectHub({QStringLiteral("saves_v1"), QStringLiteral("runs_v1"), QStringLiteral("run_rules_v1")});
    startClient();
    QVERIFY(client_->supportsRules());
    QTRY_VERIFY_WITH_TIMEOUT(client_->loaded(), 8000);
    RunRules rules;
    rules.states = true;
    rules.cheats = QStringLiteral("logged");
    std::optional<RunResult> created;
    client_->createRun(QStringLiteral("Rules run"), {}, QStringLiteral("g-1"), {}, QJsonObject(), rules, [&](const RunResult& r) { created = r; });
    QTRY_VERIFY_WITH_TIMEOUT(created.has_value(), 8000);
    QVERIFY(created->ok && created->run);
    QCOMPARE(hub_->lastRunBody.value("rules").toObject().value("cheats").toString(), QStringLiteral("logged"));
    QCOMPARE(created->run->rules, rules);

    RunRules next = rules;
    next.memoryWrite = QStringLiteral("logged");
    next.rewind = true;
    std::optional<RunResult> put;
    client_->setRules(created->run->runId, next, [&](const RunResult& r) { put = r; });
    QTRY_VERIFY_WITH_TIMEOUT(put.has_value(), 8000);
    QVERIFY(put->ok);
    QCOMPARE(hub_->runRulesPuts.size(), 1);
    QCOMPARE(hub_->runRulesPuts.at(0).value("memory_write").toString(), QStringLiteral("logged"));
    QCOMPARE(hub_->runRulesPuts.at(0).value("states").toBool(), true);  // the whole object, not a patch
    QTRY_COMPARE_WITH_TIMEOUT(client_->run(created->run->runId)->rules, next, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(!client_->events(created->run->runId, 0, 50).isEmpty(), 8000);

    hub_->runRulesForbidden = true;
    std::optional<RunResult> denied;
    client_->setRules(created->run->runId, rules, [&](const RunResult& r) { denied = r; });
    QTRY_VERIFY_WITH_TIMEOUT(denied.has_value(), 8000);
    QVERIFY(!denied->ok);
    QCOMPARE(denied->errorCode, QStringLiteral("forbidden"));
  }

  void rulesNotSentWithoutFeature() {
    connectHub();
    startClient();
    QVERIFY(!client_->supportsRules());
    QTRY_VERIFY_WITH_TIMEOUT(client_->loaded(), 8000);
    RunRules rules;
    rules.states = true;
    std::optional<RunResult> created;
    client_->createRun(QStringLiteral("Old hub"), {}, QStringLiteral("g-1"), {}, QJsonObject(), rules, [&](const RunResult& r) { created = r; });
    QTRY_VERIFY_WITH_TIMEOUT(created.has_value(), 8000);
    QVERIFY(!hub_->lastRunBody.contains(QStringLiteral("rules")));
  }

  void gatedByFeature() {
    connectHub({QStringLiteral("saves_v1")});
    startClient();
    QVERIFY(!client_->supported());
    client_->refresh();
    QTest::qWait(150);
    QCOMPARE(hub_->count(QStringLiteral("/api/v1/runs")), 0);
    QVERIFY(client_->helloRuns().isEmpty());
  }

  // runs_v2: a run without plugins; every Run JSON carries save_slot (runSaveSlot when the Hub does not send it).
  void pluginlessRunAndSaveSlot() {
    connectHub();
    startClient();
    QVERIFY(client_->supported());
    QVERIFY(!client_->supportsPluginless());  // runs_v1 only
    QTRY_VERIFY_WITH_TIMEOUT(client_->loaded(), 8000);

    std::optional<RunResult> created;
    client_->createRun(QStringLiteral("Plain run"), {}, QStringLiteral("00000000-0000-4000-8000-0000000000a1"), {}, QJsonObject(),
                       [&](const RunResult& r) { created = r; });
    QTRY_VERIFY_WITH_TIMEOUT(created.has_value(), 8000);
    QVERIFY(created->ok && created->run);
    QVERIFY(hub_->lastRunBody.contains(QStringLiteral("plugins")));
    QVERIFY(hub_->lastRunBody.value("plugins").toArray().isEmpty());  // an empty plugin list, not a missing one
    QVERIFY(created->run->plugins.isEmpty());
    QCOMPARE(created->run->saveSlot, runSaveSlot(created->run->runId));
    QVERIFY(created->run->saveSlot.startsWith(QStringLiteral("run-")));

    // A save_slot sent by the Hub wins; a missing one falls back to run-<8 hex>.
    QJsonObject o = hub_->runs.value(created->run->runId);
    o.insert("save_slot", "run-feedbeef");
    QCOMPARE(parseRun(o)->saveSlot, QStringLiteral("run-feedbeef"));
    o.remove("save_slot");
    QCOMPARE(parseRun(o)->saveSlot, runSaveSlot(created->run->runId));
    QCOMPARE(runToJson(*parseRun(o)).value("save_slot").toString(), runSaveSlot(created->run->runId));
  }

  void pluginlessFeature() {
    connectHub({QStringLiteral("saves_v1"), QStringLiteral("runs_v1"), QStringLiteral("runs_v2")});
    startClient();
    QVERIFY(client_->supported());
    QVERIFY(client_->supportsPluginless());
  }

  void listAndRunUpdated() {
    connectHub();
    const QString id = seedRun(2, QStringLiteral("starting"));
    startClient();
    waitForRun(id);
    QTRY_VERIFY_WITH_TIMEOUT(client_->loaded(), 8000);
    QCOMPARE(client_->runs().size(), 1);
    const RunInfo run = *client_->run(id);
    QCOMPARE(run.name, QStringLiteral("Test run"));
    QCOMPARE(run.participants.size(), 2);
    QCOMPARE(run.participant(hub_->runsMe)->status, QStringLiteral("joined"));
    QCOMPARE(client_->pendingInviteCount(), 0);

    QSignalSpy status(client_.get(), &RunClient::statusChanged);
    QSignalSpy parts(client_.get(), &RunClient::participantsChanged);
    QJsonObject r = hub_->runs.value(id);
    r.insert("status", "active");
    hub_->runs.insert(id, r);
    hub_->pushRun(id);
    QTRY_COMPARE_WITH_TIMEOUT(status.size(), 1, 8000);
    QCOMPARE(status.first().at(1).toString(), QStringLiteral("active"));
    QCOMPARE(parts.size(), 0);
  }

  void invitesAreCounted() {
    connectHub();
    const QString other = hub_->makeRun(QStringLiteral("Their run"), {kOther, hub_->runsMe}, kOther, pluginRefs()).value("run_id").toString();
    startClient();
    waitForRun(other);
    QCOMPARE(client_->pendingInviteCount(), 1);

    RunResult res;
    bool done = false;
    client_->joinRun(other, QStringLiteral("00000000-0000-4000-8000-0000000000aa"), [&](const RunResult& r) {
      res = r;
      done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QVERIFY(res.ok);
    QCOMPARE(hub_->lastRunBody.value("game_id").toString(), QStringLiteral("00000000-0000-4000-8000-0000000000aa"));
    QCOMPARE(client_->pendingInviteCount(), 0);
    QCOMPARE(client_->run(other)->participant(hub_->runsMe)->status, QStringLiteral("joined"));
  }

  void restActions() {
    connectHub();
    startClient();
    RunResult created;
    bool done = false;
    client_->createRun(QStringLiteral("Fresh"), {{kPlugin, QStringLiteral("1.0.0"), {}}}, QStringLiteral("g-1"), {kOther}, QJsonObject{{"rule", 1}},
                       [&](const RunResult& r) {
                         created = r;
                         done = true;
                       });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QVERIFY(created.ok);
    QVERIFY(created.run.has_value());
    QCOMPARE(hub_->lastRunBody.value("name").toString(), QStringLiteral("Fresh"));
    QCOMPARE(hub_->lastRunBody.value("invite_user_ids").toArray().size(), 1);
    QCOMPARE(hub_->lastRunBody.value("plugins").toArray().at(0).toObject().value("version").toString(), QStringLiteral("1.0.0"));
    const QString id = created.run->runId;
    QCOMPARE(created.run->status, QStringLiteral("starting"));

    const auto status = [&](const QString& s) {
      RunResult r;
      bool d = false;
      client_->setStatus(id, s, [&](const RunResult& x) {
        r = x;
        d = true;
      });
      [&] { QTRY_VERIFY_WITH_TIMEOUT(d, 8000); }();
      return r;
    };
    QVERIFY(status(QStringLiteral("active")).ok);
    QCOMPARE(client_->run(id)->status, QStringLiteral("active"));
    const RunResult bad = status(QStringLiteral("starting"));  // not a status the API accepts for a transition
    QVERIFY(!bad.ok);
    QCOMPARE(bad.status, 409);
    QVERIFY(status(QStringLiteral("paused")).ok);
    QVERIFY(status(QStringLiteral("active")).ok);

    done = false;
    client_->inviteUsers(id, {QStringLiteral("u_third")}, [&](const RunResult&) { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QCOMPARE(client_->run(id)->participants.size(), 3);

    done = false;
    QList<HubUser> users;
    client_->loadUsers([&](bool ok, const QList<HubUser>& u, const QString&) {
      QVERIFY(ok);
      users = u;
      done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 8000);
    QCOMPARE(users.size(), 1);
  }

  void eventsDedupByClientId() {
    connectHub();
    const QString id = seedRun(0);
    startClient();
    waitForRun(id);
    QTRY_COMPARE_WITH_TIMEOUT(client_->lastSeq(id), 1, 8000);  // the Hub event run.created via paging
    QSignalSpy added(client_.get(), &RunClient::eventAdded);
    const QString cid = FakeHub::makeUuid();
    hub_->addRunEvent(id, QStringLiteral("test.event"), QStringLiteral("once"), hub_->runsMe, kPlugin, cid, true);
    QTRY_COMPARE_WITH_TIMEOUT(added.size(), 1, 8000);
    // The same event again (the author's own device sees its echo) and the same client_id under another seq.
    hub_->sendWs(QStringLiteral("run_event"), {{"run_id", id}, {"event", hub_->runEvents.value(id).last()}});
    QJsonObject dup = hub_->runEvents.value(id).last();
    dup.insert("seq", 99);
    hub_->sendWs(QStringLiteral("run_event"), {{"run_id", id}, {"event", dup}});
    QTest::qWait(250);
    QCOMPARE(added.size(), 1);
    QCOMPARE(client_->events(id, 0, 100).size(), 2);
  }

  void helloCarriesLastSeqAndCatchesUp() {
    connectHub();
    const QString id = seedRun(2);  // seq 1 (run.created) + 2 events
    startClient();
    waitForRun(id);
    QTRY_COMPARE_WITH_TIMEOUT(client_->lastSeq(id), 3, 8000);
    // Disconnect the client; the Hub gets more events meanwhile.
    const QString dataDir = dir_->filePath(QStringLiteral("runs-data"));
    client_.reset();
    socket_.reset();
    for (int i = 0; i < 3; ++i) hub_->addRunEvent(id, QStringLiteral("test.event"), QStringLiteral("late%1").arg(i), hub_->runsMe, kPlugin, {}, false);
    hub_->wsReceived.clear();
    const int getsBefore = hub_->count(QStringLiteral("/api/v1/runs/%1/events").arg(id), "GET");

    startClient();
    QTRY_COMPARE_WITH_TIMEOUT(client_->lastSeq(id), 6, 8000);
    // hello carried the persisted last_seq of the run
    QJsonObject hello;
    for (const QJsonObject& m : std::as_const(hub_->wsReceived))
      if (m.value("type").toString() == QLatin1String("hello")) hello = m.value("payload").toObject();
    const QJsonArray runs = hello.value("runs").toArray();
    QCOMPARE(runs.size(), 1);
    QCOMPARE(runs.at(0).toObject().value("run_id").toString(), id);
    QCOMPARE(runs.at(0).toObject().value("last_seq").toInt(), 3);
    QCOMPARE(hub_->count(QStringLiteral("/api/v1/runs/%1/events").arg(id), "GET"), getsBefore);  // the Hub's catch-up was enough
    QCOMPARE(client_->events(id, 3, 100).size(), 3);
    Q_UNUSED(dataDir);
  }

  void pagesWhenHubSendsNothing() {
    connectHub();
    const QString id = seedRun(2);
    startClient();
    QTRY_COMPARE_WITH_TIMEOUT(client_->lastSeq(id), 3, 8000);
    client_.reset();
    socket_.reset();
    for (int i = 0; i < 6; ++i) hub_->addRunEvent(id, QStringLiteral("test.event"), QStringLiteral("late%1").arg(i), hub_->runsMe, kPlugin, {}, false);
    hub_->runHelloCatchUpMax = 2;  // 6 missing: the Hub sends none
    const int getsBefore = hub_->count(QStringLiteral("/api/v1/runs/%1/events").arg(id), "GET");
    startClient();
    QTRY_COMPARE_WITH_TIMEOUT(client_->lastSeq(id), 9, 8000);
    QVERIFY(hub_->count(QStringLiteral("/api/v1/runs/%1/events").arg(id), "GET") > getsBefore);
    const QList<RunEvent> got = client_->events(id, 3, 100);
    QCOMPARE(got.size(), 6);
    QCOMPARE(got.first().seq, 4);
    QCOMPARE(got.last().seq, 9);
  }

  void gapTriggersPaging() {
    connectHub();
    const QString id = seedRun(0);
    startClient();
    QTRY_COMPARE_WITH_TIMEOUT(client_->lastSeq(id), 1, 8000);
    hub_->addRunEvent(id, QStringLiteral("test.event"), QStringLiteral("hidden1"), hub_->runsMe, kPlugin, {}, false);
    hub_->addRunEvent(id, QStringLiteral("test.event"), QStringLiteral("hidden2"), hub_->runsMe, kPlugin, {}, false);
    hub_->addRunEvent(id, QStringLiteral("test.event"), QStringLiteral("seen"), hub_->runsMe, kPlugin, {}, true);  // seq 4 only via WSS
    QTRY_COMPARE_WITH_TIMEOUT(client_->lastSeq(id), 4, 8000);
    const QList<RunEvent> got = client_->events(id, 1, 100);
    QCOMPARE(got.size(), 3);
    QCOMPARE(got.at(0).seq, 2);
    QCOMPARE(got.at(2).text, QStringLiteral("seen"));
  }

  void eventPostRetriesIdempotently() {
    connectHub();
    const QString id = seedRun(0);
    startClient();
    waitForRun(id);
    hub_->runEventDropResponses = 1;  // the Hub stores the event but the answer never arrives
    QString cid;
    QCOMPARE(client_->postEvent(id, kPlugin, QStringLiteral("automatic"), QStringLiteral("test.caught"), QStringLiteral("hello"), QJsonObject{{"n", 1}}, &cid), QString());
    QVERIFY(!cid.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(hub_->runEventPosts.size(), 2, 8000);
    QCOMPARE(hub_->runEventPosts.at(0).value("client_id").toString(), cid);
    QCOMPARE(hub_->runEventPosts.at(1).value("client_id").toString(), cid);
    QTRY_COMPARE_WITH_TIMEOUT(client_->pendingEvents(id), 0, 8000);
    int mine = 0;
    for (const QJsonObject& e : hub_->runEvents.value(id))
      if (e.value("client_id").toString() == cid) ++mine;
    QCOMPARE(mine, 1);  // one event on the Hub
    QTRY_VERIFY_WITH_TIMEOUT(!client_->events(id, 0, 100).isEmpty() && client_->events(id, 0, 100).last().clientId == cid, 8000);
    QCOMPARE(client_->events(id, 0, 100).last().payload.value("n").toInt(), 1);
  }

  void eventsKeepOrderAndRetryOn503() {
    connectHub();
    const QString id = seedRun(0);
    startClient();
    waitForRun(id);
    hub_->runFailNext = 2;
    for (int i = 0; i < 3; ++i)
      QCOMPARE(client_->postEvent(id, kPlugin, QStringLiteral("manual"), QStringLiteral("test.n"), QString::number(i), {}), QString());
    QTRY_COMPARE_WITH_TIMEOUT(client_->pendingEvents(id), 0, 8000);
    QStringList order;
    for (const QJsonObject& e : hub_->runEvents.value(id))
      if (e.value("type").toString() == QLatin1String("test.n")) order << e.value("text").toString();
    QCOMPARE(order, (QStringList{"0", "1", "2"}));
  }

  void stateIsCoalescedPerPluginAndConflictAdoptsRev() {
    connectHub();
    const QString id = seedRun(0);
    startClient();
    waitForRun(id);
    // Two plugins write in parallel: each is coalesced on its own and has its own revision.
    for (int i = 1; i <= 12; ++i) {
      QCOMPARE(client_->setOwnState(id, kPlugin, QJsonObject{{"n", i}}), QString());
      QCOMPARE(client_->setOwnState(id, kPlugin2, QJsonObject{{"m", i * 2}}), QString());
    }
    QCOMPARE(client_->ownState(id, kPlugin).value("n").toInt(), 12);
    QCOMPARE(client_->ownState(id, kPlugin2).value("m").toInt(), 24);
    const auto lastFor = [&](const QString& plugin) {
      int n = 0, count = 0;
      for (const QJsonObject& put : std::as_const(hub_->runStatePuts))
        if (put.value("plugin_id").toString() == plugin) {
          n = put.value("state").toObject().value(plugin == kPlugin ? "n" : "m").toInt();
          ++count;
        }
      return QPair<int, int>(n, count);
    };
    QTRY_VERIFY_WITH_TIMEOUT(lastFor(kPlugin).first == 12 && lastFor(kPlugin2).first == 24, 8000);
    QVERIFY(lastFor(kPlugin).second <= 4);  // coalesced, not 12 writes
    QVERIFY(lastFor(kPlugin2).second <= 4);
    QTRY_COMPARE_WITH_TIMEOUT(client_->run(id)->participant(hub_->runsMe)->stateOf(kPlugin).rev, lastFor(kPlugin).second, 8000);
    QTRY_COMPARE_WITH_TIMEOUT(client_->run(id)->participant(hub_->runsMe)->stateOf(kPlugin2).rev, lastFor(kPlugin2).second, 8000);

    // A plugin that is not part of the run is refused locally.
    QVERIFY(!client_->setOwnState(id, QStringLiteral("net.other.plugin"), QJsonObject{{"x", 1}}).isEmpty());

    // 409: the Hub reports the revision of that plugin; the latest local value is sent again on it.
    hub_->runStatePuts.clear();
    hub_->runStateConflicts = 1;
    hub_->runConflictRev = 40;
    QCOMPARE(client_->setOwnState(id, kPlugin, QJsonObject{{"n", 100}}), QString());
    QTRY_COMPARE_WITH_TIMEOUT(hub_->runStatePuts.size(), 2, 8000);
    QCOMPARE(hub_->runStatePuts.at(1).value("plugin_id").toString(), kPlugin);
    QCOMPARE(hub_->runStatePuts.at(1).value("base_rev").toInt(), 40);
    QCOMPARE(hub_->runStatePuts.at(1).value("state").toObject().value("n").toInt(), 100);
    QTRY_COMPARE_WITH_TIMEOUT(client_->run(id)->participant(hub_->runsMe)->stateOf(kPlugin).rev, 41, 8000);
    // the other plugin's revision is untouched by that conflict
    QVERIFY(client_->run(id)->participant(hub_->runsMe)->stateOf(kPlugin2).rev < 40);
    const QJsonObject hubStates = hub_->runs.value(id).value("participants").toArray().at(0).toObject().value("states").toObject();
    QCOMPARE(hubStates.value(kPlugin).toObject().value("state").toObject().value("n").toInt(), 100);
    QCOMPARE(hubStates.value(kPlugin2).toObject().value("state").toObject().value("m").toInt(), 24);
  }

  void stateOfOthersArrivesOverWssPerPlugin() {
    connectHub();
    const QString id = seedRun(0);
    startClient();
    waitForRun(id);
    QSignalSpy st(client_.get(), &RunClient::stateChanged);
    hub_->sendWs(QStringLiteral("run_state"),
                 {{"run_id", id}, {"user_id", kOther}, {"plugin_id", kPlugin2}, {"state_rev", 3}, {"state", QJsonObject{{"hp", 12}}}});
    QTRY_COMPARE_WITH_TIMEOUT(st.size(), 1, 8000);
    QCOMPARE(st.at(0).at(1).toString(), kOther);
    QCOMPARE(st.at(0).at(2).toString(), kPlugin2);
    QCOMPARE(client_->run(id)->participant(kOther)->stateOf(kPlugin2).state.value("hp").toInt(), 12);
    QVERIFY(client_->run(id)->participant(kOther)->stateOf(kPlugin).state.isEmpty());
    QCOMPARE(client_->participantState(id, kOther, kPlugin2).value().value("hp").toInt(), 12);
    // an older revision of the same plugin is ignored, a first revision of the other plugin is not
    hub_->sendWs(QStringLiteral("run_state"),
                 {{"run_id", id}, {"user_id", kOther}, {"plugin_id", kPlugin2}, {"state_rev", 2}, {"state", QJsonObject{{"hp", 1}}}});
    hub_->sendWs(QStringLiteral("run_state"),
                 {{"run_id", id}, {"user_id", kOther}, {"plugin_id", kPlugin}, {"state_rev", 1}, {"state", QJsonObject{{"hp", 5}}}});
    QTRY_COMPARE_WITH_TIMEOUT(client_->run(id)->participant(kOther)->stateOf(kPlugin).state.value("hp").toInt(), 5, 8000);
    QCOMPARE(client_->run(id)->participant(kOther)->stateOf(kPlugin2).state.value("hp").toInt(), 12);
    // a plugin that is not in the run is dropped
    hub_->sendWs(QStringLiteral("run_state"),
                 {{"run_id", id}, {"user_id", kOther}, {"plugin_id", QStringLiteral("net.other.plugin")}, {"state_rev", 1}, {"state", QJsonObject{{"hp", 9}}}});
    QTest::qWait(150);
    QVERIFY(!client_->run(id)->participant(kOther)->states.contains(QStringLiteral("net.other.plugin")));
  }

  void statesAreCachedPerUserAndPlugin() {
    connectHub();
    const QString id = seedRun(0);
    {
      QJsonObject run = hub_->runs.value(id);
      QJsonArray parts = run.value("participants").toArray();
      QJsonObject other = parts.at(1).toObject();
      other.insert("states", QJsonObject{{kPlugin, QJsonObject{{"state", QJsonObject{{"a", 1}}}, {"state_rev", 4}, {"updated_at", "2026-10-12T10:00:00Z"}}},
                                         {kPlugin2, QJsonObject{{"state", QJsonObject{{"b", 2}}}, {"state_rev", 6}, {"updated_at", "2026-10-12T10:00:00Z"}}}});
      parts.replace(1, other);
      run.insert("participants", parts);
      hub_->runs.insert(id, run);
    }
    startClient();
    waitForRun(id);
    QCOMPARE(client_->participantState(id, kOther, kPlugin).value().value("a").toInt(), 1);
    QCOMPARE(client_->participantState(id, kOther, kPlugin2).value().value("b").toInt(), 2);
    client_.reset();  // writes the cache
    socket_.reset();
    // The Hub answers with older revisions (e.g. a restored backup): the cached newer ones fill in, per plugin.
    {
      QJsonObject run = hub_->runs.value(id);
      QJsonArray parts = run.value("participants").toArray();
      QJsonObject other = parts.at(1).toObject();
      other.insert("states", QJsonObject{{kPlugin, QJsonObject{{"state", QJsonObject{{"a", 99}}}, {"state_rev", 9}, {"updated_at", "2026-10-12T11:00:00Z"}}}});
      parts.replace(1, other);
      run.insert("participants", parts);
      hub_->runs.insert(id, run);
    }
    startClient();
    waitForRun(id);
    const RunParticipant* o = client_->run(id)->participant(kOther);
    QCOMPARE(o->stateOf(kPlugin).state.value("a").toInt(), 99);  // the Hub's newer revision wins
    QCOMPARE(o->stateOf(kPlugin).rev, 9);
    QCOMPARE(o->stateOf(kPlugin2).state.value("b").toInt(), 2);  // only the cache knows this plugin
    QCOMPARE(o->stateOf(kPlugin2).rev, 6);
    // the cache file stores them keyed by user and plugin
    client_.reset();
    QDir cacheRoot(dir_->filePath(QStringLiteral("runs-data/runs")));
    QVERIFY(cacheRoot.exists());
    int found = 0;
    QDirIterator it(cacheRoot.path(), {id + ".json"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      QFile f(it.next());
      QVERIFY(f.open(QIODevice::ReadOnly));
      const QJsonObject states = QJsonDocument::fromJson(f.readAll()).object().value("states").toObject();
      QCOMPARE(states.value(kOther).toObject().value(kPlugin2).toObject().value("state_rev").toInt(), 6);
      ++found;
    }
    QCOMPARE(found, 1);
  }

  void writesRefusedLocallyWhenNotWritable() {
    connectHub();
    const QString id = seedRun(0, QStringLiteral("paused"));
    startClient();
    waitForRun(id);
    QVERIFY(client_->setOwnState(id, kPlugin, QJsonObject{{"a", 1}}).contains(QStringLiteral("paused")));
    QVERIFY(client_->postEvent(id, kPlugin, QStringLiteral("manual"), QStringLiteral("test.n"), {}, {}).contains(QStringLiteral("paused")));
    QVERIFY(client_->setBoard(id, kPlugin, QJsonObject{{"type", "text"}}).contains(QStringLiteral("paused")));
    QTest::qWait(100);
    QVERIFY(hub_->runStatePuts.isEmpty());
    QVERIFY(hub_->runEventPosts.isEmpty());
    QCOMPARE(hub_->runBoardPuts, 0);
    // invalid input is refused with a clear text, too
    QVERIFY(!client_->postEvent(id, kPlugin, QStringLiteral("bogus"), QStringLiteral("t"), {}, {}).isEmpty());
    QVERIFY(!client_->postEvent(id, QStringLiteral("net.other.plugin"), QStringLiteral("manual"), QStringLiteral("t"), {}, {}).isEmpty());
    QVERIFY(!client_->postEvent(QStringLiteral("00000000-0000-4000-8000-00000000dead"), kPlugin, QStringLiteral("manual"), QStringLiteral("t"), {}, {}).isEmpty());
  }

  void boardIsRateLimited() {
    connectHub();
    const QString id = seedRun(0);
    startClient();
    waitForRun(id);
    for (int i = 1; i <= 8; ++i) QCOMPARE(client_->setBoard(id, kPlugin, QJsonObject{{"type", "text"}, {"text", QString::number(i)}}), QString());
    QTRY_VERIFY_WITH_TIMEOUT(hub_->runBoards.contains(id + "/" + kPlugin) && hub_->runBoards.value(id + "/" + kPlugin).value("text").toString() == "8", 8000);
    QVERIFY(hub_->runBoardPuts <= 3);
  }

  void pausedRunResumesQueuedEvents() {
    connectHub();
    const QString id = seedRun(0);
    startClient();
    waitForRun(id);
    QJsonObject r = hub_->runs.value(id);
    r.insert("status", "paused");
    hub_->runs.insert(id, r);
    hub_->pushRun(id);
    QTRY_VERIFY_WITH_TIMEOUT(client_->run(id)->status == QLatin1String("paused"), 8000);
    // a run that just paused: writes are refused, nothing is lost silently
    QVERIFY(!client_->postEvent(id, kPlugin, QStringLiteral("manual"), QStringLiteral("test.n"), {}, {}).isEmpty());
    r.insert("status", "active");
    hub_->runs.insert(id, r);
    hub_->pushRun(id);
    QTRY_VERIFY_WITH_TIMEOUT(client_->run(id)->status == QLatin1String("active"), 8000);
    QCOMPARE(client_->postEvent(id, kPlugin, QStringLiteral("manual"), QStringLiteral("test.n"), {}, {}), QString());
    QTRY_COMPARE_WITH_TIMEOUT(client_->pendingEvents(id), 0, 8000);
  }
};

QTEST_MAIN(RunClientTest)
#include "runclient_test.moc"
