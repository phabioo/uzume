#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <QtQuickControls2/QQuickStyle>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#endif
#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "diagbundle.h"
#include "linkhandler.h"
#include "mcp_tools.h"
#include "filelog.h"
#include "pluginimageprovider.h"
#include "profilestore.h"
#include "playercontroller.h"
#include "uifonts.h"
#include "version.h"

namespace {

std::atomic<int> g_qmlWarnings{0};
QtMessageHandler g_previousHandler = nullptr;

// Smoke test: count QML warnings while loading (failure), print everything else unchanged.
void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
  if (type == QtWarningMsg || type == QtCriticalMsg) {
    const bool qml = msg.contains(QLatin1String(".qml")) || msg.contains(QLatin1String("qrc:/")) ||
                     (ctx.category != nullptr && QByteArrayView(ctx.category).startsWith("qt.qml"));
    if (qml) {
      ++g_qmlWarnings;
    }
  }
  if (g_previousHandler != nullptr) {
    g_previousHandler(type, ctx, msg);
  }
}

// `--version-json`: one JSON line on stdout, before any Qt/GUI initialisation. On Windows the line goes to the
// inherited stdout handle (CI redirects it to a file) and falls back to the parent console.
void printToStdout(const std::string& text) {
#ifdef Q_OS_WIN
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h == nullptr || h == INVALID_HANDLE_VALUE) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
  }
  if (h != nullptr && h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    FlushFileBuffers(h);  // fails harmlessly on consoles
    return;
  }
#endif
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

#ifdef Q_OS_UNIX
// SIGTERM / SIGHUP / SIGINT (kill, systemd stop, logout, Ctrl+C) take the normal quit path, so a running game's save is
// flushed and synced like after "Quit". The handler only writes a byte into a socket pair (async-signal-safe); a
// QSocketNotifier turns it into QCoreApplication::quit() on the GUI thread. Windows handles logoff through Qt.
int g_signalFd[2] = {-1, -1};

void onTerminationSignal(int) {
  const char byte = 1;
  const ssize_t ignored = ::write(g_signalFd[0], &byte, 1);
  (void)ignored;
}

void installTerminationSignals(QObject* parent) {
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, g_signalFd) != 0) {
    return;
  }
  struct sigaction sa;
  sa.sa_handler = onTerminationSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  for (const int sig : {SIGTERM, SIGHUP, SIGINT}) {
    sigaction(sig, &sa, nullptr);
  }
  auto* notifier = new QSocketNotifier(g_signalFd[1], QSocketNotifier::Read, parent);
  QObject::connect(notifier, &QSocketNotifier::activated, parent, [notifier]() {
    char byte = 0;
    const ssize_t ignored = ::read(g_signalFd[1], &byte, 1);
    (void)ignored;
    notifier->setEnabled(false);  // one request is enough; further signals during shutdown are ignored
    qInfo() << "Termination signal received, quitting";
    QCoreApplication::quit();
  });
}
#endif

#ifdef Q_OS_WIN
// Named mutex: every Player instance holds a handle; the installer (/UPDATE) waits until no process holds it.
// Instances are not limited to one.
HANDLE g_instanceMutex = nullptr;
void holdInstanceMutex() { g_instanceMutex = CreateMutexW(nullptr, FALSE, L"UzumeApp"); }
#endif

