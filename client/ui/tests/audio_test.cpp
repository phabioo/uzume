// Tests without an audio device: format conversion and resampling path of AudioOutput.
#include <QtTest>
#include <cstring>

#include <QSignalSpy>
#include <QGuiApplication>
#include <QScreen>
#include <QSurfaceFormat>
#include <QWindow>
#include <QTemporaryDir>

#include "audiocontroller.h"
#include "audiooutput.h"
#include "playersettings.h"
#include "videocontroller.h"

using namespace uzume::ui;

namespace {
QByteArray pcm(std::initializer_list<qint16> v) {
  QByteArray b(static_cast<qsizetype>(v.size() * 2), 0);
  std::memcpy(b.data(), v.begin(), b.size());
  return b;
}
}  // namespace

class AudioTest : public QObject {
  Q_OBJECT
 private slots:
  void gainFullIsUnchanged() {
    const QByteArray in = pcm({1000, -1000, 32767, -32768});
    QCOMPARE(AudioOutput::applyGain(in, 1.0), in);
    QCOMPARE(AudioOutput::applyGain(in, 5.0), in);  // clamped to 1
  }

  void gainHalvesAndSilences() {
    const QByteArray in = pcm({1000, -1000, 32767, -32768});
    QCOMPARE(AudioOutput::applyGain(in, 0.5), pcm({500, -500, 16384, -16384}));
    QCOMPARE(AudioOutput::applyGain(in, 0.0), pcm({0, 0, 0, 0}));
    QCOMPARE(AudioOutput::applyGain(in, -1.0), pcm({0, 0, 0, 0}));  // clamped to 0
    QCOMPARE(AudioOutput::applyGain(in, 0.5).size(), in.size());
  }

  void volumeClampsAndPushIsNoOpWithoutDevice() {
    AudioOutput out;
    QCOMPARE(out.volume(), 1.0);
    out.setVolume(2.0);
    QCOMPARE(out.volume(), 1.0);
    out.setVolume(-3.0);
    QCOMPARE(out.volume(), 0.0);
    out.setVolume(0.25);
    QCOMPARE(out.volume(), 0.25);
    out.push(pcm({1, 2}));  // no device: nothing happens
    QCOMPARE(out.droppedBytes(), qint64(0));
  }

  void videoControllerPersistsAndAppliesVsync() {
    QTemporaryDir dir;
    {
      uzume::PlayerSettings settings(dir.path());
      VideoController v(&settings);
      QVERIFY(v.vsync());
      QCOMPARE(v.fullscreenScreen(), QString());
      QVERIFY(v.targetScreen() == nullptr);                // no choice: the window stays where it is
      QCOMPARE(v.screens().first().toMap().value("value").toString(), QString());
      v.setVsync(false);
      v.setFullscreenScreen(QStringLiteral("no-such-monitor"));
      QVERIFY(v.targetScreen() == nullptr);                // a monitor that is gone: fullscreen where the window is
      QCOMPARE(v.screens().last().toMap().value("value").toString(), QStringLiteral("no-such-monitor"));  // but the row keeps the choice
      QCOMPARE(VideoController::swapIntervalFor(true), 1);
      QCOMPARE(VideoController::swapIntervalFor(false), 0);
    }
    uzume::PlayerSettings settings(dir.path());
    VideoController v(&settings);
    QVERIFY(!v.vsync());
    QCOMPARE(v.fullscreenScreen(), QStringLiteral("no-such-monitor"));
    const QSurfaceFormat before = QSurfaceFormat::defaultFormat();
    v.applyStartupFormat();
    QCOMPARE(QSurfaceFormat::defaultFormat().swapInterval(), 0);
    v.setVsync(true);
    v.applyStartupFormat();
    QCOMPARE(QSurfaceFormat::defaultFormat().swapInterval(), 1);
    QSurfaceFormat::setDefaultFormat(before);
  }

