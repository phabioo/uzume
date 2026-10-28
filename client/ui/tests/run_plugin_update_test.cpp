// Updating the pinned plugins of a run (run_plugin_updates_v1): the offer (newest semver-greater version, required plugins bundled, restore of
// previous_version), the permissions a switch adds, the restart decision and the log row of plugins.updated. Pure logic, no window.
#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "runpluginupdate.h"

using namespace uzume;
using namespace uzume::ui;

namespace {
HubPluginInfo offer(const QString& id, const QString& version, const QString& name, const QStringList& perms = {QStringLiteral("run.state")},
                    const QJsonObject& requires_ = {}) {
  HubPluginInfo p;
  p.id = id;
  p.version = version;
  p.name = name;
  p.permissions = perms;
  p.requires_ = requires_;
  return p;
}
RunInfo runWith(const QList<RunPluginRef>& pins) {
  RunInfo r;
  r.runId = QStringLiteral("11111111-1111-4111-8111-111111111111");
  r.plugins = pins;
  return r;
}
}  // namespace

class RunPluginUpdateTest : public QObject {
  Q_OBJECT
 private slots:
  void offersTheNewestGreaterVersionOnly() {
    const RunInfo run = runWith({{"net.t.a", "1.0.0", "aa", {}}});
    QVERIFY(runplugins::changes(run, {offer("net.t.a", "1.0.0", "A")}).isEmpty());  // nothing newer
    QVERIFY(runplugins::changes(run, {offer("net.t.a", "0.9.0", "A")}).isEmpty());  // older is no update
    const auto c = runplugins::changes(run, {offer("net.t.a", "1.0.0", "A"), offer("net.t.a", "1.2.0", "A"), offer("net.t.a", "1.1.0", "A")});
    QCOMPARE(c.size(), 1);
    QCOMPARE(c.first().to, QStringLiteral("1.2.0"));
    QCOMPARE(c.first().from, QStringLiteral("1.0.0"));
    QVERIFY(!c.first().restore);
    QCOMPARE(c.first().request.size(), 1);
    QCOMPARE(c.first().request.first().version, QStringLiteral("1.2.0"));
    QVERIFY(c.first().addedPermissions.isEmpty());
  }

  void bundlesRequiredPluginsThatNoLongerFit() {
    const RunInfo run = runWith({{"net.t.soul", "1.0.0", "aa", {}}, {"net.t.core", "0.1.0", "bb", {}}});
    const QList<HubPluginInfo> list{offer("net.t.soul", "1.0.0", "SoulLink"),
                                    offer("net.t.soul", "1.1.0", "SoulLink", {"run.state"}, {{"net.t.core", "^0.2"}}),
                                    offer("net.t.core", "0.1.0", "Core"), offer("net.t.core", "0.2.0", "Core"), offer("net.t.core", "0.2.5", "Core"),
                                    offer("net.t.core", "0.3.0", "Core")};
    const auto c = runplugins::changes(run, list);
    // soul is offered with core 0.2.5 (newest that satisfies ^0.2); core itself is offered 0.3.0 alone.
    const runplugins::Change* soul = nullptr;
    for (const auto& x : c)
      if (x.pluginId == QLatin1String("net.t.soul")) soul = &x;
    QVERIFY(soul != nullptr);
    QCOMPARE(soul->request.size(), 2);
    QCOMPARE(soul->request.at(0).pluginId, QStringLiteral("net.t.soul"));
    QCOMPARE(soul->request.at(1).pluginId, QStringLiteral("net.t.core"));
    QCOMPARE(soul->request.at(1).version, QStringLiteral("0.2.5"));
    QCOMPARE(soul->bundledNames, QStringList{QStringLiteral("Core")});
    // A requirement that no offered version meets leaves the offer out.
    const auto none = runplugins::changes(run, {offer("net.t.soul", "1.1.0", "SoulLink", {"run.state"}, {{"net.t.core", "^9.0"}})});
    QVERIFY(none.isEmpty());
    // A required plugin the run does not have yet is appended.
    const RunInfo solo = runWith({{"net.t.soul", "1.0.0", "aa", {}}});
    const auto add = runplugins::changes(solo, {offer("net.t.soul", "1.1.0", "SoulLink", {"run.state"}, {{"net.t.core", "*"}}), offer("net.t.core", "0.1.0", "Core", {"memory.read"})});
    QCOMPARE(add.size(), 1);
    QCOMPARE(add.first().request.size(), 2);
    QVERIFY(add.first().addedPermissions.contains(QStringLiteral("memory.read")));
  }

