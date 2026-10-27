#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

#include "plugin_test_util.h"
#include "pluginhub.h"

using namespace uzume::plugins;
using uzume::update::SemVer;

class ManifestTest : public QObject {
  Q_OBJECT

  static QByteArray base(const QString& patch = {}) {
    QJsonObject o = QJsonDocument::fromJson(R"({"id":"net.example.tracker","name":"Tracker","version":"1.2.3","api":1,"entry":"main.lua"})").object();
    if (!patch.isEmpty()) {
      const QJsonObject p = QJsonDocument::fromJson(patch.toUtf8()).object();
      for (auto it = p.begin(); it != p.end(); ++it) {
        if (it.value().isNull()) o.remove(it.key());
        else o.insert(it.key(), it.value());
      }
    }
    return QJsonDocument(o).toJson();
  }
  static QString errOf(const QString& patch) {
    const ManifestResult r = parseManifest(base(patch));
    return r.ok ? QString() : r.error;
  }

 private slots:
  void validFull() {
    const ManifestResult r = parseManifest(R"({
      "id": "net.uzume.example", "name": "Example", "version": "1.0.0", "api": 1, "type": "script",
      "requires": { "net.uzume.example-core": "^1.0" }, "provides": ["example.team"],
      "games": [{ "system": "nds", "codes": ["ABC*"] }],
      "permissions": ["memory.read", "rom.read", "ui", "run.state", "video.capture"], "entry": "main.lua",
      "assets": ["icons/", "i18n/"], "settings": { "a": 1 } })");
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.manifest.id, QString("net.uzume.example"));
    QCOMPARE(r.manifest.dependencies.size(), 1);
    QCOMPARE(r.manifest.provides, QStringList{"example.team"});
    QCOMPARE(r.manifest.permissions.size(), 5);
    QVERIFY(r.manifest.hasPermission("video.capture"));
    QVERIFY(r.manifest.matchesGame("nds", "ABCD"));
    QVERIFY(r.manifest.matchesGame("NDS", "abce"));
    QVERIFY(!r.manifest.matchesGame("nds", "XBCD"));
    QVERIFY(!r.manifest.matchesGame("gba", "ABCD"));
    QCOMPARE(r.manifest.settings.value("a").toInt(), 1);
  }
  void defaultsAllGames() {
    const ManifestResult r = parseManifest(base());
    QVERIFY(r.ok);
    QVERIFY(r.manifest.matchesGame("3ds", "anything"));
    QCOMPARE(r.manifest.type, QString("script"));
  }
  void apiVersion() {
    QCOMPARE(errOf(R"({"api":2})"), QString("Tracker needs plugin API 2; this app supports API 1"));
    QVERIFY(errOf(R"({"api":null})").contains("'api' is required"));
  }
  void typeTool() {
    QCOMPARE(errOf(R"({"type":"tool"})"), QString("tool plugins are not supported yet"));
    QVERIFY(errOf(R"({"type":"weird"})").contains("unknown plugin type"));
  }
  void permissions() {
    QVERIFY(errOf(R"({"permissions":["memory.write","cheats","store"]})").isEmpty());
    QCOMPARE(errOf(R"({"permissions":["tool.exec"]})"), QString("permission tool.exec is not available yet"));
    QVERIFY(errOf(R"({"permissions":["network"]})").contains("unknown permission 'network'"));
  }
  void ids() {
    QVERIFY(errOf(R"({"id":"Tracker"})").contains("invalid plugin id"));
    QVERIFY(errOf(R"({"id":"tracker"})").contains("invalid plugin id"));
    QVERIFY(errOf(R"({"id":"net..x"})").contains("invalid plugin id"));
    QVERIFY(errOf(R"({"id":"net.example.a_b"})").contains("invalid plugin id"));
    QVERIFY(errOf(R"({"id":null})").contains("'id' is required"));
  }
  void versionAndName() {
    QVERIFY(errOf(R"({"version":"1.0"})").contains("invalid version"));
    QVERIFY(errOf(R"({"name":""})").contains("'name'"));
  }
  void entryRules() {
    QVERIFY(errOf(R"({"entry":"../x.lua"})").contains("invalid entry"));
    QVERIFY(errOf(R"({"entry":"/etc/x.lua"})").contains("invalid entry"));
    QVERIFY(errOf(R"({"entry":"main.txt"})").contains("invalid entry"));
    QVERIFY(errOf(R"({"entry":null})").contains("'entry' is required"));
    QTemporaryDir t;
    testutil::PluginSpec s;
    const QString dir = testutil::writePlugin(t.path(), s);
    QVERIFY(loadManifest(dir).ok);
    QFile::remove(dir + "/main.lua");
    QVERIFY(loadManifest(dir).error.contains("does not exist"));
    QVERIFY(!loadManifest(t.path() + "/nothing").ok);
  }
  void badJson() {
    QVERIFY(!parseManifest("{nope").ok);
    QVERIFY(!parseManifest("[]").ok);
  }
  void ranges() {
    auto m = [](const char* r, const char* v) {
      const auto range = VersionRange::parse(r);
      return range && range->matches(*SemVer::parse(v));
    };
    QVERIFY(m("^1.2", "1.2.0")); QVERIFY(m("^1.2", "1.9.9")); QVERIFY(!m("^1.2", "2.0.0")); QVERIFY(!m("^1.2", "1.1.9"));
    QVERIFY(m("^0.2", "0.2.7")); QVERIFY(!m("^0.2", "0.3.0"));
    QVERIFY(m("~1.2", "1.2.9")); QVERIFY(!m("~1.2", "1.3.0")); QVERIFY(m("~1", "1.9.0")); QVERIFY(!m("~1", "2.0.0"));
    QVERIFY(m(">=1.0", "1.0.0")); QVERIFY(m(">=1.0", "7.0.0")); QVERIFY(!m(">=1.0", "0.9.0"));
    QVERIFY(m("1.2.3", "1.2.3")); QVERIFY(!m("1.2.3", "1.2.4"));
    QVERIFY(m("*", "9.9.9"));
    QVERIFY(!m("^2.0", "2.0.0-beta.1"));
    QVERIFY(!VersionRange::parse("nonsense")); QVERIFY(!VersionRange::parse("^a.b")); QVERIFY(!VersionRange::parse(">=")); QVERIFY(!VersionRange::parse("1.2.3.4"));
    QVERIFY(errOf(R"({"requires":{"net.x.y":"huh"}})").contains("invalid version range"));
    QVERIFY(errOf(R"({"requires":{"net.example.tracker":"*"}})").contains("cannot require itself"));
  }
  void minPlayer() {
    // Optional: the oldest Uzume that runs the plugin (a plugin that needs a newer one is turned off).
    QVERIFY(errOf(R"({"min_player":"0.16.4"})").isEmpty());
    QVERIFY(errOf(R"({"min_player":"0.16.4-beta.2"})").isEmpty());
    QVERIFY(errOf(R"({"min_player":"banana"})").contains("invalid min_player"));
    QVERIFY(errOf(R"({"min_player":7})").contains("'min_player' must be a string"));
    QVERIFY(errOf(R"({"min_player":null})").isEmpty());
  }
  void globs() {
    QVERIFY(globMatch("AB?D", "ABCD")); QVERIFY(!globMatch("AB?D", "ABD")); QVERIFY(globMatch("*", "")); QVERIFY(globMatch("A*D*", "ABCDE"));
    QVERIFY(!globMatch("A*D", "ABCDE")); QVERIFY(globMatch("a*", "ABC"));
    QVERIFY(errOf(R"({"games":[{"codes":["A"]}]})").contains("'system'"));
  }
  static QString rs(const QString& arr) { return QStringLiteral(R"({"run_settings":%1})").arg(arr); }
  void runSettingsValid() {
    const ManifestResult r = parseManifest(base(rs(R"([
      {"key":"mode","type":"enum","label":"rules.mode","hint":"rules.mode.hint","default":"soullink",
       "options":[{"value":"soullink","label":"rules.mode.soullink"},{"value":"nuzlocke","label":"rules.mode.nuzlocke"}],"extra":1},
      {"key":"rules.dupes_clause","type":"boolean","label":"rules.dupes","default":true}])")));
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.manifest.runSettings.size(), 2);
    QCOMPARE(r.manifest.runSettings.at(0).options.size(), 2);
    QCOMPARE(r.manifest.runSettings.at(0).def.toString(), QString("soullink"));
    QCOMPARE(r.manifest.runSettings.at(1).def.toBool(), true);
    QVERIFY(parseManifest(base()).manifest.runSettings.isEmpty());
  }
  void runSettingsInvalid() {
    const QString b = R"({"key":"a","type":"boolean","label":"l","default":false})";
    const QString en = R"({"key":"m","type":"enum","label":"l","default":"x","options":[{"value":"x","label":"l1"},{"value":"y","label":"l2"}]})";
    QVERIFY(errOf(rs("[" + b + "]")).isEmpty());
    QVERIFY(errOf(rs("[" + en + "]")).isEmpty());
    QVERIFY(errOf(rs("[]")).contains("1 to 32"));
    QVERIFY(errOf(rs("{}")).contains("array"));
    QStringList many;
    for (int i = 0; i < 33; ++i) many << QStringLiteral(R"({"key":"k%1","type":"boolean","label":"l","default":true})").arg(i);
    QVERIFY(errOf(rs("[" + many.join(",") + "]")).contains("1 to 32"));
    QVERIFY(errOf(rs("[1]")).contains("must be an object"));
    QVERIFY(errOf(rs(R"([{"type":"boolean","label":"l","default":true}])")).contains("'key' is required"));
    for (const QString& k : {QString("A"), QString("a.b.c"), QString("1a"), QString("a-b"), QString("a..b"), QString(), QString("a."), QString(33, QLatin1Char('a'))})
      QVERIFY2(errOf(rs(QStringLiteral(R"([{"key":"%1","type":"boolean","label":"l","default":true}])").arg(k))).contains("key"), qPrintable(k));
    QVERIFY(errOf(rs("[" + b + "," + b + "]")).contains("duplicate key"));
    QVERIFY(errOf(rs(R"([{"key":"rules","type":"boolean","label":"l","default":true},{"key":"rules.x","type":"boolean","label":"l","default":true}])")).contains("prefix"));
    QVERIFY(errOf(rs(R"([{"key":"a","type":"number","label":"l","default":1}])")).contains("type must be"));
    QVERIFY(errOf(rs(R"([{"key":"a","type":"boolean","default":true}])")).contains("'label'"));
    QVERIFY(errOf(rs(R"([{"key":"a","type":"boolean","label":"bad key!","default":true}])")).contains("'label'"));
    QVERIFY(errOf(rs(R"([{"key":"a","type":"boolean","label":"l","hint":"bad hint","default":true}])")).contains("'hint'"));
    QVERIFY(errOf(rs(R"([{"key":"a","type":"boolean","label":"l"}])")).contains("'default'"));
    QVERIFY(errOf(rs(R"([{"key":"a","type":"boolean","label":"l","default":"yes"}])")).contains("'default'"));
    QVERIFY(errOf(rs(R"([{"key":"a","type":"boolean","label":"l","default":true,"options":[]}])")).contains("only allowed for enum"));
    QVERIFY(errOf(rs(R"([{"key":"m","type":"enum","label":"l","default":"x"}])")).contains("2 to 8 options"));
    QVERIFY(errOf(rs(R"([{"key":"m","type":"enum","label":"l","default":"x","options":[{"value":"x","label":"l"}]}])")).contains("2 to 8 options"));
    QStringList nine;
    for (int i = 0; i < 9; ++i) nine << QStringLiteral(R"({"value":"v%1","label":"l"})").arg(i);
    QVERIFY(errOf(rs(R"([{"key":"m","type":"enum","label":"l","default":"v0","options":[)" + nine.join(",") + "]}]")).contains("2 to 8 options"));
    QVERIFY(errOf(rs(R"([{"key":"m","type":"enum","label":"l","default":"x","options":[{"value":"X","label":"l"},{"value":"y","label":"l"}]}])")).contains("option 'value'"));
    QVERIFY(errOf(rs(R"([{"key":"m","type":"enum","label":"l","default":"x","options":[{"value":"x","label":"l"},{"value":"x","label":"l"}]}])")).contains("duplicate option"));
    QVERIFY(errOf(rs(R"([{"key":"m","type":"enum","label":"l","default":"x","options":[{"value":"x"},{"value":"y","label":"l"}]}])")).contains("option 'label'"));
    QVERIFY(errOf(rs(R"([{"key":"m","type":"enum","label":"l","default":"z","options":[{"value":"x","label":"l"},{"value":"y","label":"l"}]}])")).contains("one of the option values"));
  }
  void runSettingsBuildAndHubInfo() {
    QList<uzume::RunSetting> list;
    QVERIFY(uzume::parseRunSettings(QJsonDocument::fromJson(R"([
      {"key":"mode","type":"enum","label":"m","default":"soullink","options":[{"value":"soullink","label":"a"},{"value":"nuzlocke","label":"b"}]},
      {"key":"rules.dupes_clause","type":"boolean","label":"d","default":true},
      {"key":"rules.shiny","type":"boolean","label":"s","default":false}])").array(), list).isEmpty());
    QCOMPARE(QJsonDocument(uzume::buildRunSettings(list, {})).toJson(QJsonDocument::Compact),
             QByteArray(R"({"mode":"soullink","rules":{"dupes_clause":true,"shiny":false}})"));
    QCOMPARE(QJsonDocument(uzume::buildRunSettings(list, {{"mode", "nuzlocke"}, {"rules.shiny", true}, {"rules.dupes_clause", false}})).toJson(QJsonDocument::Compact),
             QByteArray(R"({"mode":"nuzlocke","rules":{"dupes_clause":false,"shiny":true}})"));
    const QJsonObject strings = QJsonDocument::fromJson(R"({"en":{"a":"A en","b":"B en"},"de":{"a":"A de"}})").object();
    QCOMPARE(uzume::runSettingText(strings, "de", "a"), QString("A de"));
    QCOMPARE(uzume::runSettingText(strings, "de", "b"), QString("B en"));
    QCOMPARE(uzume::runSettingText(strings, "de", "zz"), QString("zz"));

    QJsonObject hub{{"plugin_id", "net.test.x"}, {"version", "1.0.0"}, {"sha256", QString(64, 'a')}, {"origin", "official"},
                    {"run_settings", QJsonDocument::fromJson(R"([{"key":"a","type":"boolean","label":"l","default":true}])").array()},
                    {"run_settings_strings", QJsonDocument::fromJson(R"({"en":{"l":"Label"}})").object()}};
    auto info = uzume::parseHubPluginInfo(hub);
    QVERIFY(info);
    QCOMPARE(info->runSettings.size(), 1);
    QCOMPARE(info->runSettingsStrings.value("en").toObject().value("l").toString(), QString("Label"));
    hub.insert("run_settings", QJsonDocument::fromJson(R"([{"key":"A","type":"boolean","label":"l","default":true}])").array());
    info = uzume::parseHubPluginInfo(hub);  // invalid run_settings: the plugin stays, without rules
    QVERIFY(info);
    QVERIFY(info->runSettings.isEmpty());
    hub.remove("run_settings");
    QVERIFY(uzume::parseHubPluginInfo(hub)->runSettings.isEmpty());
  }
  // The shared corpus of the Hub (server/internal/pluginpkg/testdata/manifests): valid-* must parse here, invalid-* must not.
  void sharedCorpusAgreesWithTheHub() {
    const QDir dir(QStringLiteral(FB_MANIFEST_CORPUS));
    QVERIFY2(dir.exists(), "manifest corpus directory missing");
    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    QVERIFY(files.size() >= 20);
    int valid = 0, invalid = 0;
    for (const QString& name : files) {
      QFile f(dir.filePath(name));
      QVERIFY(f.open(QIODevice::ReadOnly));
      const ManifestResult r = parseManifest(f.readAll());
      if (name.startsWith(QLatin1String("valid-"))) {
        QVERIFY2(r.ok, qPrintable(name + ": " + r.error));
        ++valid;
      } else if (name.startsWith(QLatin1String("invalid-"))) {
        QVERIFY2(!r.ok, qPrintable(name + " must be rejected"));
        ++invalid;
      } else {
        QFAIL(qPrintable("unexpected corpus file " + name));
      }
    }
    QVERIFY(valid >= 5 && invalid >= 20);
  }
  void providesAndAssets() {
    QVERIFY(errOf(R"({"provides":"x"})").contains("array"));
    QVERIFY(errOf(R"({"assets":["../x"]})").contains("invalid asset path"));
  }
};

QTEST_APPLESS_MAIN(ManifestTest)
#include "manifest_test.moc"
