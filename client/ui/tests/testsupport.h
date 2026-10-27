#pragma once
// Shared helpers of the UI tests: harness (controller + Main.qml offscreen), QML warning counter,
// screenshots (only if UZUME_SHOT_DIR is set; never in the repo).

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>
#include <QtQml/QQmlExtensionPlugin>
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <QGuiApplication>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <memory>
#include <string>
#include <vector>

#include "playercontroller.h"
#include "uifonts.h"

Q_IMPORT_QML_PLUGIN(Uzume_AppPlugin)

namespace uitest {

inline std::atomic<int>& warningCount() {
  static std::atomic<int> n{0};
  return n;
}
inline QtMessageHandler& previousHandler() {
  static QtMessageHandler h = nullptr;
  return h;
}
inline void countingHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
  // Diagnostics: warnings/errors immediately and unbuffered on stderr so they stay visible even after a crash
  // (e.g. on Windows CI with a buffered pipe).
  if (type != QtDebugMsg && type != QtInfoMsg) {
    const QByteArray line = msg.toLocal8Bit();
    std::fprintf(stderr, "[uitest] %s: %s\n", type == QtWarningMsg ? "warning" : "error", line.constData());
    std::fflush(stderr);
  }
  // Only QML/Qt Quick warnings are counted. Environment-related messages of other categories
  // (fonts, multimedia/audio backend without a device, platform) deliberately do not cause a failure.
  if (type == QtWarningMsg || type == QtCriticalMsg) {
    const bool qml = msg.contains(QLatin1String(".qml")) || msg.contains(QLatin1String("qrc:/")) ||
                     msg.contains(QLatin1String("QML")) || msg.contains(QLatin1String("Qt Quick Layouts")) ||
                     msg.contains(QLatin1String("recursive rearrange")) || msg.contains(QLatin1String("Polish loop")) ||
                     (ctx.category != nullptr && (QByteArrayView(ctx.category).startsWith("qt.qml") ||
                                                  QByteArrayView(ctx.category).startsWith("qt.quick")));
    if (qml) {
      ++warningCount();
    }
  }
  if (previousHandler() != nullptr) {
    previousHandler()(type, ctx, msg);
  }
}
#ifdef Q_OS_WIN
inline LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
  std::fprintf(stderr, "[uitest] unhandled exception 0x%08lx at address %p\n", ep->ExceptionRecord->ExceptionCode,
               ep->ExceptionRecord->ExceptionAddress);
  std::fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif
inline void crashSignal(int sig) {
  std::fprintf(stderr, "[uitest] signal %d (crash/abort)\n", sig);
  std::fflush(stderr);
  std::_Exit(3);
}
// UZUME_TEST_LOG_DIR=<dir>: also write the QtTest log to <dir>/<test exe name>.txt (see testutil/processguard.h).
inline std::vector<std::string> logArgs(const char* argv0) {
  const QByteArray dir = qgetenv("UZUME_TEST_LOG_DIR");
  if (dir.isEmpty()) {
    return {};
  }
  QString name = QFileInfo(QString::fromLocal8Bit(argv0)).completeBaseName();
  if (qEnvironmentVariableIntValue("UZUME_TEST_WIDE_FONT") != 0) {
    name += QStringLiteral("_widefont");   // the *_widefont ctest variants run the same exe: keep both logs
  }
  QDir().mkpath(QString::fromLocal8Bit(dir));
  const QString file = QDir(QString::fromLocal8Bit(dir)).filePath(name + QStringLiteral(".txt"));
  return {"-o", (file + QStringLiteral(",txt")).toLocal8Bit().toStdString(), "-o", "-,txt"};
}
// Must run before QGuiApplication: unbuffered output + crash message so a failure is never silent.
inline void prepareProcess() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef Q_OS_WIN
  SetUnhandledExceptionFilter(crashFilter);
#endif
  std::set_terminate([]() {
    std::fprintf(stderr, "[uitest] std::terminate (uncaught exception)\n");
    std::fflush(stderr);
    std::_Exit(4);
  });
#ifdef Q_OS_WIN
  // CRT fatal paths that bypass the SEH filter and signal(): purecall, invalid parameter, abort message box.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _set_purecall_handler([]() {
    std::fprintf(stderr, "[uitest] pure virtual function call\n");
    std::fflush(stderr);
    std::_Exit(5);
  });
  _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    std::fprintf(stderr, "[uitest] CRT invalid parameter\n");
    std::fflush(stderr);
    std::_Exit(6);
  });
#endif
  for (int sig : {SIGSEGV, SIGABRT, SIGILL, SIGFPE}) {
    std::signal(sig, crashSignal);
  }
}