// `--collect-logs <out.zip> [--data-dir <path>]`: writes the diagnostics bundle without the GUI (also when the Player
// does not start). Offline: reads the data directory only, never connects to a Hub. Refuses to overwrite a file.
int collectLogsCli(int argc, char* argv[], const QString& out) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("Uzume"));
  QCoreApplication::setApplicationName(QStringLiteral("Uzume"));
  QString dataDir;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--data-dir") == 0) dataDir = QString::fromLocal8Bit(argv[i + 1]);
  }
  if (dataDir.isEmpty()) dataDir = uzume::ProfileStore::defaultBaseDir();
  if (QFileInfo::exists(out)) {
    printToStdout("uzume: " + out.toStdString() + " already exists, not overwriting\n");
    return 1;
  }
  QString err;
  if (!uzume::diag::writeBundle(out, uzume::diag::offlineInfo(dataDir), dataDir, {}, &err)) {
    printToStdout("uzume: could not write the bundle: " + err.toStdString() + "\n");
    return 1;
  }
  printToStdout("Diagnostics bundle written to " + QDir::toNativeSeparators(out).toStdString() + "\n");
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  // Info options are answered here, before QGuiApplication, the mutex, updates and the data directory.
  // QCommandLineParser must not see --version/--help: on Windows a GUI application shows them in a message box
  // that blocks forever when nobody clicks (CI package smoke check).
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version-json") == 0) {
      printToStdout(uzume::playerVersionJson() + "\n");
      return 0;
    }
    if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
      printToStdout("Uzume " + std::string(uzume::playerVersion()) + "\n");
      return 0;
    }
    if (std::strcmp(argv[i], "--collect-logs") == 0) {
      if (i + 1 >= argc) {
        printToStdout("uzume: --collect-logs needs an output file (<out.zip>)\n");
        return 2;
      }
      return collectLogsCli(argc, argv, QString::fromLocal8Bit(argv[i + 1]));
    }
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "-?") == 0) {
      printToStdout(
          "Usage: uzume [options] [uzume://join/<server>/<invite>#<security code> | uzume://run/<server>/<run id>/recap]\n"
          "  --version            Print the version and exit.\n"
          "  --version-json       Print version, channel, commit and protocol versions as JSON and exit.\n"
          "  --smoke-test         Load QML and exit with code 0 (code 1 on QML warnings).\n"
          "  --collect-logs <out.zip>  Write the diagnostics bundle (logs, system info; no passwords or tokens) and exit.\n"
          "  --data-dir <path>    Data directory (profiles, cache, saves) instead of the default.\n"
          "  --mcp                Allow Claude to control this app (debug MCP server on 127.0.0.1) for this start only.\n"
          "  --mcp-port <n>       Port of the debug MCP server for this start (default: the setting, 47615).\n"
          "                       With --mcp, the token UZUME_MCP_TOKEN (if set) replaces the stored one for this start.\n"
          "  --dev-allow-http     Development only: also allow HTTP hubs outside localhost.\n"
          "  --setup-local-hub    Continue \"Set up a server on this PC\" (the installer starts the app with it).\n");
      return 0;
    }
  }
#ifdef Q_OS_WIN
  SetCurrentProcessExplicitAppUserModelID(L"Uzume.App");  // same id as the installer shortcuts
#endif
  QGuiApplication app(argc, argv);
  QGuiApplication::setOrganizationName(QStringLiteral("Uzume"));
  QGuiApplication::setApplicationName(QStringLiteral("Uzume"));
  uzume::ui::loadDesignFonts();   // IBM Plex Sans / Mono (design system), application font = Plex Sans
#ifdef Q_OS_UNIX
  installTerminationSignals(&app);
#endif
  {
    QIcon icon;
    for (const int s : {16, 24, 32, 48, 64, 128, 256, 512}) {
      icon.addFile(QStringLiteral(":/uzume/app/uzume-%1.png").arg(s), QSize(s, s));
    }
    QGuiApplication::setWindowIcon(icon);
  }
  QGuiApplication::setApplicationVersion(QString::fromUtf8(uzume::playerVersion().data(),
                                                           static_cast<qsizetype>(uzume::playerVersion().size())));

  QCommandLineParser parser;
  const QCommandLineOption smoke(QStringLiteral("smoke-test"),
                                 QStringLiteral("Load QML and exit with code 0 (code 1 on QML warnings)."));
  const QCommandLineOption dataDir(QStringLiteral("data-dir"),
                                   QStringLiteral("Data directory (profiles, cache, saves) instead of AppData."),
                                   QStringLiteral("path"));
  const QCommandLineOption devHttp(QStringLiteral("dev-allow-http"),
                                   QStringLiteral("Development only: also allow HTTP hubs outside localhost."));
  const QCommandLineOption setupLocalHub(QStringLiteral("setup-local-hub"),
                                         QStringLiteral("Continue \"Set up a server on this PC\" after the server was installed."));
  const QCommandLineOption mcpOpt(QStringLiteral("mcp"), QStringLiteral("Debug MCP server on 127.0.0.1 for this start only (ADR 0032)."));
  const QCommandLineOption mcpPortOpt(QStringLiteral("mcp-port"), QStringLiteral("Port of the debug MCP server for this start only."),
                                       QStringLiteral("port"));
  parser.addOption(mcpOpt);
  parser.addOption(mcpPortOpt);
  parser.addOption(setupLocalHub);
  parser.addOption(smoke);
  parser.addOption(dataDir);
  parser.addOption(devHttp);
  parser.addPositionalArgument(QStringLiteral("link"), QStringLiteral("An uzume:// invite link to open."), QStringLiteral("[link]"));
  parser.process(app);
  const bool smokeMode = parser.isSet(smoke);
  const QString startLink = uzume::app::findLinkArgument(parser.positionalArguments());
  // One running app per user and data directory: a second start hands its link over and exits. Not in smoke mode.
  uzume::app::SingleInstance instance(uzume::app::instanceServerName(
      QString::fromLocal8Bit(qgetenv("USERNAME").isEmpty() ? qgetenv("USER") : qgetenv("USERNAME")), parser.value(dataDir)));
  if (!smokeMode) {
    if (!instance.acquire(startLink)) {
      return 0;
    }
    uzume::app::ensureSchemeRegistered(QCoreApplication::applicationFilePath());  // Windows only; no-op elsewhere
  }
