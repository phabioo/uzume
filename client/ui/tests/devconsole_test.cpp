// DevConsole (0.22, boards 4d-1 and 4d-2): the model shared by the QML console and the debug MCP tools. Fake sources (no Lua, no
// game): log filters and counts, Pause log, Clear, the one-time reload and pause signals, permissions, unpacked listing and the
// memory viewer over a synthetic memory map. No QML.
#include <QSignalSpy>
#include <QtTest>

#include "devconsole.h"

using namespace uzume::ui;
using namespace uzume::plugins;
namespace emu = uzume::emu;

namespace {

LogLine line(qint64 ms, LogLevel level, const QString& text, qint64 seq) {
  LogLine l;
  l.timeMs = ms;
  l.level = level;
  l.text = text;
  l.seq = seq;
  return l;
}

// A MemoryMap over one buffer, read through the same pure functions the MCP tools use.
class FakeMemory : public MemorySource {
 public:
  FakeMemory() : bytes_(0x400, 0), map_(emu::MemoryMap::fromBuffer(bytes_.data(), bytes_.size(), 0x08000000, "heap")) {
    for (int i = 0; i < bytes_.size(); ++i) bytes_[i] = static_cast<char>(i & 0xFF);
    const QByteArray text = "HELLO WORLD";
    std::memcpy(bytes_.data() + 0x120, text.constData(), static_cast<size_t>(text.size()));
    bytes_[0x200] = 0x2A;
    bytes_[0x300] = 0x2A;
  }
  bool available() const override { return avail; }
  std::vector<emu::MemoryMap::RegionInfo> regions() const override { return map_.regions(); }
  bool read(quint64 addr, quint64 len, QByteArray* out, QString*) override {
    ++reads;
    *out = emu::memaccess::read(map_, addr, len);
    return true;
  }
  bool search(quint64 start, quint64 end, const QByteArray& pattern, int max, emu::memaccess::SearchResult* out, QString*) override {
    *out = emu::memaccess::search(map_, start, end, pattern, max);
    return true;
  }
  bool avail = true;
  int reads = 0;

 private:
  QByteArray bytes_;
  emu::MemoryMap map_;
};

PluginInfo infoOf(const QString& id, const QString& name, const QStringList& perms, bool unpacked) {
  PluginInfo p;
  p.manifest.id = id;
  p.manifest.name = name;
  p.manifest.permissions = perms;
  p.devLinked = unpacked;
  p.path = QStringLiteral("/dev/%1").arg(name);
  return p;
}

}  // namespace

class DevConsoleTest : public QObject {
  Q_OBJECT

  QStringList active_{QStringLiteral("net.test.a")};
  QMap<QString, ConsoleSnapshot> snaps_;
  QMap<QString, PluginInfo> infos_;
  std::unique_ptr<DevConsole> dc_;
  std::vector<QString> reloads_;

  void make() {
    DevConsole::Sources s;
    s.developerMode = [] { return true; };
    s.activeIds = [this] { return active_; };
    s.info = [this](const QString& id) -> std::optional<PluginInfo> {
      if (infos_.contains(id)) return infos_.value(id);
      return std::nullopt;
    };
    s.snapshot = [this](const QString& id) { return snaps_.value(id); };
    s.unpacked = [this] {
      QList<PluginInfo> out;
      for (const PluginInfo& p : infos_)
        if (p.devLinked) out.append(p);
      return out;
    };
    s.permissionLabel = [](const QString& p) { return QStringLiteral("label:") + p; };
    s.reload = [this](const QString& id) { reloads_.push_back(id); };
    dc_ = std::make_unique<DevConsole>(std::move(s));
  }

 private slots:
  void init() {
    snaps_.clear();
    infos_.clear();
    reloads_.clear();
    active_ = {QStringLiteral("net.test.a")};
    ConsoleSnapshot a;
    a.status = PluginStatus::Running;
    a.log = {line(1000, LogLevel::Info, "loaded", 1), line(2000, LogLevel::Warn, "careful", 2), line(3000, LogLevel::Error, "bad one", 3),
             line(4000, LogLevel::Debug, "detail", 4), line(5000, LogLevel::Error, "bad two", 5)};
    a.lastSeq = 5;
    snaps_.insert(QStringLiteral("net.test.a"), a);
    infos_.insert(QStringLiteral("net.test.a"), infoOf("net.test.a", "Item Helper", {"memory.read", "ui"}, true));
    make();
  }