  void restoreNeedsPreviousVersionStillOffered() {
    const RunInfo run = runWith({{"net.t.a", "1.1.0", "bb", "1.0.0"}});
    auto c = runplugins::changes(run, {offer("net.t.a", "1.0.0", "A"), offer("net.t.a", "1.1.0", "A")});
    QCOMPARE(c.size(), 1);
    QVERIFY(c.first().restore);
    QCOMPARE(c.first().to, QStringLiteral("1.0.0"));
    QCOMPARE(c.first().request.first().version, QStringLiteral("1.0.0"));
    c = runplugins::changes(run, {offer("net.t.a", "1.1.0", "A")});  // removed from the server since
    QVERIFY(c.isEmpty());
  }

  void addedPermissionsComeFromTheOfferedVersions() {
    const QList<HubPluginInfo> list{offer("net.t.a", "1.0.0", "A", {"run.state"}), offer("net.t.a", "1.1.0", "A", {"run.state", "cheats", "memory.write"})};
    QCOMPARE(runplugins::addedPermissions(list, "net.t.a", "1.1.0", "1.0.0"), (QStringList{"cheats", "memory.write"}));
    QVERIFY(runplugins::addedPermissions(list, "net.t.a", "1.0.0", "1.1.0").isEmpty());  // restoring asks for nothing new
    QVERIFY(runplugins::addedPermissions(list, "net.t.a", "1.1.0", "").isEmpty());       // never switched
    QCOMPARE(runplugins::addedPermissions(list, "net.t.a", "1.1.0", "0.9.0"), (QStringList{"cheats", "memory.write"}));  // unknown previous: all but run.state
    // The offer of the update lists them too.
    const RunInfo run = runWith({{"net.t.a", "1.0.0", "aa", {}}});
    QCOMPARE(runplugins::changes(run, list).first().addedPermissions, (QStringList{"cheats", "memory.write"}));
  }

  void newPermissionsOfAnAppendedPlugin() {
    const QList<HubPluginInfo> list{offer("net.t.a", "1.0.0", "A", {"run.state", "cheats"})};
    bool known = false;
    QCOMPARE(runplugins::newPermissions(list, "net.t.a", "1.0.0", &known), QStringList{QStringLiteral("cheats")});
    QVERIFY(known);
    QVERIFY(runplugins::newPermissions(list, "net.t.a", "9.9.9", &known).isEmpty());
    QVERIFY(!known);  // stale catalog: unknown, not "none"
  }

  void restartOnlyWhenAPinChanged() {
    const QList<RunPluginRef> a{{"p", "1.0.0", "aa", {}}, {"q", "2.0.0", "bb", {}}};
    QVERIFY(!runplugins::pinsDiffer(a, a));
    QList<RunPluginRef> b = a;
    b[0].previousVersion = QStringLiteral("0.9.0");  // only the restore hint changed
    QVERIFY(!runplugins::pinsDiffer(a, b));
    b = a;
    b[1].version = QStringLiteral("2.1.0");
    QVERIFY(runplugins::pinsDiffer(a, b));
    b = a;
    b[0].sha256 = QStringLiteral("cc");  // same version, other file
    QVERIFY(runplugins::pinsDiffer(a, b));
    b = a;
    b.append({"r", "1.0.0", "dd", {}});
    QVERIFY(runplugins::pinsDiffer(a, b));
  }

  void logRow() {
    const QJsonObject payload{{"changes", QJsonArray{QJsonObject{{"plugin_id", "net.t.a"}, {"from", "1.0.0"}, {"to", "1.1.0"}}}}, {"by_user_name", "Lena"}};
    const auto name = [](const QString& id) { return id == QLatin1String("net.t.a") ? QStringLiteral("Cheats") : id; };
    QCOMPARE(runplugins::eventText("Lena", payload, name), QStringLiteral("Lena updated Cheats 1.0.0 → 1.1.0"));
    QCOMPARE(runplugins::eventText("Lena", {}, name), QStringLiteral("Lena updated the plugins"));
  }
};

QTEST_MAIN(RunPluginUpdateTest)
#include "run_plugin_update_test.moc"