  void windowedPositionIsRestoredAfterFullscreenMove() {
    QTemporaryDir dir;
    uzume::PlayerSettings settings(dir.path());
    VideoController v(&settings);
    QScreen* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);
    QWindow w;
    w.setScreen(screen);
    const QRect windowed(screen->geometry().left() + 120, screen->geometry().top() + 80, 640, 480);
    w.setGeometry(windowed);
    v.setFullscreenScreen(screen->name().isEmpty() ? QStringLiteral("screen-1") : screen->name());
    QVERIFY(v.targetScreen() != nullptr);
    v.moveToFullscreenScreen(&w);
    QCOMPARE(w.position(), screen->geometry().topLeft());   // moved to the chosen monitor's origin
    w.setGeometry(screen->geometry());                      // as fullscreen would
    v.moveToFullscreenScreen(&w);                            // a second call must not overwrite the saved geometry
    v.restoreWindowedPosition(&w);
    QCOMPARE(w.geometry(), windowed);
    QCOMPARE(w.screen(), screen);
    w.setGeometry(QRect(10, 10, 300, 200));
    v.restoreWindowedPosition(&w);                           // nothing saved any more: no-op
    QCOMPARE(w.geometry(), QRect(10, 10, 300, 200));
  }

  void controllerStepsMuteAndNotices() {
    QTemporaryDir dir;
    uzume::PlayerSettings settings(dir.path());
    AudioController c(&settings);
    QCOMPARE(c.volume(), 100);
    QVERIFY(!c.muted());
    QSignalSpy notice(&c, &AudioController::volumeNotice);
    c.volumeDown();
    QCOMPARE(c.volume(), 95);
    QCOMPARE(notice.takeLast().first().toString(), QStringLiteral("Volume 95 %"));
    c.setVolumeStep(10);
    c.volumeDown();
    QCOMPARE(c.volume(), 85);
    c.volumeUp();
    c.volumeUp();
    QCOMPARE(c.volume(), 100);  // clamped
    c.toggleMute();
    QVERIFY(c.muted());
    QVERIFY(c.effectiveMuted());
    QCOMPARE(notice.takeLast().first().toString(), QStringLiteral("Muted"));
    QCOMPARE(c.localGain(), 0.0);
    QCOMPARE(c.remoteGain(), 0.0);
    c.volumeDown();  // stays muted
    QVERIFY(c.muted());
    c.volumeUp();    // unmutes
    QVERIFY(!c.muted());
    QCOMPARE(notice.takeLast().first().toString(), QStringLiteral("Sound on · 100 %"));
    c.setVolume(0);
    QVERIFY(c.effectiveMuted());
    c.setVolume(72);
    QCOMPARE(c.localGain(), 0.72);
    c.setShowVolume(false);
    notice.clear();
    c.volumeDown();
    QCOMPARE(notice.size(), 0);
    c.setVolumeStep(7);  // rejected
    QCOMPARE(c.volumeStep(), 10);
    // persisted per PC
    AudioController again(&settings);
    QCOMPARE(again.volume(), c.volume());
    QCOMPARE(again.volumeStep(), 10);
    QVERIFY(!again.showVolume());
  }

  void controllerBackgroundMutesOnlyTheLocalGame() {
    QTemporaryDir dir;
    uzume::PlayerSettings settings(dir.path());
    AudioController c(&settings);
    c.setVolume(60);
    QSignalSpy gains(&c, &AudioController::gainsChanged);
    QVERIFY(c.muteInBackground());
    c.setAppActive(false);
    QCOMPARE(c.localGain(), 0.0);   // the game is silent in the background ...
    QCOMPARE(c.remoteGain(), 0.6);  // ... watched sessions stay audible
    QVERIFY(gains.count() >= 1);
    c.setAppActive(true);           // back: sound at once
    QCOMPARE(c.localGain(), 0.6);
    c.setMuteInBackground(false);
    c.setAppActive(false);
    QCOMPARE(c.localGain(), 0.6);
    c.setMuted(true);
    c.setAppActive(true);
    QCOMPARE(c.remoteGain(), 0.0);  // the master mute covers both
  }

  void toFloat() {
    const QByteArray out = AudioOutput::convertSamples(pcm({0, 16384, -32768, 32767}), QAudioFormat::Float);
    QCOMPARE(out.size(), 16);
    float f[4];
    std::memcpy(f, out.constData(), 16);
    QCOMPARE(f[0], 0.0f);
    QCOMPARE(f[1], 0.5f);
    QCOMPARE(f[2], -1.0f);
    QVERIFY(f[3] < 1.0f && f[3] > 0.999f);
  }
  void toInt32() {
    const QByteArray out = AudioOutput::convertSamples(pcm({1, -2}), QAudioFormat::Int32);
    QCOMPARE(out.size(), 8);
    qint32 v[2];
    std::memcpy(v, out.constData(), 8);
    QCOMPARE(v[0], 65536);
    QCOMPARE(v[1], -131072);
  }
  void toUInt8() {
    const QByteArray out = AudioOutput::convertSamples(pcm({0, -32768, 32767}), QAudioFormat::UInt8);
    QCOMPARE(out.size(), 3);
    QCOMPARE(static_cast<quint8>(out[0]), quint8(128));
    QCOMPARE(static_cast<quint8>(out[1]), quint8(0));
    QCOMPARE(static_cast<quint8>(out[2]), quint8(255));
  }
  void int16Passthrough() {
    const QByteArray in = pcm({1, 2, 3, 4});
    QCOMPARE(AudioOutput::convertSamples(in, QAudioFormat::Int16), in);
    QVERIFY(AudioOutput::convertSamples(in, QAudioFormat::Unknown).isEmpty());
  }
  // 0.6 D6: underrun counting and buffer fill.
  void readPadsWithSilence() {
    AudioQueueDevice d;
    d.configure(8, 0, 1000, 1000);
    d.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    d.append(QByteArray(16, 'x'));
    QByteArray out(30, 'z');  // rounds down to 3 frames; only 2 are queued: no padding
    QCOMPARE(d.read(out.data(), out.size()), qint64(16));
    QCOMPARE(out.left(16), QByteArray(16, 'x'));
    QCOMPARE(out.mid(16), QByteArray(14, 'z'));  // untouched
    out.fill('z');
    QCOMPARE(d.read(out.data(), 30), qint64(24));  // dry: whole request is silence
    QCOMPARE(out.left(24), QByteArray(24, '\0'));
    QCOMPARE(out.mid(24), QByteArray(6, 'z'));
  }
  void silenceIsMidpointForUInt8() {
    AudioQueueDevice d;
    d.configure(2, static_cast<char>(0x80), 100, 100);
    d.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    QByteArray out(4, 0);
    QCOMPARE(d.read(out.data(), 4), qint64(4));
    QCOMPARE(out, QByteArray(4, static_cast<char>(0x80)));
  }
  void queuedDataKeepsOrder() {
    AudioQueueDevice d;
    d.configure(2, 0, 100, 100);
    d.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    d.append(QByteArray("ab"));
    d.append(QByteArray("cd"));
    QCOMPARE(d.queuedBytes(), qint64(4));
    char o[2];
    QCOMPARE(d.read(o, 2), qint64(2));
    QCOMPARE(QByteArray(o, 2), QByteArray("ab"));
    QCOMPARE(d.read(o, 2), qint64(2));
    QCOMPARE(QByteArray(o, 2), QByteArray("cd"));
    QCOMPARE(d.queuedBytes(), qint64(0));
  }
  void underrunOncePerDrySpell() {
    AudioQueueDevice d;
    d.configure(2, 0, 100, 100);
    d.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    char o[4];
    d.read(o, 4);  // dry before any data: no underrun
    QCOMPARE(d.underruns(), 0);
    d.append(QByteArray(2, 'a'));
    d.read(o, 4);  // partial read, not dry yet
    QCOMPARE(d.underruns(), 0);
    d.read(o, 4);  // now dry after data
    QCOMPARE(d.underruns(), 1);
    d.read(o, 4);  // still dry: not counted again
    d.read(o, 4);
    QCOMPARE(d.underruns(), 1);
    d.append(QByteArray(2, 'b'));
    d.read(o, 4);
    d.read(o, 4);
    QCOMPARE(d.underruns(), 2);
  }
  void underrunNotCountedWhileOff() {
    AudioQueueDevice d;
    d.configure(2, 0, 100, 100);
    d.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    char o[4];
    d.setUnderrunCounting(false);
    d.append(QByteArray(2, 'a'));
    d.read(o, 4);
    d.read(o, 4);
    QCOMPARE(d.underruns(), 0);
    d.append(QByteArray(2, 'a'));
    d.setUnderrunCounting(true);  // data predates re-enabling
    d.read(o, 4);
    d.read(o, 4);
    QCOMPARE(d.underruns(), 0);
    d.append(QByteArray(2, 'a'));
    d.read(o, 4);
    d.read(o, 4);
    QCOMPARE(d.underruns(), 1);
  }
  void capDropsOldestWholeFrames() {
    AudioQueueDevice d;
    d.configure(4, 0, 100, 8);
    QCOMPARE(d.append(QByteArray("aaaabbbb")), qint64(0));
    QCOMPARE(d.append(QByteArray("cccc")), qint64(4));
    QCOMPARE(d.queuedBytes(), qint64(8));
    QCOMPARE(d.append(QByteArray(20, 'd')), qint64(20));  // more than the cap in one go
    QCOMPARE(d.queuedBytes(), qint64(8));
    d.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    char o[8];
    d.read(o, 8);
    QCOMPARE(QByteArray(o, 8), QByteArray(8, 'd'));
  }
  void bytesAvailableNeverZero() {
    AudioQueueDevice d;
    d.configure(8, 0, 1200, 8000);
    d.open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    QCOMPARE(d.bytesAvailable(), qint64(1200));  // empty: nominal
    d.append(QByteArray(4000, 'x'));
    QCOMPARE(d.bytesAvailable(), qint64(4000));  // queued
    QVERIFY(d.isSequential());
  }
  void underrunsStartAtZeroWithoutDevice() {
    AudioOutput out;
    QCOMPARE(out.underruns(), 0);
    QCOMPARE(out.bufferedMs(), 0.0);
    out.setUnderrunCounting(false);
    out.setUnderrunCounting(true);
    QCOMPARE(out.underruns(), 0);
  }
  void rateChangeWithoutDeviceIsIgnored() {  // PE-10: with a device the resampler follows (needs hardware, not testable here)
    AudioOutput out;
    out.setCoreRate(44100);
    QCOMPARE(out.coreRate(), 0);
    QVERIFY(!out.isActive());
  }

  void bufferFillInMilliseconds() {
    // 48 kHz, 8-byte float frames: 14400-byte buffer (37.5 ms), 7200 free -> 7200 bytes queued = 900 frames = 18.75 ms
    QVERIFY(qAbs(AudioOutput::bufferedMsFor(14400, 7200, 0, 8, 48000) - 18.75) < 1e-9);
    // plus what waits in the device queue
    QVERIFY(qAbs(AudioOutput::bufferedMsFor(14400, 7200, 3600, 8, 48000) - 28.125) < 1e-9);
    QCOMPARE(AudioOutput::bufferedMsFor(14400, 14400, 0, 8, 48000), 0.0);  // empty sink
    QCOMPARE(AudioOutput::bufferedMsFor(100, 400, -5, 8, 48000), 0.0);     // bogus values never go negative
    QCOMPARE(AudioOutput::bufferedMsFor(100, 0, 0, 0, 48000), 0.0);        // no format yet
  }
  void resampleThenFloat() {
    // 32768 -> 48000 Hz, 1 s constant signal: ~48000 frames of float stereo (8 bytes per frame).
    LinearResampler r(32768, 48000);
    QByteArray in(32768 * 4, 0);
    auto* s = reinterpret_cast<qint16*>(in.data());
    for (int i = 0; i < 32768 * 2; ++i) s[i] = 16384;
    const QByteArray out = AudioOutput::convertSamples(r.process(in), QAudioFormat::Float);
    const qsizetype frames = out.size() / 8;
    QVERIFY(std::abs(frames - 48000) <= 2);
    float f;
    std::memcpy(&f, out.constData() + out.size() - 4, 4);
    QCOMPARE(f, 0.5f);
  }
};

QTEST_MAIN(AudioTest)
#include "audio_test.moc"
