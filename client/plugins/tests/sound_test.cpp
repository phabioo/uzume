// uz.sound (0.23, ADR 0033 D6) against a fake sound sink: permission, path rules, WAV validation (format, 10 s, 2 MiB), volume,
// the 8 voices per plugin and stop_all. WAV files are generated here (a few samples), never real audio.
#include <QTemporaryDir>
#include <QtTest>
#include <mutex>

#include "plugin_host.h"
#include "plugin_test_util.h"

using namespace uzume::plugins;
using namespace uzume::plugins::testutil;

namespace {

QByteArray le(quint32 v, int bytes) {
  QByteArray b;
  for (int i = 0; i < bytes; ++i) b.append(static_cast<char>((v >> (8 * i)) & 0xff));
  return b;
}

// 16-bit PCM WAV with `frames` frames of a ramp.
QByteArray wav(int rate, int channels, int frames, int bits = 16, int format = 1) {
  const int dataLen = frames * channels * (bits / 8);
  QByteArray d;
  d.append("RIFF").append(le(36 + dataLen, 4)).append("WAVE");
  d.append("fmt ").append(le(16, 4)).append(le(format, 2)).append(le(channels, 2)).append(le(rate, 4));
  d.append(le(rate * channels * (bits / 8), 4)).append(le(channels * (bits / 8), 2)).append(le(bits, 2));
  d.append("data").append(le(dataLen, 4));
  QByteArray body(dataLen, '\0');
  for (int i = 0; i + 1 < dataLen; i += 2) {
    body[i] = static_cast<char>(i / 2 % 100);
    body[i + 1] = 0;
  }
  return d.append(body);
}

class FakeSink : public SoundSink {
 public:
  struct Play {
    QString plugin;
    int rate;
    qint64 frames;
    double volume;
  };
  quint64 play(const QString& plugin, std::shared_ptr<const SoundClip> clip, double volume) override {
    std::lock_guard<std::mutex> l(m);
    plays.append({plugin, clip->sampleRate, clip->frames(), volume});
    ++voices;
    return plays.size();
  }
  void stopAll(const QString&) override {
    std::lock_guard<std::mutex> l(m);
    voices = 0;
    ++stops;
  }
  int activeVoices(const QString&) const override {
    std::lock_guard<std::mutex> l(m);
    return voices;
  }
  mutable std::mutex m;
  QList<Play> plays;
  int voices = 0;
  int stops = 0;
};

bool hasLog(const ConsoleSnapshot& c, const QString& needle) {
  for (const LogLine& l : c.log)
    if (l.text.contains(needle)) return true;
  return false;
}

}  // namespace

class SoundApiTest : public QObject {
  Q_OBJECT

  QTemporaryDir m_dir;
  std::shared_ptr<FakeSink> m_sink;
  PluginHost* m_host = nullptr;
  GameContext m_ctx{"gba", "ABCD", "abc123", "Test Game", "", "en"};

  void startWith(const QStringList& perms, const QString& lua, const QMap<QString, QByteArray>& files = {}) {
    PluginSpec s;
    s.permissions = perms;
    s.lua = lua;
    s.name = "Test Plugin";
    s.files = files;
    m_host->start(m_ctx, nullptr, {infoFor(writePlugin(m_dir.path() + "/src", s), m_dir.path() + "/cache")});
    QTRY_COMPARE_WITH_TIMEOUT(m_host->consoleSnapshot("net.test.plugin").status, PluginStatus::Running, 10000);
  }
  ConsoleSnapshot con() const { return m_host->consoleSnapshot("net.test.plugin"); }

 private slots:
  void init() {
    m_sink = std::make_shared<FakeSink>();
    m_ctx.sound = m_sink;
    m_host = new PluginHost;
    m_host->setTickHz(200);
  }
  void cleanup() {
    delete m_host;
    m_host = nullptr;
    QDir(m_dir.path() + "/src").removeRecursively();
  }

  void permissionIsRequired() {
    startWith({}, R"LUA(
      for _, f in ipairs({ function() return uz.sound.play("a.wav") end, function() return uz.sound.stop_all() end }) do
        local ok, e = pcall(f)
        assert(not ok and e:find("permission sound not granted", 1, true), tostring(e))
      end
      uz.log.info("denied ok")
    )LUA");
    QVERIFY2(hasLog(con(), "denied ok"), qPrintable(con().error));
    QVERIFY(m_sink->plays.isEmpty());
  }