  void buildLogViewFiltersCountsAndOrder() {
    const QList<ConsoleLogEntry> e = DevConsole::entriesOf(snaps_.value("net.test.a").log);
    ConsoleLogView all = DevConsole::buildLogView(e, {});
    QCOMPARE(all.lines.size(), 5);
    QCOMPARE(all.lines.first().seq, qint64(5));  // newest first
    QCOMPARE(all.error, 2);
    QCOMPARE(all.warn, 1);
    QCOMPARE(all.info, 1);
    QCOMPARE(all.debug, 1);
    QCOMPARE(all.total, 5);
    QCOMPARE(all.lastSeq, qint64(5));

    ConsoleLogFilter f;
    f.levels = {QStringLiteral("error")};
    ConsoleLogView errs = DevConsole::buildLogView(e, f);
    QCOMPARE(errs.lines.size(), 2);
    QCOMPARE(errs.warn, 1);  // the chips keep their counts while another level is off
    f = {};
    f.sinceSeq = 3;
    QCOMPARE(DevConsole::buildLogView(e, f).lines.size(), 2);  // seq 4 and 5
    f = {};
    f.limit = 2;
    QCOMPARE(DevConsole::buildLogView(e, f).lines.size(), 2);
    f = {};
    f.maxSeq = 3;  // a paused log
    const ConsoleLogView paused = DevConsole::buildLogView(e, f);
    QCOMPARE(paused.lines.size(), 3);
    QCOMPARE(paused.hiddenNew, 2);
    f = {};
    f.clearedBeforeMs = 3000;
    QCOMPARE(DevConsole::buildLogView(e, f).lines.size(), 2);
  }

  void modelHasChipsCountsAndFilters() {
    QVariantMap m = dc_->buildModel();
    QCOMPARE(m.value("selected").toString(), QStringLiteral("net.test.a"));
    QCOMPARE(m.value("counts").toMap().value("error").toInt(), 2);
    QCOMPARE(m.value("log").toList().size(), 5);
    QCOMPARE(m.value("log").toList().first().toMap().value("text").toString(), QStringLiteral("bad two"));
    dc_->setLevel("error", false);
    dc_->setLevel("debug", false);
    m = dc_->buildModel();
    QCOMPARE(m.value("log").toList().size(), 2);  // error and debug are off: loaded, careful
    QCOMPARE(m.value("counts").toMap().value("error").toInt(), 2);  // the count stays
    QVERIFY(!m.value("filters").toMap().value("error").toBool());
    // The last chip cannot be turned off: an empty table would only be confusing.
    dc_->setLevel("info", false);
    dc_->setLevel("warn", false);
    m = dc_->buildModel();
    QVERIFY(m.value("filters").toMap().value("warn").toBool());
  }

  void pauseLogFreezesAndCountsNewLines() {
    dc_->setLogPaused(true);
    ConsoleSnapshot a = snaps_.value("net.test.a");
    a.log.append(line(6000, LogLevel::Info, "later", 6));
    a.lastSeq = 6;
    snaps_["net.test.a"] = a;
    QVariantMap m = dc_->buildModel();
    QCOMPARE(m.value("log").toList().size(), 5);
    QCOMPARE(m.value("logPending").toInt(), 1);
    QVERIFY(m.value("logPaused").toBool());
    dc_->setLogPaused(false);
    m = dc_->buildModel();
    QCOMPARE(m.value("log").toList().size(), 6);
    QCOMPARE(m.value("logPending").toInt(), 0);
  }

  void clearHidesOldLinesButNotTheMcpView() {
    dc_->clearLog("net.test.a");
    QCOMPARE(dc_->buildModel().value("log").toList().size(), 0);
    QCOMPARE(dc_->logView("net.test.a").lines.size(), 5);  // the MCP tools see the whole ring
    QVERIFY(dc_->logText("net.test.a").isEmpty());
  }