// Custom main instead of QTEST_MAIN: unbuffered, console executable, "-v2" (every test function is reported).
// A test that needs a core: QSKIP without one (real-core tests, nightly); in the *_testcore variants (FB_TEST_CORE, the
// Uzume test core is always present) a missing precondition must fail instead of silently skipping.
#ifdef FB_TEST_CORE
#define UITEST_SKIP_NO_CORE(msg) QFAIL(msg)
#else
#define UITEST_SKIP_NO_CORE(msg) QSKIP(msg)
#endif

#define UITEST_MAIN(TestClass)                                  \
  int main(int argc, char** argv) {                             \
    uitest::prepareProcess();                                   \
    std::vector<char*> args(argv, argv + argc);                 \
    char verbose[] = "-v2";                                     \
    char maxw[] = "-maxwarnings";                               \
    char maxwN[] = "0";                                         \
    args.push_back(verbose);                                    \
    args.push_back(maxw);                                       \
    args.push_back(maxwN);                                      \
    const std::vector<std::string> logExtra = uitest::logArgs(argv[0]); \
    for (const std::string& a : logExtra) args.push_back(const_cast<char*>(a.c_str())); \
    int n = static_cast<int>(args.size());                      \
    QGuiApplication app(n, args.data());                        \
    uzume::ui::loadDesignFonts();                               \
    uitest::applyWideFontIfRequested();                         \
    TestClass tc;                                               \
    const int rc = QTest::qExec(&tc, n, args.data());           \
    std::fprintf(stderr, "[uitest] exit code %d\n", rc);       \
    return rc;                                                  \
  }

// UZUME_TEST_WIDE_FONT=1: noticeably wider text, the way Windows renders the same pixel sizes. Layout tests then
// catch items that only fit with Linux font metrics. QML text sets its own pixel size but inherits the application
// font's letter spacing, so a large absolute spacing (about +30%) widens nearly all text at creation time, before any
// layout runs. Must run before QML loads.
inline void applyWideFontIfRequested() {
  const int v = qEnvironmentVariableIntValue("UZUME_TEST_WIDE_FONT");   // 1 = 3 px spacing, larger = that many px
  if (v == 0) return;
  QFont f = QGuiApplication::font();
  f.setLetterSpacing(QFont::AbsoluteSpacing, qMax(3, v));
  QGuiApplication::setFont(f);
}

// All warnings/errors from here on count (QML load errors, binding errors, Qt warnings).
inline void installWarningCounter() { previousHandler() = qInstallMessageHandler(countingHandler); }

inline QString sha256Hex(const QByteArray& d) {
  return QString::fromLatin1(QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex());
}

inline QJsonObject gameJson(const QString& id, const QString& title, const QString& sha, qint64 size,
                            const QString& file = QString()) {
  return {{QStringLiteral("id"), id},
          {QStringLiteral("title"), title},
          {QStringLiteral("system"), QStringLiteral("nds")},
          {QStringLiteral("rom"), QJsonObject{{QStringLiteral("sha256"), sha},
                                             {QStringLiteral("size"), size},
                                             {QStringLiteral("filename"), file.isEmpty() ? id + QStringLiteral(".nds") : file}}},
          {QStringLiteral("uploaded_by"), QStringLiteral("u_test_1")},
          {QStringLiteral("added_at"), QStringLiteral("2026-01-01T12:00:00Z")}};
}

inline void saveShot(QQuickWindow* w, const QString& name) {
  const QString dir = qEnvironmentVariable("UZUME_SHOT_DIR");
  if (dir.isEmpty() || w == nullptr) {
    return;
  }
  QDir().mkpath(dir);
  QTest::qWait(150);  // let layout/animations settle
  const QImage img = w->grabWindow();
  img.save(QDir(dir).filePath(name + QStringLiteral(".png")));
}

inline bool isAllBlack(const QImage& img) {
  for (int y = 0; y < img.height(); ++y) {
    const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    for (int x = 0; x < img.width(); ++x) {
      if ((line[x] & 0x00ffffff) != 0) {
        return false;
      }
    }
  }
  return true;
}

// Controller + Main.qml in an offscreen window.
struct Harness {
  QTemporaryDir dir;
  std::unique_ptr<uzume::ui::PlayerController> controller;
  std::unique_ptr<QQmlApplicationEngine> engine;
  QQuickWindow* window = nullptr;
  // SDL gamepads are off by default (most tests need no input devices). The Controllers tests turn them on and
  // drive the service from the test (gamepadPollMs = 0: no timer, poll() is called explicitly).
  bool gamepads = false;
  int gamepadPollMs = 0;
  // Connection check: fake probes (tests never touch the network); empty = the real probes.
  std::function<std::unique_ptr<uzume::ConnectionProbes>()> connectionProbes;
  std::function<void(uzume::ConnectionTarget&)> connectionTargetPatch;

