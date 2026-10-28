// AudioMixer (0.23 audio stage 2) without an audio device: sums, soft clip, gains, mute, exclusive routing vs mix-all, ducking,
// the plugin generator bus, per-person gains and the short-read rule.
#include <QtTest>
#include <cmath>
#include <cstring>

#include "audiomixer.h"

using namespace uzume::ui;

namespace {
// n stereo frames of a constant value
QByteArray constant(qint16 v, int frames) {
  QByteArray b(frames * 4, 0);
  auto* s = reinterpret_cast<qint16*>(b.data());
  for (int i = 0; i < frames * 2; ++i) s[i] = v;
  return b;
}
}  // namespace

class AudioMixerTest : public QObject {
  Q_OBJECT

  static float level(AudioMixer& m, int frames = 16, int* produced = nullptr) {
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    const int n = m.mix(out.data(), frames);
    if (produced) *produced = n;
    return n > 0 ? out[0] : 0.0f;
  }

 private slots:
  void sumsInputsAndKeepsShortReads() {
    AudioMixer m;
    m.openHeadless(48000);
    m.setMixAll(true);
    const auto a = m.addInput(AudioMixer::Game);
    const auto b = m.addInput(AudioMixer::Tiles, "u1");
    m.push(a, constant(8192, 20));  // 0.25
    m.push(b, constant(4096, 8));   // 0.125, shorter
    std::vector<float> out(64, 0.0f);
    QCOMPARE(m.mix(out.data(), 16), 16);
    QVERIFY(std::fabs(out[0] - 0.375f) < 1e-4f);
    QVERIFY(std::fabs(out[2 * 12] - 0.25f) < 1e-4f);  // only the longer input is left
    int n = 0;
    level(m, 100, &n);
    QCOMPARE(n, 4);  // the 4 frames still queued, no padding (short read)
    level(m, 100, &n);
    QCOMPARE(n, 100);  // dry: silence for the whole request
  }

  void softClipLimitsTheSum() {
    AudioMixer m;
    m.openHeadless(48000);
    m.setMixAll(true);
    const auto a = m.addInput(AudioMixer::Game);
    const auto b = m.addInput(AudioMixer::Tiles);
    m.push(a, constant(32000, 8));
    m.push(b, constant(32000, 8));
    const float v = level(m, 8);
    QVERIFY(v > 0.95f && v <= 1.0f);
    QCOMPARE(AudioMixer::softClip(0.5f), 0.5f);
    QVERIFY(AudioMixer::softClip(-5.0f) >= -1.0f && AudioMixer::softClip(-5.0f) < -0.99f);
  }

  void gainsMasterAndMute() {
    AudioMixer m;
    m.openHeadless(48000);
    const auto g = m.addInput(AudioMixer::Game);
    m.setFocus(g);
    m.setMaster(0.5);
    m.push(g, constant(16384, 4));  // 0.5
    QVERIFY(std::fabs(level(m, 4) - 0.25f) < 1e-4f);
    m.setChannelGain(AudioMixer::Game, 0.5);
    m.push(g, constant(16384, 4));
    QVERIFY(std::fabs(level(m, 4) - 0.125f) < 1e-4f);
    m.setInputGain(g, 0.5);
    QVERIFY(std::fabs(m.effectiveGain(g) - 0.125) < 1e-9);
    m.setMuted(true);
    QCOMPARE(m.effectiveGain(g), 0.0);
    QVERIFY(!m.isAudible(g));
    QCOMPARE(m.push(g, constant(16384, 4)), qint64(0));
    QCOMPARE(m.queuedFrames(g), qint64(0));  // nothing piles up while silent
    m.setMuted(false);
    m.setGameSilenced(true);  // "muted in the background" affects only the game channel
    const auto t = m.addInput(AudioMixer::Tiles);
    m.setMixAll(true);
    QCOMPARE(m.effectiveGain(g), 0.0);
    QVERIFY(m.effectiveGain(t) > 0.0);
  }