  void pausedErrorAndPermissions() {
    ConsoleSnapshot a = snaps_.value("net.test.a");
    a.status = PluginStatus::Paused;
    a.error = QStringLiteral("main.lua:41: boom\nstack traceback:\n\tx");
    a.notGranted = {QStringLiteral("memory.write")};
    snaps_["net.test.a"] = a;
    const QVariantMap m = dc_->buildModel();
    QVERIFY(m.value("paused").toBool());
    QCOMPARE(m.value("status").toString(), QStringLiteral("error"));
    QCOMPARE(m.value("errorFirst").toString(), QStringLiteral("main.lua:41: boom"));
    QCOMPARE(m.value("errorLine").toString(), QStringLiteral("Paused after an error. Your game keeps running."));
    const QVariantMap perms = m.value("permissions").toMap();
    QCOMPARE(perms.value("granted").toList().size(), 2);
    QCOMPARE(perms.value("granted").toList().first().toMap().value("label").toString(), QStringLiteral("label:memory.read"));
    QCOMPARE(perms.value("notGranted").toList().first().toMap().value("id").toString(), QStringLiteral("memory.write"));
    QVERIFY(m.value("unpacked").toBool());
    QCOMPARE(m.value("folder").toString(), QStringLiteral("/dev/Item Helper"));
  }

  void unpackedPluginThatDoesNotRunIsListed() {
    infos_.insert(QStringLiteral("net.test.b"), infoOf("net.test.b", "Other", {}, true));
    const QVariantMap m = dc_->buildModel();
    const QVariantList list = m.value("plugins").toList();
    QCOMPARE(list.size(), 2);
    QVERIFY(!list.at(1).toMap().value("running").toBool());
    QCOMPARE(list.at(1).toMap().value("statusText").toString(), QStringLiteral("Not running"));
    dc_->selectPlugin("net.test.b");
    QVERIFY(!dc_->buildModel().value("running").toBool());
  }

  void pauseSignalFiresOncePerPause() {
    QSignalSpy paused(dc_.get(), &DevConsole::pausedAfterError);
    ConsoleSnapshot a = snaps_.value("net.test.a");
    a.status = PluginStatus::Paused;
    a.error = QStringLiteral("boom");
    snaps_["net.test.a"] = a;
    dc_->noteChanged("net.test.a");
    dc_->noteChanged("net.test.a");
    QCOMPARE(paused.size(), 1);
    QCOMPARE(paused.at(0).at(0).toString(), QStringLiteral("net.test.a"));
    QCOMPARE(paused.at(0).at(1).toString(), QStringLiteral("Item Helper"));
    a.status = PluginStatus::Running;
    a.error.clear();
    snaps_["net.test.a"] = a;
    dc_->noteChanged("net.test.a");
    a.status = PluginStatus::Paused;
    a.error = QStringLiteral("boom again");
    snaps_["net.test.a"] = a;
    dc_->noteChanged("net.test.a");
    QCOMPARE(paused.size(), 2);
  }

  void reloadSignalCarriesMilliseconds() {
    QSignalSpy reloaded(dc_.get(), &DevConsole::reloaded);
    ConsoleSnapshot a = snaps_.value("net.test.a");
    a.reloadCount = 1;
    a.reloadMs = 38;
    snaps_["net.test.a"] = a;
    dc_->noteChanged("net.test.a");
    dc_->noteChanged("net.test.a");
    QCOMPARE(reloaded.size(), 1);
    QCOMPARE(reloaded.at(0).at(1).toString(), QStringLiteral("Item Helper"));
    QCOMPARE(reloaded.at(0).at(2).toLongLong(), qint64(38));
    QCOMPARE(dc_->buildModel().value("reload").toMap().value("text").toString(), QStringLiteral("Reloaded · 38 ms"));
    // A reload that ended paused (a new error in the file) is not announced as a reload.
    a.reloadCount = 2;
    a.status = PluginStatus::Paused;
    a.error = QStringLiteral("syntax");
    snaps_["net.test.a"] = a;
    dc_->noteChanged("net.test.a");
    QCOMPARE(reloaded.size(), 1);
  }

  void reloadSelectedCallsTheSource() {
    dc_->reloadSelected();
    QCOMPARE(reloads_.size(), size_t(1));
    QCOMPARE(reloads_[0], QStringLiteral("net.test.a"));
  }