  void playsAMonoClipIntoTheBus() {
    startWith({"sound"}, R"LUA(
      local h = uz.sound.play("sounds/ding.wav", { volume = 0.5 })
      assert(math.type(h) == "integer" and h > 0, tostring(h))
      assert(uz.sound.play("sounds/ding.wav") ~= nil)
      uz.log.info("play ok")
    )LUA", {{"sounds/ding.wav", wav(22050, 1, 441)}});
    QVERIFY2(hasLog(con(), "play ok"), qPrintable(con().error));
    QCOMPARE(m_sink->plays.size(), 2);
    QCOMPARE(m_sink->plays[0].plugin, QString("net.test.plugin"));
    QCOMPARE(m_sink->plays[0].rate, 22050);
    QCOMPARE(m_sink->plays[0].frames, qint64(441));  // mono became stereo frames
    QCOMPARE(m_sink->plays[0].volume, 0.5);
    QCOMPARE(m_sink->plays[1].volume, 1.0);
  }

  void badPathsAndFilesAreRefusedWithoutAnError() {
    startWith({"sound"}, R"LUA(
      local function bad(code, ...)
        local h, msg = uz.sound.play(...)
        assert(h == nil and msg == code, tostring(code) .. " got " .. tostring(msg))
      end
      bad("invalid_path", "../outside.wav")
      bad("invalid_path", "/etc/passwd")
      bad("invalid_path", "sounds\\ding.wav")
      bad("not_found", "missing.wav")
      bad("unsupported_format", "junk.wav")
      bad("unsupported_format", "eight.wav")
      bad("too_long", "long.wav")
      bad("too_large", "huge.wav")
      uz.log.info("refusals ok")
    )LUA", {{"junk.wav", QByteArray("not a wave file at all")},
           {"eight.wav", wav(8000, 1, 100, 8)},
           {"long.wav", wav(8000, 1, 8000 * 10 + 1)},                       // 10.000125 s
           {"huge.wav", wav(48000, 2, 12000) + QByteArray(2 * 1024 * 1024, '\0')},
           {"sounds/ding.wav", wav(8000, 1, 10)}});
    QVERIFY2(hasLog(con(), "refusals ok"), qPrintable(con().error));
    QVERIFY(m_sink->plays.isEmpty());
  }

  void tenSecondsExactlyIsAccepted() {
    startWith({"sound"}, R"LUA(
      assert(uz.sound.play("ten.wav"))
      uz.log.info("ten ok")
    )LUA", {{"ten.wav", wav(8000, 1, 8000 * 10)}});
    QVERIFY2(hasLog(con(), "ten ok"), qPrintable(con().error));
    QCOMPARE(m_sink->plays.size(), 1);
  }

  void volumeMustBeBetweenZeroAndOne() {
    startWith({"sound"}, R"LUA(
      assert(not pcall(uz.sound.play, "a.wav", { volume = 1.5 }))
      assert(not pcall(uz.sound.play, "a.wav", { volume = -0.1 }))
      assert(not pcall(uz.sound.play, "a.wav", { volume = "loud" }))
      assert(uz.sound.play("a.wav", { volume = 0 }))
      uz.log.info("volume ok")
    )LUA", {{"a.wav", wav(8000, 2, 10)}});
    QVERIFY2(hasLog(con(), "volume ok"), qPrintable(con().error));
    QCOMPARE(m_sink->plays.size(), 1);
    QCOMPARE(m_sink->plays[0].volume, 0.0);
  }

  void eightVoicesPerPluginAndStopAll() {
    startWith({"sound"}, R"LUA(
      for i = 1, 8 do assert(uz.sound.play("a.wav")) end
      local h, msg = uz.sound.play("a.wav")
      assert(h == nil and msg == "too_many_voices", tostring(msg))
      assert(uz.sound.stop_all() == true)
      assert(uz.sound.play("a.wav"))
      uz.log.info("voices ok")
    )LUA", {{"a.wav", wav(8000, 1, 10)}});
    QVERIFY2(hasLog(con(), "voices ok"), qPrintable(con().error));
    QCOMPARE(m_sink->plays.size(), 9);
    QVERIFY(m_sink->stops >= 1);
  }

  void withoutASinkPlayIsUnavailable() {
    m_ctx.sound.reset();
    startWith({"sound"}, R"LUA(
      local h, msg = uz.sound.play("a.wav")
      assert(h == nil and msg == "unavailable", tostring(msg))
      assert(uz.sound.stop_all() == true)
      uz.log.info("none ok")
    )LUA", {{"a.wav", wav(8000, 1, 10)}});
    QVERIFY2(hasLog(con(), "none ok"), qPrintable(con().error));
  }

  void decodeWavDirectly() {
    QString err;
    auto c = decodeWav(wav(44100, 2, 100), &err);
    QVERIFY(c);
    QCOMPARE(c->frames(), qint64(100));
    QVERIFY(!decodeWav(wav(44100, 3, 100), &err));
    QVERIFY(!decodeWav(wav(44100, 1, 100, 16, 3), &err));  // float format
    QVERIFY(!decodeWav(wav(1000, 1, 100), &err));          // rate out of range
  }
};

QTEST_MAIN(SoundApiTest)
#include "sound_test.moc"
