// Frontend behavior against the Uzume test core (test_core.cpp, generic flavor), no real emulator: core and game
// loading, frames, audio, SAVE_RAM, joypad (bitmask and per button), pointer, core options (V2/V1/V0), variables,
// directory queries, no-game probe, fast-forward query, AV info. Never skips.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "core_options.h"
#include "emulator_backend.h"
#include "libretro_backend.h"
#include "test_core_layout.h"

using namespace uzume::emu;
namespace tc = uzume::testcore;

namespace {
uint dataPx(const QImage& img, int idx) { return img.pixel(idx, tc::kDataRow) & 0xFFFFFFu; }
bool cellPressed(const QImage& img, unsigned port, int id) {
  return (img.pixel(id * tc::kCellWidth + 8, tc::kStripY0 + static_cast<int>(port) * tc::kStripHeight + 8) & 0xFFFFFFu) == 0u;
}
QByteArray hashOf(const QImage& img) {
  QCryptographicHash h(QCryptographicHash::Sha256);
  for (int y = 0; y < img.height(); ++y) h.addData(QByteArrayView(reinterpret_cast<const char*>(img.constScanLine(y)), img.width() * 4));
  return h.result().toHex();
}
}  // namespace

class TestCoreTest : public QObject {
  Q_OBJECT
  QTemporaryDir m_dir;
  QString m_core = QStringLiteral(FB_TEST_CORE_PATH);
  QString sysDir() const { return m_dir.filePath(QStringLiteral("system")); }
  QString saveDir() const { return m_dir.filePath(QStringLiteral("save")); }
  QString dummyGame(const QByteArray& data = "dummy game", const QString& name = QStringLiteral("game.bin")) const {
    QFile f(m_dir.filePath(name));
    [&] { QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); }();
    f.write(data);
    return f.fileName();
  }
  bool start(LibretroBackend& be, const QString& game, QString* err) {
    be.setSystemDirectory(sysDir());
    be.setSaveDirectory(saveDir());
    return be.loadCore(m_core, err) && be.loadGame(game, err);
  }
  static bool run(LibretroBackend& be, int n) {
    for (int i = 0; i < n; ++i)
      if (!be.runFrame()) return false;
    return true;
  }

 private slots:
  void init() {
    for (const char* v : {"FB_TEST_CORE_OPTIONS", "FB_TEST_CORE_NO_BITMASK", "FB_TEST_CORE_SRAM_SIZE", "FB_TEST_CORE_SRAM_DELAY_FRAMES",
                          "FB_TEST_CORE_SRAM_WRITE", "FB_TEST_CORE_FF_INHIBIT", "FB_TEST_CORE_WRITE_DIRS", "FB_TEST_CORE_FPS",
                          "FB_TEST_CORE_SAMPLE_RATE", "FB_TEST_CORE_NO_NOGAME"})
      qunsetenv(v);
    qputenv("FB_TEST_CORE_FLAVOR", "generic");
    QDir(saveDir()).removeRecursively();
    QVERIFY(QDir().mkpath(sysDir()));
    QVERIFY(QDir().mkpath(saveDir()));
  }

  void coreInfoAndAvInfo() {
    LibretroBackend be;
    QString err;
    be.setSystemDirectory(sysDir());
    be.setSaveDirectory(saveDir());
    QVERIFY2(be.loadCore(m_core, &err), qPrintable(err));
    const CoreInfo info = be.coreInfo();
    QCOMPARE(info.name, QStringLiteral("Uzume Test Core"));
    QCOMPARE(info.version, QStringLiteral("1.0-test"));
    QVERIFY(info.needFullpath);
    QVERIFY(info.extensions.contains(QStringLiteral(".nds")));
    QVERIFY(info.extensions.contains(QStringLiteral(".bin")));
    QVERIFY2(be.loadGame(dummyGame(), &err), qPrintable(err));
    const AvInfo av = be.avInfo();
    QCOMPARE(av.width, tc::kWidth);
    QCOMPARE(av.height, tc::kHeight);
    QCOMPARE(av.fps, 50.0);
    QCOMPARE(av.sampleRate, 32000.0);
    QVERIFY(qAbs(av.aspectRatio - 256.0 / 384.0) < 0.001);
    be.unloadCore();

    qputenv("FB_TEST_CORE_FPS", "60");
    qputenv("FB_TEST_CORE_SAMPLE_RATE", "44100");
    LibretroBackend be2;
    QVERIFY2(start(be2, dummyGame(), &err), qPrintable(err));
    QCOMPARE(be2.avInfo().fps, 60.0);
    QCOMPARE(be2.avInfo().sampleRate, 44100.0);
    be2.unloadCore();
  }

  void gameFileIsLoaded() {
    LibretroBackend be;
    QString err;
    const QByteArray bytes("0123456789abcdef");
    QVERIFY2(start(be, dummyGame(bytes), &err), qPrintable(err));
    QVERIFY(run(be, 2));
    uint sum = 0;
    for (const char c : bytes) sum += static_cast<uchar>(c);
    QCOMPARE(dataPx(be.videoFrame(), tc::DataRomSize), uint(bytes.size()));
    QCOMPARE(dataPx(be.videoFrame(), tc::DataRomSum), sum);
    QCOMPARE(dataPx(be.videoFrame(), tc::DataLoads), 1u);
    be.unloadCore();

    LibretroBackend missing;
    QVERIFY(!start(missing, m_dir.filePath(QStringLiteral("does-not-exist.nds")), &err));
    QVERIFY(!err.isEmpty());
    missing.unloadCore();
  }

  void framesAreDeterministic() {
    QByteArray first;
    for (int pass = 0; pass < 2; ++pass) {
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, dummyGame(), &err), qPrintable(err));
      QVERIFY(run(be, 1));
      const QImage f1 = be.videoFrame();
      QVERIFY(run(be, 59));
      const QImage img = be.videoFrame();
      QCOMPARE(img.format(), QImage::Format_RGB32);
      QCOMPARE(img.size(), QSize(tc::kWidth, tc::kHeight));
      QCOMPARE(be.frameCount(), quint64(60));
      QCOMPARE(dataPx(img, tc::DataFrame), 60u);
      QVERIFY(hashOf(f1) != hashOf(img));  // the cyan square moves
      auto is = [&](int x, int y, QRgb c) { return (img.pixel(x, y) & 0xFFFFFFu) == (c & 0xFFFFFFu); };
      QVERIFY(is(64, 48, 0xFFFF0000));
      QVERIFY(is(192, 48, 0x00FF00));
      QVERIFY(is(64, 140, 0x0000FF));
      QVERIFY(is(192, 140, 0xFFFFFF));
      QVERIFY(is(128, 288, 0xFF00FF));
      const QByteArray h = hashOf(img);
      if (pass == 0) first = h; else QCOMPARE(h, first);  // second pass loads the core again
      be.unloadCore();
    }
  }

  void audioBatchesPerFrame() {
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, dummyGame(), &err), qPrintable(err));
    qint64 bytes = 0;
    for (int i = 0; i < 10; ++i) {
      QVERIFY(be.runFrame());
      bytes += be.takeAudio().size();
    }
    QCOMPARE(bytes, qint64(10 * 640 * 4));  // 32000 Hz / 50 fps = 640 stereo frames of 16 bit
    be.unloadCore();
  }

  void joypadBitmaskAndPerButton() {
    for (const bool bitmask : {true, false}) {
      if (bitmask) qunsetenv("FB_TEST_CORE_NO_BITMASK"); else qputenv("FB_TEST_CORE_NO_BITMASK", "1");
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, dummyGame(), &err), qPrintable(err));
      be.setJoypadState(0, buttonMask(JoypadButton::A) | buttonMask(JoypadButton::R2) | buttonMask(JoypadButton::Up));
      be.setJoypadState(1, buttonMask(JoypadButton::L2) | buttonMask(JoypadButton::Start));
      QVERIFY(run(be, 2));
      QImage img = be.videoFrame();
      QCOMPARE(dataPx(img, tc::DataBitmask), bitmask ? 1u : 0u);
      for (int id = 0; id < 16; ++id) {
        const bool p0 = id == int(JoypadButton::A) || id == int(JoypadButton::R2) || id == int(JoypadButton::Up);
        const bool p1 = id == int(JoypadButton::L2) || id == int(JoypadButton::Start);
        QCOMPARE(cellPressed(img, 0, id), p0);
        QCOMPARE(cellPressed(img, 1, id), p1);
      }
      QVERIFY(cellPressed(img, 0, 13));   // R2 is bit 13 (ZR of the 3ds profile)
      QVERIFY(!cellPressed(img, 0, 12));  // L2 is not
      be.setJoypadState(0, 0);
      be.setJoypadState(1, 0);
      QVERIFY(run(be, 1));
      img = be.videoFrame();
      for (int id = 0; id < 16; ++id) QVERIFY(!cellPressed(img, 0, id) && !cellPressed(img, 1, id));
      be.unloadCore();
    }
  }

  void pointerReachesTheCore() {
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, dummyGame(), &err), qPrintable(err));
    be.setPointer(0.5, 0.75, true);
    QVERIFY(run(be, 2));
    QImage img = be.videoFrame();
    QCOMPARE(dataPx(img, tc::DataPointerDown), 1u);
    QVERIFY(qAbs(int(dataPx(img, tc::DataPointerX)) - 0x8000) <= 1);                // 0.5 -> 0
    QVERIFY(qAbs(int(dataPx(img, tc::DataPointerY)) - (0x8000 + 0x3fff)) <= 2);     // 0.75 -> +0.5 of full scale
    be.setPointer(0.0, 0.0, false);
    QVERIFY(run(be, 1));
    img = be.videoFrame();
    QCOMPARE(dataPx(img, tc::DataPointerDown), 0u);
    be.unloadCore();
  }

  void analogSticksReachTheCoreAsValues() {
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, dummyGame(), &err), qPrintable(err));
    const auto axis = [](const QImage& img, int i) { return int(dataPx(img, tc::DataAnalogLeftX + i)) - 0x8000; };
    // Digital bits only: full deflection (circle pad = bits 20..23: up down left right).
    be.setJoypadState(0, (1u << 23) | (1u << 20));
    QVERIFY(run(be, 2));
    QImage img = be.videoFrame();
    QCOMPARE(axis(img, 0), 0x7fff);
    QCOMPARE(axis(img, 1), -0x7fff);
    // A real stick position wins over the digital bits (hand-over frames).
    const qint16 axes[4] = {12000, -20000, -300, 32767};
    be.setAnalogState(0, axes);
    QVERIFY(run(be, 2));
    img = be.videoFrame();
    QCOMPARE(axis(img, 0), 12000);
    QCOMPARE(axis(img, 1), -20000);
    QCOMPARE(axis(img, 2), -300);
    QCOMPARE(axis(img, 3), 32767);
    // Neutral again: back to the digital fallback, then to zero.
    const qint16 none[4] = {0, 0, 0, 0};
    be.setAnalogState(0, none);
    be.setJoypadState(0, 0);
    QVERIFY(run(be, 2));
    img = be.videoFrame();
    for (int i = 0; i < 4; ++i) QCOMPARE(axis(img, i), 0);
    be.unloadCore();
  }

  void resetAndFastForwardQuery() {
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, dummyGame(), &err), qPrintable(err));
    QVERIFY(be.supportsFastForward());
    QVERIFY(run(be, 1));
    QCOMPARE(dataPx(be.videoFrame(), tc::DataFastForward), 0u);
    be.setFastForwarding(true, 4.0);
    QVERIFY(run(be, 1));
    QCOMPARE(dataPx(be.videoFrame(), tc::DataFastForward), 1u);
    be.setFastForwarding(false, 1.0);
    be.reset();
    QVERIFY(run(be, 1));
    QCOMPARE(dataPx(be.videoFrame(), tc::DataFastForward), 0u);
    QCOMPARE(dataPx(be.videoFrame(), tc::DataResets), 1u);
    be.unloadCore();

    qputenv("FB_TEST_CORE_FF_INHIBIT", "1");
    LibretroBackend inhibit;
    QVERIFY2(start(inhibit, dummyGame(), &err), qPrintable(err));
    QVERIFY(!inhibit.supportsFastForward());  // SET_FASTFORWARDING_OVERRIDE with inhibit_toggle
    inhibit.unloadCore();
  }

  void directoriesAreAnswered() {
    qputenv("FB_TEST_CORE_WRITE_DIRS", "1");
    LibretroBackend be;
    QString err;
    QVERIFY2(start(be, dummyGame(), &err), qPrintable(err));
    QVERIFY(run(be, 1));
    QCOMPARE(dataPx(be.videoFrame(), tc::DataDirs), 3u);
    be.unloadCore();
    QFile f(QDir(saveDir()).filePath(QStringLiteral("uzume_test_core_dirs.txt")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(f.readAll());
    QVERIFY2(text.contains(QStringLiteral("system=") + QDir::toNativeSeparators(sysDir())), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("save=") + QDir::toNativeSeparators(saveDir())), qPrintable(text));

    // Without directories the core is told so (the query fails).
    LibretroBackend none;
    QVERIFY(none.loadCore(m_core, &err));
    QVERIFY(none.loadGame(dummyGame(), &err));
    QVERIFY(run(none, 1));
    QCOMPARE(dataPx(none.videoFrame(), tc::DataDirs), 0u);
    none.unloadCore();
  }

  void saveRamIsLoadedAndWritten() {
    const QString game = dummyGame("rom", QStringLiteral("saves.bin"));
    const QString sav = QDir(saveDir()).filePath(QStringLiteral("saves.sav"));
    {
      qputenv("FB_TEST_CORE_SRAM_WRITE", "7");
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, game, &err), qPrintable(err));
      QCOMPARE(be.saveMemorySize(), qint64(8192));
      QVERIFY(run(be, 3));
      QCOMPARE(dataPx(be.videoFrame(), tc::DataSram0), 7u);
      be.unloadCore();  // flushes
    }
    QFile f(sav);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray written = f.readAll();
    f.close();
    QCOMPARE(written.size(), 8192);
    QCOMPARE(uchar(written.at(0)), uchar(7));
    {
      // An existing save file is loaded into SAVE_RAM before the first frame.
      qunsetenv("FB_TEST_CORE_SRAM_WRITE");
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
      f.write(QByteArray("\x2a\x15", 2) + QByteArray(8190, '\0'));
      f.close();
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, game, &err), qPrintable(err));
      QVERIFY(run(be, 2));
      QCOMPARE(dataPx(be.videoFrame(), tc::DataSram0), 0x2au);
      QCOMPARE(dataPx(be.videoFrame(), tc::DataSram1), 0x15u);
      be.unloadCore();
    }
    {
      // SAVE_RAM that appears late (after N frames) is picked up.
      QFile::remove(sav);
      qputenv("FB_TEST_CORE_SRAM_DELAY_FRAMES", "5");
      qputenv("FB_TEST_CORE_SRAM_WRITE", "9");
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, game, &err), qPrintable(err));
      QVERIFY(run(be, 10));
      be.unloadCore();
      QVERIFY(f.open(QIODevice::ReadOnly));
      QCOMPARE(uchar(f.readAll().at(0)), uchar(9));
    }
  }

  void coreOptionsV2WithCategories() {
    LibretroBackend be;
    QString err;
    be.setSystemDirectory(sysDir());
    be.setSaveDirectory(saveDir());
    QVERIFY2(be.loadCore(m_core, &err), qPrintable(err));
    QVERIFY(be.coreOptions().isEmpty());  // registered in retro_load_game, like melonDS DS
    QVERIFY(be.setCoreOption(QStringLiteral("test_text_mode"), QStringLiteral("gamma")));  // remembered until the game is loaded
    QVERIFY2(be.loadGame(dummyGame(), &err), qPrintable(err));
    const QList<CoreOption> opts = be.coreOptions();
    QCOMPARE(opts.size(), 5);
    QCOMPARE(be.coreOptionCategories().size(), 6);
    const CoreOption* mode = nullptr;
    const CoreOption* hidden = nullptr;
    for (const CoreOption& o : opts) {
      if (o.key == QLatin1String("test_text_mode")) mode = &o;
      if (o.key == QLatin1String("test_hidden_by_core")) hidden = &o;
    }
    QVERIFY(mode && hidden);
    QCOMPARE(mode->values.size(), 3);
    QCOMPARE(mode->defaultValue, QStringLiteral("alpha"));
    QCOMPARE(mode->currentValue, QStringLiteral("gamma"));  // the value set before the game
    QCOMPARE(mode->categoryKey, QStringLiteral("video"));
    QVERIFY(!mode->description.isEmpty());
    QVERIFY(!hidden->visible);  // SET_CORE_OPTIONS_DISPLAY
    QVERIFY(run(be, 1));
    QCOMPARE(dataPx(be.videoFrame(), tc::DataOptionTag), uint((5 << 8) | 'g'));  // GET_VARIABLE "gamma"
    QVERIFY(be.setCoreOption(QStringLiteral("test_text_mode"), QStringLiteral("beta")));
    QVERIFY(run(be, 2));
    QCOMPARE(dataPx(be.videoFrame(), tc::DataOptionTag), uint((4 << 8) | 'b'));
    QVERIFY(dataPx(be.videoFrame(), tc::DataVarUpdates) >= 1u);  // GET_VARIABLE_UPDATE reported the change
    QCOMPARE(dataPx(be.videoFrame(), tc::DataUnknownVar), 1u);  // PE-8: an unknown key resets var.value to NULL
    QVERIFY(!be.setCoreOption(QStringLiteral("test_text_mode"), QStringLiteral("nope")));
    QVERIFY(!be.setCoreOption(QStringLiteral("invented"), QStringLiteral("x")));
    be.unloadCore();
  }

  void coreOptionsV1AndV0() {
    for (const char* api : {"v1", "v0"}) {
      qputenv("FB_TEST_CORE_OPTIONS", api);
      LibretroBackend be;
      QString err;
      QVERIFY2(start(be, dummyGame(), &err), qPrintable(err));
      const QList<CoreOption> opts = be.coreOptions();
      QVERIFY2(opts.size() == 5, api);
      QVERIFY(be.coreOptionCategories().isEmpty());
      for (const CoreOption& o : opts) {
        if (o.key != QLatin1String("test_gl_scale")) continue;
        QCOMPARE(o.values.size(), 4);
        QCOMPARE(o.defaultValue, QStringLiteral("1"));
        QCOMPARE(o.currentValue, QStringLiteral("1"));
      }
      QVERIFY(be.setCoreOption(QStringLiteral("test_text_mode"), QStringLiteral("beta")));
      QVERIFY(run(be, 2));
      QCOMPARE(dataPx(be.videoFrame(), tc::DataOptionTag), uint((4 << 8) | 'b'));
      be.unloadCore();
    }
  }

  void noGameProbe() {
    const CoreProbe p = probeCore(m_core, sysDir(), saveDir());
    QVERIFY2(p.ok, qPrintable(p.error));
    QCOMPARE(p.info.name, QStringLiteral("Uzume Test Core"));
    QCOMPARE(p.options.size(), 5);  // registered by retro_load_game(NULL)
    QVERIFY(!p.categories.isEmpty());
    const CoreProbe again = coreProbeFromJson(coreProbeToJson(p));  // cache round trip
    QVERIFY(again.ok);
    QCOMPARE(again.options.size(), 5);
    QCOMPARE(again.categories.size(), p.categories.size());

    qputenv("FB_TEST_CORE_NO_NOGAME", "1");  // not declared: the core is never loaded without a game
    const CoreProbe none = probeCore(m_core, sysDir(), saveDir());
    QVERIFY2(none.ok, qPrintable(none.error));
    QVERIFY(none.options.isEmpty());
    LibretroBackend be;
    QString err;
    QVERIFY(be.loadCore(m_core, &err));
    QVERIFY(!be.supportsNoGame());
    QVERIFY(!be.loadGame(QString(), &err));
    be.unloadCore();
  }

  void hardwareRequestFallsBackToSoftwareWithoutGui() {
    // QCoreApplication: no OpenGL context possible. The core asks for one in "opengl" mode, is rejected and must
    // deliver ordinary software frames.
    LibretroBackend be;
    QString err;
    be.setSystemDirectory(sysDir());
    be.setSaveDirectory(saveDir());
    QVERIFY2(be.loadCore(m_core, &err), qPrintable(err));
    QVERIFY2(be.loadGame(dummyGame(), &err), qPrintable(err));
    be.unloadGame();
    QVERIFY(be.setCoreOption(QStringLiteral("test_render_mode"), QStringLiteral("opengl")));
    QVERIFY(be.setCoreOption(QStringLiteral("test_gl_scale"), QStringLiteral("2")));
    QVERIFY2(be.loadGame(dummyGame(), &err), qPrintable(err));
    QVERIFY(run(be, 3));
    QCOMPARE(be.videoFrame().size(), QSize(tc::kWidth, tc::kHeight));
    QVERIFY(be.renderInfo().hwRequested);
    QVERIFY(!be.renderInfo().hwActive);
    be.unloadCore();
  }
};

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  TestCoreTest t;
  return QTest::qExec(&t, argc, argv);
}
#include "test_core_test.moc"