  void exclusiveRoutingVersusMixAll() {
    AudioMixer m;
    m.openHeadless(48000);
    const auto g = m.addInput(AudioMixer::Game);
    const auto t1 = m.addInput(AudioMixer::Tiles, "u1");
    const auto t2 = m.addInput(AudioMixer::Tiles, "u2");
    m.setFocus(t1);
    QVERIFY(!m.mixAll());
    QVERIFY(m.isRouted(t1) && !m.isRouted(g) && !m.isRouted(t2));
    m.push(g, constant(1000, 4));
    QCOMPARE(m.queuedFrames(g), qint64(0));  // not routed: dropped
    m.setFocus(g);
    QVERIFY(m.isRouted(g) && !m.isRouted(t1));
    m.setMixAll(true);
    QVERIFY(m.isRouted(g) && m.isRouted(t1) && m.isRouted(t2));
    // per-person gains apply in both modes
    m.setPersonGain(AudioMixer::Tiles, "u2", 0.25);
    QCOMPARE(m.effectiveGain(t2), 0.25);
    QCOMPARE(m.effectiveGain(t1), 1.0);
    m.setMixAll(false);
    m.setFocus(t2);
    QCOMPARE(m.effectiveGain(t2), 0.25);
    QCOMPARE(m.effectiveGain(t1), 0.0);
    m.removeInput(t2);
    QCOMPARE(m.focus(), 0);
  }

  void duckingHalvesGameAndTilesOnly() {
    AudioMixer m;
    m.openHeadless(48000);
    m.setMixAll(true);
    const auto g = m.addInput(AudioMixer::Game);
    const auto t = m.addInput(AudioMixer::Tiles);
    const auto v = m.addVoiceInput("u9");
    m.setDucking(true);
    QCOMPARE(m.effectiveGain(g), 0.5);
    QCOMPARE(m.effectiveGain(t), 0.5);
    QCOMPARE(m.effectiveGain(v), 1.0);
    m.setDucking(false);
    QCOMPARE(m.effectiveGain(g), 1.0);
    m.setChannelGain(AudioMixer::Voice, 0.4);  // the Voice slider drives the voice channel, also with nobody talking
    QCOMPARE(m.effectiveGain(v), 0.4);
    m.push(v, constant(10000, 4));
    QVERIFY(level(m, 4) > 0.1f);
    m.removeVoiceInput(v);
    QCOMPARE(m.queuedFrames(v), qint64(0));
  }

  void pluginBusGeneratorIsMixedWithItsChannelGain() {
    AudioMixer m;
    m.openHeadless(48000);
    int calls = 0;
    int seenRate = 0;
    int left = 6;
    const int id = m.addGenerator("plugin", [&](float* acc, int frames, int rate) {
      ++calls;
      seenRate = rate;
      const int n = std::min(frames, left);
      for (int i = 0; i < n * 2; ++i) acc[i] += 0.5f;
      left -= n;
      return n;
    });
    m.setChannelGain(AudioMixer::Plugin, 0.5);
    int n = 0;
    QVERIFY(std::fabs(level(m, 4, &n) - 0.25f) < 1e-4f);
    QCOMPARE(n, 4);
    level(m, 4, &n);
    QCOMPARE(n, 2);
    level(m, 4, &n);
    QCOMPARE(n, 4);  // idle again: silence
    QCOMPARE(seenRate, 48000);
    m.setMuted(true);
    left = 4;
    QCOMPARE(level(m, 4), 0.0f);
    m.removeGenerator(id);
    const int before = calls;
    level(m, 4);
    QCOMPARE(calls, before);
  }

  void underrunsCountOncePerDrySpell() {
    AudioMixer m;
    m.openHeadless(48000);
    const auto g = m.addInput(AudioMixer::Game);
    m.setFocus(g);
    m.push(g, constant(100, 4));
    level(m, 4);
    level(m, 4);
    level(m, 4);
    QCOMPARE(m.underruns(g), 1);
    m.setUnderrunCounting(g, false);
    m.push(g, constant(100, 4));
    level(m, 4);
    level(m, 4);
    QCOMPARE(m.underruns(g), 1);
  }

  void stateListsChannelsAndInputs() {
    AudioMixer m;
    m.openHeadless(48000);
    const auto t = m.addInput(AudioMixer::Tiles, "u1");
    m.setFocus(t);
    m.setPersonGain(AudioMixer::Tiles, "u1", 0.5);
    m.setChannelGain(AudioMixer::Plugin, 0.3);
    const QVariantMap s = m.state();
    QCOMPARE(s.value("mix_all").toBool(), false);
    QCOMPARE(s.value("channels").toMap().value("plugin").toDouble(), 0.3);
    const QVariantMap in = s.value("inputs").toList().at(0).toMap();
    QCOMPARE(in.value("channel").toString(), QStringLiteral("tiles"));
    QCOMPARE(in.value("key").toString(), QStringLiteral("u1"));
    QCOMPARE(in.value("gain").toDouble(), 0.5);
    QVERIFY(in.value("audible").toBool());
  }
};

QTEST_GUILESS_MAIN(AudioMixerTest)
#include "audiomixer_test.moc"