  bool start(bool probeCores = false) {
    uzume::ui::PlayerController::Options o;
    o.dataDir = dir.path();
    o.memoryCredentials = true;
    o.probeCoreVersions = probeCores;
    o.enableGamepads = gamepads;
    o.gamepadPollMs = gamepadPollMs;
    o.connectionProbes = connectionProbes;
    o.connectionTargetPatch = connectionTargetPatch;
    o.allowHttp = true;
    controller = std::make_unique<uzume::ui::PlayerController>(o);
    controller->connection()->setPollIntervalMs(80);
    engine = std::make_unique<QQmlApplicationEngine>();
    engine->addImportPath(QStringLiteral("qrc:/qt/qml"));
    engine->addImageProvider(QStringLiteral("screenshots"), controller->createScreenshotImageProvider());
    engine->setInitialProperties({{QStringLiteral("player"), QVariant::fromValue(controller.get())}});
    engine->load(QUrl(QStringLiteral("qrc:/qt/qml/Uzume/App/Main.qml")));
    if (engine->rootObjects().isEmpty()) {
      return false;
    }
    window = qobject_cast<QQuickWindow*>(engine->rootObjects().first());
    return window != nullptr && QTest::qWaitForWindowExposed(window);
  }
  ~Harness() {
    engine.reset();
    controller.reset();
  }

  // Opens the game page (the Library column shows one game instead of the list); an empty id takes the selected game, `tab` is
  // "overview" | "saves" | "settings" | "manage". True once the page is shown.
  bool openGame(const QString& id = QString(), const QString& tab = QString()) {
    if (id.isEmpty()) QTest::qWaitFor([&]() { return !controller->selectedGameId().isEmpty(); }, 5000);
    controller->openGamePage(id, tab);
    return QTest::qWaitFor(
        [&]() {
          QQuickItem* page = item("gamePage");
          return page != nullptr && page->isVisible() && page->width() > 0 && !controller->selectedGameId().isEmpty();
        },
        5000);
  }

  // By objectName; falls back to the visual tree (Repeater/Loader delegates are not QObject children of the window).
  // Several items may share a name (Repeater delegates that are not yet deleted, per-layout copies): prefer one
  // that is visible, enabled and in a window; fall back to the first of any kind.
  QQuickItem* item(const char* objectName) const {
    if (window == nullptr) {
      return nullptr;
    }
    const QString name = QString::fromLatin1(objectName);
    QList<QQuickItem*> all = window->findChildren<QQuickItem*>(name);
    for (QQuickItem* c : items(objectName)) {
      if (!all.contains(c)) {
        all.append(c);
      }
    }
    for (QQuickItem* c : std::as_const(all)) {
      if (c->isVisible() && c->isEnabled() && c->window() == window) {
        return c;
      }
    }
    for (QQuickItem* c : std::as_const(all)) {
      if (c->isVisible() && c->window() == window) {
        return c;
      }
    }
    return all.isEmpty() ? nullptr : all.first();
  }
  static QQuickItem* findVisual(QQuickItem* root, const QString& name) {
    for (QQuickItem* c : root->childItems()) {
      if (c->objectName() == name) {
        return c;
      }
      if (auto* r = findVisual(c, name)) {
        return r;
      }
    }
    return nullptr;
  }
  static void collectVisual(QQuickItem* root, const QString& name, QList<QQuickItem*>& out) {
    for (QQuickItem* c : root->childItems()) {
      if (c->objectName() == name) {
        out.append(c);
      }
      collectVisual(c, name, out);
    }
  }
  QList<QQuickItem*> items(const char* objectName) const {
    QList<QQuickItem*> out;
    if (window != nullptr) {
      collectVisual(window->contentItem(), QString::fromLatin1(objectName), out);
    }
    return out;
  }
  // True if `it` is covered by an open UzDialog: a visible "dialogScrim" swallows clicks unless the target lives inside that
  // dialog (descendant of the scrim's parent) or is stacked above it (compare paint order below the common ancestor, e.g. a
  // popup parented to the window contentItem with a higher z).
  bool coveredByDialog(QQuickItem* it) const {
    QList<QQuickItem*> scrims = items("dialogScrim");
    for (QQuickItem* scrim : std::as_const(scrims)) {
      if (!scrim->isVisible() || scrim == it) continue;
      QQuickItem* dlg = scrim->parentItem();
      if (dlg != nullptr && (dlg == it || dlg->isAncestorOf(it))) continue;
      QQuickItem* common = scrim->parentItem();
      while (common != nullptr && !common->isAncestorOf(it)) common = common->parentItem();
      if (common == nullptr) continue;
      QQuickItem* bs = scrim;
      while (bs->parentItem() != common) bs = bs->parentItem();
      QQuickItem* bt = it;
      while (bt->parentItem() != common) bt = bt->parentItem();
      const QList<QQuickItem*> order = common->childItems();
      QList<QQuickItem*> paint;
      for (QQuickItem* c : order) paint.append(c);
      std::stable_sort(paint.begin(), paint.end(), [](QQuickItem* a, QQuickItem* b) { return a->z() < b->z(); });
      if (paint.indexOf(bt) < paint.indexOf(bs)) return true;
    }
    return false;
  }