  void memoryViewerRegionsRowsAndGoto() {
    auto mem = std::make_shared<FakeMemory>();
    dc_->setMemorySource(mem);
    dc_->memoryRefresh();
    QVariantMap v = dc_->memory();
    QVERIFY(v.value("available").toBool());
    QCOMPARE(v.value("regions").toList().size(), 1);
    QCOMPARE(v.value("baseText").toString(), QStringLiteral("0x08000000"));
    QCOMPARE(v.value("rows").toList().size(), 16);
    QVariantMap row0 = v.value("rows").toList().at(0).toMap();
    QCOMPARE(row0.value("addr").toString(), QStringLiteral("0x08000000"));
    QCOMPARE(row0.value("hex").toString(), QStringLiteral("00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F"));
    QCOMPARE(row0.value("ascii").toString(), QStringLiteral("................"));

    dc_->memoryGoto("0x08000125");  // aligned down to the row
    v = dc_->memory();
    QCOMPARE(v.value("baseText").toString(), QStringLiteral("0x08000120"));
    row0 = v.value("rows").toList().at(0).toMap();
    QCOMPARE(row0.value("ascii").toString(), QStringLiteral("HELLO WORLD+,-./"));
    QVERIFY(row0.value("hex").toString().startsWith(QStringLiteral("48 45 4C 4C 4F")));

    dc_->memoryScroll(1);
    QCOMPARE(dc_->memory().value("baseText").toString(), QStringLiteral("0x08000130"));
    dc_->memoryScroll(-200000000);  // never below zero
    QCOMPARE(dc_->memory().value("baseText").toString(), QStringLiteral("0x00000000"));
    QVERIFY(!dc_->memory().value("error").toString().isEmpty());  // nothing is mapped there

    dc_->memoryGoto("nonsense");
    QVERIFY(dc_->memory().value("error").toString().startsWith(QStringLiteral("Not an address")));
    QVERIFY(mem->reads > 0);
  }

  void memoryViewerSearch() {
    dc_->setMemorySource(std::make_shared<FakeMemory>());
    dc_->memoryRefresh();
    dc_->memorySearch("value", "42", "u8");  // 0x2A: found at 0x2A in the ramp (i & 0xFF == 42) and at 0x200 and 0x300
    QVariantMap s = dc_->memory().value("search").toMap();
    QVERIFY2(s.value("error").toString().isEmpty(), qPrintable(s.value("error").toString()));
    const QVariantList m = s.value("matches").toList();
    QVERIFY(m.contains(QStringLiteral("0x0800002A")));
    QVERIFY(m.contains(QStringLiteral("0x08000200")));

    dc_->memorySearch("bytes", "48 45 4C 4C 4F", "");
    s = dc_->memory().value("search").toMap();
    QCOMPARE(s.value("matches").toList().size(), 1);
    QCOMPARE(s.value("matches").toList().first().toString(), QStringLiteral("0x08000120"));

    dc_->memorySearch("bytes", "xyz", "");
    QVERIFY(!dc_->memory().value("search").toMap().value("error").toString().isEmpty());
    dc_->memorySearch("value", "abc", "u8");
    QVERIFY(!dc_->memory().value("search").toMap().value("error").toString().isEmpty());
    dc_->memoryClearSearch();
    QVERIFY(dc_->memory().value("search").toMap().isEmpty());
  }

  void memoryViewerWithoutGame() {
    auto mem = std::make_shared<FakeMemory>();
    mem->avail = false;
    dc_->setMemorySource(mem);
    dc_->memoryRefresh();
    QVERIFY(!dc_->memory().value("available").toBool());
    QVERIFY(dc_->memory().value("rows").toList().isEmpty());
  }

  void previewKeepsTheOldContract() {
    QVariantList log{QVariantMap{{"time", "18:42:09.870"}, {"level", "error"}, {"text", "e"}}, QVariantMap{{"time", "18:42:08.120"}, {"level", "warn"}, {"text", "w"}},
                     QVariantMap{{"time", "18:42:05.001"}, {"level", "debug"}, {"text", "d"}}};
    dc_->setPreview(QVariantMap{{"plugins", QVariantList()}, {"selected", "x"}, {"status", "error"}, {"log", log}});
    QVariantMap m = dc_->model();
    QCOMPARE(m.value("log").toList().size(), 3);
    QCOMPARE(m.value("log").toList().first().toMap().value("time").toString(), QStringLiteral("18:42:09.870"));
    QCOMPARE(m.value("counts").toMap().value("warn").toInt(), 1);
    dc_->setLevel("debug", false);
    QCOMPARE(dc_->model().value("log").toList().size(), 2);
    dc_->clearLog("x");
    QCOMPARE(dc_->model().value("log").toList().size(), 0);
    dc_->setPreview({});
    QVERIFY(!dc_->hasPreview());
  }
};

QTEST_GUILESS_MAIN(DevConsoleTest)
#include "devconsole_test.moc"
