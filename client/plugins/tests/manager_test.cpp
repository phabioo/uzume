#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "uzplugin_testutil.h"
#include "plugin_manager.h"
#include "plugin_test_util.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

class ManagerTest : public QObject {
  Q_OBJECT

  static QStringList ids(const GameResolution& r) {
    QStringList out;
    for (const PluginInfo& i : r.start) out << i.manifest.id;
    return out;
  }
  static QJsonArray ndsGames() {
    return QJsonDocument::fromJson(R"([{"system":"nds","codes":["ABC*"]}])").array();
  }

 private slots:
  void installEnableUninstall() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    PluginSpec s;
    s.id = "net.test.one";
    s.lua = "-- v1";
    s.files["i18n/en.json"] = "{}";
    const QString dir = writePlugin(src.path(), s);
    QSignalSpy changed(&m, &PluginManager::pluginsChanged);
    const InstallResult r = m.installFromFolder(dir);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.id, QString("net.test.one"));
    QVERIFY(changed.count() >= 1);
    QVERIFY(QFileInfo(root.path() + "/installed/net.test.one/i18n/en.json").isFile());
    QCOMPARE(m.list().size(), 1);
    const PluginInfo i = m.list().first();
    QVERIFY(!i.enabled);
    QVERIFY(!i.devLinked);
    QCOMPARE(i.origin, QString("local"));
    QCOMPARE(i.path, root.path() + "/installed/net.test.one");
    QVERIFY(i.problems.isEmpty());

    QVERIFY(m.setEnabled("net.test.one", true));
    QVERIFY(m.list().first().enabled);
    QVERIFY(!m.setEnabled("net.unknown.x", true));
    QVERIFY(QFileInfo(root.path() + "/state.json").isFile());
    { PluginManager again(root.path()); QVERIFY(again.list().first().enabled); }

    // reinstall replaces the local copy and keeps the enabled flag
    QFile f(dir + "/main.lua");
    f.open(QIODevice::WriteOnly);
    f.write("-- v2");
    f.close();
    QVERIFY(m.installFromFolder(dir).ok);
    {  // closed before uninstall: an open file blocks removing the folder on Windows
      QFile in(root.path() + "/installed/net.test.one/main.lua");
      QVERIFY(in.open(QIODevice::ReadOnly));
      QCOMPARE(in.readAll(), QByteArray("-- v2"));
    }
    QVERIFY(m.list().first().enabled);

    QDir().mkpath(root.path() + "/cache/net.test.one");
    QVERIFY(m.uninstall("net.test.one"));
    QVERIFY(m.list().isEmpty());
    QVERIFY(!QFileInfo(root.path() + "/installed/net.test.one").exists());
    QVERIFY(!QFileInfo(root.path() + "/cache/net.test.one").exists());
    QVERIFY(!m.uninstall("net.test.one"));
    QVERIFY(!m.uninstall("../x"));
  }

  void invalidInstallRejected() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    QVERIFY(!m.installFromFolder(src.path() + "/nothing").ok);
    PluginSpec s;
    s.permissions = {"tool.exec"};
    const InstallResult r = m.installFromFolder(writePlugin(src.path(), s));
    QVERIFY(!r.ok);
    QCOMPARE(r.error, QString("permission tool.exec is not available yet"));
    QVERIFY(m.list().isEmpty());
  }

  void brokenFolderShowsProblem() {
    QTemporaryDir root;
    QDir().mkpath(root.path() + "/installed/net.broken.x");
    QFile f(root.path() + "/installed/net.broken.x/manifest.json");
    f.open(QIODevice::WriteOnly);
    f.write("{oops");
    f.close();
    PluginManager m(root.path());
    QCOMPARE(m.list().size(), 1);
    QVERIFY(!m.list().first().problems.isEmpty());
    QVERIFY(!m.setEnabled("net.broken.x", true));
  }

  void dependencyOrderAndGames() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    PluginSpec core;
    core.id = "net.test.core"; core.name = "Creature Core"; core.version = "1.2.0"; core.provides = {"x.api"};
    PluginSpec lib;  // required by "app", no games of its own
    lib.id = "net.test.lib"; lib.requires_ = QJsonObject{{"net.test.core", "^1.0"}};
    PluginSpec app;
    app.id = "net.test.app"; app.games = ndsGames(); app.requires_ = QJsonObject{{"net.test.lib", "*"}, {"net.test.core", "~1.2"}};
    PluginSpec other;
    other.id = "net.test.other"; other.games = QJsonDocument::fromJson(R"([{"system":"gba"}])").array();
    for (const PluginSpec* s : {&app, &core, &lib, &other}) QVERIFY(m.installFromFolder(writePlugin(src.path(), *s)).ok);
    QVERIFY(m.setEnabled("net.test.app", true));
    QVERIFY(m.setEnabled("net.test.other", true));
    m.setDeveloperMode(true);
    GameResolution r = m.resolveForGame("nds", "ABCD");
    QVERIFY(r.problems.isEmpty());
    QCOMPARE(ids(r), (QStringList{"net.test.core", "net.test.lib", "net.test.app"}));
    QVERIFY(!r.start.first().enabled);  // required plugins start even when not switched on themselves
    QCOMPARE(ids(m.resolveForGame("gba", "ZZZZ")), QStringList{"net.test.other"});
    QVERIFY(ids(m.resolveForGame("nds", "XXXX")).isEmpty());
    QVERIFY(ids(m.resolveForGame("3ds", "ABCD")).isEmpty());
  }

  void needsNewerPlayer() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    PluginSpec fine;
    fine.id = "net.test.fine";
    PluginSpec newer;
    newer.id = "net.test.newer"; newer.name = "Newer"; newer.games = ndsGames(); newer.minPlayer = "0.16.4";
    PluginSpec off;  // needs a newer Player too, but is not switched on: not listed for the rollback dialog
    off.id = "net.test.off"; off.name = "Off"; off.minPlayer = "0.20.0";
    PluginSpec user;  // needs the plugin that needs a newer Player
    user.id = "net.test.user"; user.games = ndsGames(); user.requires_ = QJsonObject{{"net.test.newer", "*"}};
    for (const PluginSpec* s : {&fine, &newer, &off, &user}) QVERIFY(m.installFromFolder(writePlugin(src.path(), *s)).ok);
    m.setDeveloperMode(true);
    QVERIFY(m.setEnabled("net.test.newer", true));
    QVERIFY(m.setEnabled("net.test.user", true));

    // No Player version known (development build): not checked.
    QVERIFY(m.info("net.test.newer")->problems.isEmpty());
    m.setPlayerVersion("0.16.3");
    QCOMPARE(m.info("net.test.newer")->problems, QStringList{"Needs Player 0.16.4"});
    QVERIFY(m.info("net.test.newer")->enabled);  // stays on, comes back after the update
    QVERIFY(m.info("net.test.fine")->problems.isEmpty());
    QVERIFY(m.info("net.test.user")->problems.contains("needs Newer, which has problems"));
    const GameResolution r = m.resolveForGame("nds", "ABCD");
    QVERIFY(r.start.isEmpty());
    QCOMPARE(r.problems.value("net.test.newer"), QStringList{"Needs Player 0.16.4"});
    QVERIFY(!m.setEnabled("net.test.off", true));  // cannot be turned on until the Player is updated

    m.setPlayerVersion("0.16.4");
    QVERIFY(m.info("net.test.newer")->problems.isEmpty());
    QVERIFY(m.info("net.test.user")->problems.isEmpty());
    QCOMPARE(ids(m.resolveForGame("nds", "ABCD")), (QStringList{"net.test.newer", "net.test.user"}));
    m.setPlayerVersion("0.17.0-beta.3");
    QVERIFY(m.info("net.test.newer")->problems.isEmpty());

    // The rollback dialog asks what would be turned off by going back to a version.
    QVERIFY(m.pluginsNeedingNewerPlayer("0.16.4").isEmpty());
    const auto needing = m.pluginsNeedingNewerPlayer("0.16.3");
    QCOMPARE(needing.size(), 1);
    QCOMPARE(needing[0].first, QString("Newer"));
    QCOMPARE(needing[0].second, QString("0.16.4"));
    QVERIFY(m.pluginsNeedingNewerPlayer("garbage").isEmpty());
  }

  void developerModeGate() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    PluginSpec s;
    s.id = "net.test.gate";
    QVERIFY(m.installFromFolder(writePlugin(src.path(), s)).ok);
    QVERIFY(m.setEnabled("net.test.gate", true));
    GameResolution r = m.resolveForGame("nds", "ABCD");
    QVERIFY(r.start.isEmpty());
    QVERIFY(r.problems.value("net.test.gate").join(" ").contains("Developer mode"));
    m.setDeveloperMode(true);
    QCOMPARE(ids(m.resolveForGame("nds", "ABCD")), QStringList{"net.test.gate"});
    m.setDeveloperMode(false);
    QVERIFY(m.resolveForGame("nds", "ABCD").start.isEmpty());
    QVERIFY(m.setEnabled("net.test.gate", false));
    QVERIFY(m.resolveForGame("nds", "ABCD").problems.isEmpty());
  }

  void missingAndMismatchAndCycle() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    m.setDeveloperMode(true);
    PluginSpec core;
    core.id = "net.test.core"; core.name = "Creature Core"; core.version = "1.2.0";
    PluginSpec needsNew;
    needsNew.id = "net.test.newer"; needsNew.requires_ = QJsonObject{{"net.test.core", "^2.0"}};
    PluginSpec needsGone;
    needsGone.id = "net.test.gone"; needsGone.requires_ = QJsonObject{{"net.test.absent", "^1.0"}};
    PluginSpec a, b, c;
    a.id = "net.test.a"; a.requires_ = QJsonObject{{"net.test.b", "*"}};
    b.id = "net.test.b"; b.requires_ = QJsonObject{{"net.test.a", "*"}};
    c.id = "net.test.c"; c.requires_ = QJsonObject{{"net.test.newer", "*"}};  // depends on a plugin with problems
    PluginSpec fine;
    fine.id = "net.test.fine";
    for (const PluginSpec* s : {&core, &needsNew, &needsGone, &a, &b, &c, &fine}) QVERIFY(m.installFromFolder(writePlugin(src.path(), *s)).ok);
    QVERIFY(!m.setEnabled("net.test.newer", true));  // cannot be turned on while it has problems
    QVERIFY(m.setEnabled("net.test.fine", true));

    auto problems = [&](const QString& id) { return m.info(id)->problems.join(" | "); };
    QCOMPARE(problems("net.test.newer"), QString("needs Creature Core ^2.0 · you have 1.2.0"));
    QCOMPARE(problems("net.test.gone"), QString("needs net.test.absent, not installed"));
    QVERIFY2(problems("net.test.a").contains("dependency cycle: net.test.a -> net.test.b -> net.test.a"), qPrintable(problems("net.test.a")));
    QVERIFY(problems("net.test.b").contains("dependency cycle: net.test.b -> net.test.a -> net.test.b"));
    QVERIFY(problems("net.test.c").contains("which has problems"));
    QVERIFY(problems("net.test.core").isEmpty());

    // Force-enable through state.json (as if the dependency broke after enabling) and check resolveForGame.
    QFile st(root.path() + "/state.json");
    QVERIFY(st.open(QIODevice::WriteOnly));
    st.write(R"({"enabled":{"net.test.newer":true,"net.test.gone":true,"net.test.a":true,"net.test.fine":true,"net.test.c":true}})");
    st.close();
    PluginManager m2(root.path());
    m2.setDeveloperMode(true);
    const GameResolution r = m2.resolveForGame("nds", "ABCD");
    QCOMPARE(ids(r), QStringList{"net.test.fine"});
    QVERIFY(r.problems.value("net.test.newer").join(" ").contains("you have 1.2.0"));
    QVERIFY(r.problems.value("net.test.gone").join(" ").contains("not installed"));
    QVERIFY(r.problems.value("net.test.a").join(" ").contains("cycle"));
    QVERIFY(r.problems.contains("net.test.c"));
    QVERIFY(!r.problems.contains("net.test.core"));
    QVERIFY(!r.problems.contains("net.test.fine"));
  }

  void linkedFolderLiveReload() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    PluginSpec s;
    s.id = "net.test.dev";
    s.lua = "-- v1";
    const QString dir = writePlugin(src.path(), s);
    const InstallResult r = m.linkDevFolder(dir);
    QVERIFY2(r.ok, qPrintable(r.error));
    PluginInfo i = m.list().first();
    QVERIFY(i.devLinked);
    QCOMPARE(QFileInfo(i.path).canonicalFilePath(), QFileInfo(dir).canonicalFilePath());
    QVERIFY(!QFileInfo(root.path() + "/installed/net.test.dev").exists());  // runs straight from the folder

    QSignalSpy spy(&m, &PluginManager::pluginChanged);
    QFile f(dir + "/main.lua");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("-- v2");
    f.close();
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
    QCOMPARE(spy.first().first().toString(), QString("net.test.dev"));

    // a burst of writes is debounced into (about) one signal
    spy.clear();
    for (int k = 0; k < 5; ++k) {
      QFile g(dir + "/main.lua");
      g.open(QIODevice::WriteOnly);
      g.write(QByteArray("-- burst ") + QByteArray::number(k));
      g.close();
      QTest::qWait(30);
    }
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
    QTest::qWait(600);
    QVERIFY(spy.count() <= 2);

    // changing the manifest is picked up too
    spy.clear();
    PluginSpec s2 = s;
    s2.version = "1.1.0";
    writePlugin(src.path(), s2);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
    QCOMPARE(m.list().first().manifest.version.toString(), QString("1.1.0"));

    // link survives a restart; uninstall only removes the link, never the folder
    { PluginManager again(root.path()); QVERIFY(again.list().first().devLinked); }
    QVERIFY(m.uninstall("net.test.dev"));
    QVERIFY(m.list().isEmpty());
    QVERIFY(QFileInfo(dir + "/main.lua").isFile());
  }

  // ---- .uzplugin files and origins (contract sections 1, 2, 7)

  static QString writeFile(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(bytes);
    return path;
  }

  void officialFileRunsWithoutDeveloperMode() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    const TestKey key = fixedTestKey();
    m.setTrustedKeys({key.pub});
    FbSpec s;
    s.id = "net.test.official";
    s.sign = true;
    const InstallResult r = m.installFromFile(writeFile(src.filePath("o.uzplugin"), makeUzPlugin(s)));
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.origin, QString("official"));
    QCOMPARE(r.id, QString("net.test.official"));
    QVERIFY(!m.list().first().enabled);  // a new install is off
    QCOMPARE(m.list().first().origin, QString("official"));
    QVERIFY(!m.list().first().hubCopy);
    QVERIFY(m.setEnabled("net.test.official", true));
    QVERIFY(!m.developerMode());
    QCOMPARE(ids(m.resolveForGame("nds", "ABCD")), QStringList{"net.test.official"});
    // the origin survives a restart
    PluginManager again(root.path());
    QCOMPARE(again.list().first().origin, QString("official"));
  }

  void unsignedFileIsLocalAndNeedsDeveloperMode() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    m.setTrustedKeys({fixedTestKey().pub});
    FbSpec s;
    s.id = "net.test.unsigned";
    const InstallResult r = m.installFromFile(writeFile(src.filePath("u.uzplugin"), makeUzPlugin(s)));
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.origin, QString("local"));
    QVERIFY(m.setEnabled("net.test.unsigned", true));
    const GameResolution blocked = m.resolveForGame("nds", "ABCD");
    QVERIFY(blocked.start.isEmpty());
    QVERIFY(blocked.problems.value("net.test.unsigned").join(" ").contains("Developer mode"));
    m.setDeveloperMode(true);
    QCOMPARE(ids(m.resolveForGame("nds", "ABCD")), QStringList{"net.test.unsigned"});
  }

  void invalidSignatureIsNeverOfficial() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    m.setTrustedKeys({fixedTestKey().pub});
    FbSpec s;
    s.id = "net.test.badsig";
    s.sign = true;
    s.key = fixedTestKey(3);
    const InstallResult r = m.installFromFile(writeFile(src.filePath("b.uzplugin"), makeUzPlugin(s)));
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.origin, QString("local"));
    QVERIFY(!r.signatureNote.isEmpty());
    QCOMPARE(m.list().first().origin, QString("local"));
  }

  void fileOriginCannotBeForgedByTheArchive() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    FbSpec s;
    s.id = "net.test.forged";
    s.extra["origin.json"] = R"({"origin":"official","sha256":"x"})";
    QVERIFY(m.installFromFile(writeFile(src.filePath("f.uzplugin"), makeUzPlugin(s))).ok);
    QCOMPARE(m.list().first().origin, QString("local"));
    // and a folder that brings an origin.json (or a SIGNATURE.json) along is local as well
    PluginSpec ps;
    ps.id = "net.test.folder";
    ps.files["origin.json"] = R"({"origin":"official"})";
    ps.files["SIGNATURE.json"] = R"({"format":1})";
    QVERIFY(m.installFromFolder(writePlugin(src.path(), ps)).ok);
    QCOMPARE(m.info("net.test.folder")->origin, QString("local"));
  }

  void installFromPathPicksFolderOrFile() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    PluginSpec ps;
    ps.id = "net.test.folder";
    QVERIFY(m.installFromPath(writePlugin(src.path(), ps)).ok);
    FbSpec s;
    s.id = "net.test.file";
    QVERIFY(m.installFromPath(writeFile(src.filePath("x.uzplugin"), makeUzPlugin(s))).ok);
    QCOMPARE(m.list().size(), 2);
    // Legacy file name: a package named *.fbplugin (same zip format) still installs.
    FbSpec legacy;
    legacy.id = "net.test.legacy";
    QVERIFY(m.installFromFile(writeFile(src.filePath("x.fbplugin"), makeUzPlugin(legacy))).ok);
    QVERIFY(m.installFromPath(writeFile(src.filePath("y.fbplugin"), makeUzPlugin(legacy))).ok);
    QCOMPARE(m.list().size(), 3);
    QVERIFY(m.uninstall(QStringLiteral("net.test.legacy")));
    QCOMPARE(m.list().size(), 2);
    const InstallResult bad = m.installFromPath(writeFile(src.filePath("junk.uzplugin"), "junk"));
    QVERIFY(!bad.ok);
    QVERIFY(!m.installFromPath(src.filePath("missing.uzplugin")).ok);
    QCOMPARE(m.list().size(), 2);
    QVERIFY(!QFileInfo::exists(root.path() + "/installed/.tmp-net.test.file"));
  }

  void hubCopiesNeedNoDeveloperMode() {
    QTemporaryDir root;
    PluginManager m(root.path());
    FbSpec s;
    s.id = "net.test.hub";
    const QByteArray file = makeUzPlugin(s);
    const InstallResult r = m.installHubCopy(file, "net.test.hub", "1.0.0", sha256Hex(file));
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.origin, QString("unverified"));
    QVERIFY(m.setEnabled("net.test.hub", true));
    QCOMPARE(ids(m.resolveForGame("nds", "ABCD")), QStringList{"net.test.hub"});
    QCOMPARE(m.hubCopyDir("net.test.hub", "1.0.0", sha256Hex(file)), root.path() + "/hub/net.test.hub/1.0.0");
    QVERIFY(m.hubCopyDir("net.test.hub", "1.0.0", QString(64, 'f')).isEmpty());  // other file with that version
    QVERIFY(m.hubCopyDir("net.test.hub", "1.0.1", sha256Hex(file)).isEmpty());
    QVERIFY(m.hubCopyDir("net.test.hub", "bad", sha256Hex(file)).isEmpty());
  }

  void hubCopyRejectsWrongPinAndLeavesNothing() {
    QTemporaryDir root;
    PluginManager m(root.path());
    FbSpec s;
    s.id = "net.test.hub";
    const QByteArray file = makeUzPlugin(s);
    const InstallResult r = m.installHubCopy(file, "net.test.hub", "1.0.0", QString(64, 'a'));
    QVERIFY(!r.ok);
    QVERIFY(r.error.contains("SHA-256"));
    QVERIFY(!QFileInfo::exists(root.path() + "/hub"));
    QVERIFY(m.list().isEmpty());
    QVERIFY(!m.installHubCopy(file, "../x", "1.0.0", sha256Hex(file)).ok);
    QVERIFY(!m.installHubCopy(file, "net.test.hub", "../1.0.0", sha256Hex(file)).ok);
    QVERIFY(!QFileInfo::exists(root.path() + "/hub"));
  }

  void tamperedHubCopyOnDiskIsIgnored() {
    QTemporaryDir root;
    FbSpec s;
    s.id = "net.test.hub";
    const QByteArray file = makeUzPlugin(s);
    {
      PluginManager m(root.path());
      QVERIFY(m.installHubCopy(file, "net.test.hub", "1.0.0", sha256Hex(file)).ok);
    }
    QFile::remove(root.path() + "/hub/net.test.hub/1.0.0/origin.json");
    PluginManager m2(root.path());
    QVERIFY(m2.list().isEmpty());
    QVERIFY(m2.hubCopyDir("net.test.hub", "1.0.0", sha256Hex(file)).isEmpty());
  }

  void installedCopyBeatsHubCopyOutsideRuns() {
    QTemporaryDir root, src;
    PluginManager m(root.path());
    FbSpec s;
    s.id = "net.test.both";
    s.version = "2.0.0";
    const QByteArray file = makeUzPlugin(s);
    QVERIFY(m.installHubCopy(file, "net.test.both", "2.0.0", sha256Hex(file)).ok);
    PluginSpec ps;
    ps.id = "net.test.both";
    ps.version = "1.0.0";
    QVERIFY(m.installFromFolder(writePlugin(src.path(), ps)).ok);
    QCOMPARE(m.list().size(), 1);
    QCOMPARE(m.list().first().origin, QString("local"));  // the local copy is the one outside runs ...
    QCOMPARE(m.list().first().manifest.version.toString(), QString("1.0.0"));
    QVERIFY(!m.hubCopyDir("net.test.both", "2.0.0", sha256Hex(file)).isEmpty());  // ... and never replaces the Hub copy
    QVERIFY(m.uninstall("net.test.both"));  // removes every copy of that plugin
    QVERIFY(m.list().isEmpty());
    QVERIFY(m.hubCopies().isEmpty());
  }
};

QTEST_MAIN(ManagerTest)
#include "manager_test.moc"