  // Click for a control that appears as the result of an earlier click or state change (a popover opened by the previous
  // click instantiates its content one event-loop turn later): waits until an item of that name is visible and enabled, then
  // clicks. click() alone fails at once when the item is not there yet, which under load made tests flaky.
  bool clickWhenReady(const char* objectName, int timeoutMs = 8000) const {
    const bool ready = QTest::qWaitFor(
        [&]() {
          QQuickTest::qWaitForPolish(window);
          for (QQuickItem* c : items(objectName)) {
            if (c->isVisible() && c->isEnabled() && !coveredByDialog(c)) return true;
          }
          return false;
        },
        timeoutMs);
    return ready && click(objectName);
  }

  bool click(const char* objectName) const {
    // Let pending layout polish finish first: right after a state change a freshly shown item can still carry
    // stale geometry, and the click would land on its neighbor (e.g. Back instead of Request approval).
    QQuickTest::qWaitForPolish(window);
    QQuickItem* it = item(objectName);
    if (it == nullptr || !it->isVisible() || !it->isEnabled()) {
      it = nullptr;  // several items may share a name (e.g. per layout): take the first visible, enabled one
      for (QQuickItem* c : items(objectName)) {
        if (c->isVisible() && c->isEnabled()) {
          it = c;
          break;
        }
      }
    }
    if (it == nullptr) {
      return false;
    }
    if (coveredByDialog(it)) {
      qWarning("[uitest] click on '%s' lost: covered by an open dialog (its dialogScrim swallows the click); wait for the dialog to close",
               objectName);
      return false;
    }
    // Scroll every Flickable ancestor so that the item is inside its viewport (a panel whose content grew, or
    // whose text metrics differ per platform, may have the button below the fold).
    for (QQuickItem* a = it->parentItem(); a != nullptr; a = a->parentItem()) {
      const QVariant ch = a->property("contentHeight"), cy = a->property("contentY");
      if (!ch.isValid() || !cy.isValid() || a->property("contentItem").value<QQuickItem*>() == nullptr) {
        continue;
      }
      QQuickItem* content = a->property("contentItem").value<QQuickItem*>();
      const QPointF inContent = it->mapToItem(content, QPointF(0, 0));
      const qreal viewH = a->height();
      qreal y = cy.toReal();
      if (inContent.y() < y) {
        y = inContent.y() - 8;
      } else if (inContent.y() + it->height() > y + viewH) {
        y = inContent.y() + it->height() - viewH + 8;
      }
      const qreal maxY = std::max<qreal>(0, ch.toReal() - viewH);
      a->setProperty("contentY", std::clamp<qreal>(y, 0, maxY));
    }
    QQuickTest::qWaitForPolish(window);
    const QPointF p = it->mapToScene(QPointF(it->width() / 2, it->height() / 2));
    // Clip-aware visibility check: the point must be inside the window and inside the scene rect of every clipping
    // ancestor (e.g. a Flickable); otherwise the click would be lost, so fail explicitly with the geometry.
    // (childAt() is not usable as a "will it land" test: it also returns overlay items that ignore the mouse.)
    const QRectF sceneRect = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
    if (p.x() < 0 || p.y() < 0 || p.x() >= window->width() || p.y() >= window->height()) {
      qWarning("[uitest] click on '%s' lost: point (%.0f,%.0f) outside the window %dx%d (target scene rect %.0f,%.0f %.0fx%.0f)",
               objectName, p.x(), p.y(), window->width(), window->height(), sceneRect.x(), sceneRect.y(), sceneRect.width(),
               sceneRect.height());
      return false;
    }
    for (QQuickItem* a = it->parentItem(); a != nullptr; a = a->parentItem()) {
      if (!a->clip()) {
        continue;
      }
      const QRectF cr = a->mapRectToScene(QRectF(0, 0, a->width(), a->height()));
      if (!cr.contains(p)) {
        qWarning("[uitest] click on '%s' lost: point (%.0f,%.0f) outside clipping ancestor '%s' (%s, scene rect %.0f,%.0f %.0fx%.0f); "
                 "target scene rect %.0f,%.0f %.0fx%.0f",
                 objectName, p.x(), p.y(), qPrintable(a->objectName()), a->metaObject()->className(), cr.x(), cr.y(), cr.width(),
                 cr.height(), sceneRect.x(), sceneRect.y(), sceneRect.width(), sceneRect.height());
        return false;
      }
    }
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, p.toPoint());
    return true;
  }
};

}  // namespace uitest