#ifdef Q_OS_WIN
  if (!smokeMode) {
    holdInstanceMutex();
  }
#endif

  QTemporaryDir smokeDir;  // smoke test without --data-dir does not touch real user data
  uzume::ui::PlayerController::Options opts;
  opts.dataDir = parser.value(dataDir);
  opts.allowHttp = parser.isSet(devHttp);
  opts.enableUpdates = !smokeMode;
  if (smokeMode) {
    if (opts.dataDir.isEmpty() && smokeDir.isValid()) {
      opts.dataDir = smokeDir.path();
    }
    opts.memoryCredentials = true;
    opts.probeCoreVersions = false;
    g_previousHandler = qInstallMessageHandler(messageHandler);
  }

  QQuickStyle::setStyle(QStringLiteral("Basic"));
  // Log file <data>/logs/player.log (the GUI exe has no console). Lines before the data directory is known are
  // buffered. Not in smoke mode (temporary data directory).
  if (!smokeMode) {
    uzume::filelog::install();
  }
  uzume::ui::PlayerController controller(opts);
  if (!smokeMode) {
    uzume::filelog::setDirectory(controller.profileStore()->baseDir());
  }
  // Beta channel + automatic install: a verified staged installer from the last run is applied before the
  // main window appears (never during a game: none can be running yet). The installer waits for this process.
  if (!smokeMode && controller.updates()->manager()->applyStagedAtStart()) {
    return 0;
  }

  QQmlApplicationEngine engine;
  engine.addImportPath(QStringLiteral("qrc:/qt/qml"));
  engine.addImageProvider(QStringLiteral("fbplugin"), new uzume::ui::PluginImageProvider(controller.plugins()->imageSources()));
  engine.addImageProvider(QStringLiteral("screenshots"), controller.createScreenshotImageProvider());
  engine.setInitialProperties({{QStringLiteral("player"), QVariant::fromValue(&controller)}});
  engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Uzume/App/Main.qml")));
  if (engine.rootObjects().isEmpty()) {
    return 1;
  }
  if (smokeMode) {
    QTimer::singleShot(400, &app, &QCoreApplication::quit);  // wait briefly: polish/bindings only run in the event loop
    const int rc = app.exec();
    return g_qmlWarnings.load() > 0 ? 1 : rc;
  }
  controller.mcp()->setToolInstaller([&controller](uzume::mcp::McpToolRegistry& r) { uzume::mcp::installTools(r, controller); });
  if (parser.isSet(mcpPortOpt)) {
    bool portOk = false;
    const int port = parser.value(mcpPortOpt).toInt(&portOk);
    if (!portOk || !controller.mcp()->setPortForThisStart(port)) {
      printToStdout("uzume: --mcp-port must be a number from 1024 to 65535\n");
      return 2;
    }
  }
  if (parser.isSet(mcpOpt)) {
    const QByteArray envToken = uzume::ui::McpController::tokenFromEnvironment(true);  // only together with --mcp
    if (!envToken.isEmpty() && !controller.mcp()->setTokenForThisStart(envToken)) {
      printToStdout("uzume: UZUME_MCP_TOKEN ignored (needs 32 to 256 characters of A-Z a-z 0-9 - _)\n");
    }
    controller.mcp()->enableForThisStart();
  }
  controller.startup();
  QObject::connect(&instance, &uzume::app::SingleInstance::activateRequested, &app, [] {
    for (QWindow* w : QGuiApplication::topLevelWindows()) {
      if (w->windowStates() & Qt::WindowMinimized) w->showNormal();
      w->raise();
      w->requestActivate();
    }
  });
  QObject::connect(&instance, &uzume::app::SingleInstance::linkReceived, &app, [&controller](const QString& link) {
    if (!link.isEmpty() && !controller.openRunLink(link)) controller.onboarding()->openLink(link);
  });
  if (!startLink.isEmpty()) {
    QTimer::singleShot(0, &app, [&controller, startLink] { if (!controller.openRunLink(startLink)) controller.onboarding()->openLink(startLink); });
  }
  if (parser.isSet(setupLocalHub)) {
    controller.localHub()->startAfterInstall();  // after the MSI installed the Hub: connect, create the admin, pair
  }
  return app.exec();
}
