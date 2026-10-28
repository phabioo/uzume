#include "installroot.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace uzume::update {

QString launcherFileName(const QString& installRoot) {
  if (installRoot.isEmpty()) return {};
  const QDir d(installRoot);
  return d.exists(QLatin1String(kPlayerExeName)) ? QString::fromLatin1(kPlayerExeName) : QString();
}

QString installRootFor(const QString& exeDir) {
  if (exeDir.isEmpty()) return exeDir;
  const QDir d(exeDir);
  if (d.dirName().compare(QLatin1String("bin"), Qt::CaseInsensitive) == 0) {
    QDir parent = d;
    if (parent.cdUp() && !launcherFileName(parent.absolutePath()).isEmpty()) {
      return parent.absolutePath();
    }
  }
  return d.absolutePath();
}

bool hasMsiMarker(const QString& installRoot) {
  return !installRoot.isEmpty() && QFileInfo::exists(QDir(installRoot).filePath(QLatin1String(kPlayerMsiMarker)));
}

bool canSelfUpdate(const QString& installRoot) { return hasMsiMarker(installRoot); }

bool hubInstalledNear(const QString& installRoot) {
  if (installRoot.isEmpty()) return false;
  QDir d(installRoot);
  if (!d.cdUp()) return false;
  return QFileInfo::exists(d.filePath(QStringLiteral("Server/") + QLatin1String(kHubMsiMarker)));
}

bool isMsiFile(const QString& path) { return path.endsWith(QLatin1String(".msi"), Qt::CaseInsensitive); }

bool directoryWritable(const QString& dir) {
  if (dir.isEmpty() || !QFileInfo(dir).isDir()) return false;
  const QString probe = QDir(dir).filePath(QStringLiteral(".fbwrite-") + QUuid::createUuid().toString(QUuid::Id128));
  QFile f(probe);
  if (!f.open(QIODevice::WriteOnly)) return false;
  f.close();
  return f.remove();
}

MsiScope detectMsiScope(bool rootWritable, bool hubInstalled) {
  if (hubInstalled) return MsiScope::MachineWithHub;
  return rootWritable ? MsiScope::User : MsiScope::Machine;
}

QString msiScopeName(MsiScope s) {
  switch (s) {
    case MsiScope::User: return QStringLiteral("user");
    case MsiScope::Machine: return QStringLiteral("machine");
    case MsiScope::MachineWithHub: return QStringLiteral("machine-hub");
    case MsiScope::SetupHub: return QStringLiteral("setup-hub");
    case MsiScope::ShareHub: return QStringLiteral("share-hub");
  }
  return QStringLiteral("user");
}

bool msiScopeNeedsElevation(MsiScope s) { return s != MsiScope::User; }

// msiexec and the launcher need backslash paths (forward slashes give error 1619); explicit so it also works on Linux.
static QString withBackslashes(QString path) { return path.replace(QLatin1Char('/'), QLatin1Char('\\')); }

QStringList msiexecArguments(const QString& msiPath, MsiScope scope, const QStringList& properties) {
  QStringList a{QStringLiteral("/i"), withBackslashes(msiPath), QStringLiteral("/qn"), QStringLiteral("/norestart")};
  switch (scope) {
    case MsiScope::User:
      a << QStringLiteral("MSIINSTALLPERUSER=1") << QStringLiteral("ALLUSERS=2");
      break;
    case MsiScope::Machine:
      a << QStringLiteral("ALLUSERS=1");
      break;
    case MsiScope::MachineWithHub:
      a << QStringLiteral("ALLUSERS=1") << QStringLiteral("INSTALL_HUB=1");
      break;
    case MsiScope::SetupHub:
      a << QStringLiteral("ALLUSERS=1") << QStringLiteral("INSTALL_HUB=1") << QStringLiteral("NETWORK_SHARING=0");
      break;
    case MsiScope::ShareHub:
      a << QStringLiteral("ALLUSERS=1") << QStringLiteral("INSTALL_HUB=1") << properties;
      break;
  }
  return a;
}

QString copyLauncherToTemp(const QString& installRoot, const QString& tempBase, QString* error) {
  const auto fail = [error](const QString& why) {
    if (error != nullptr) *error = why;
    return QString();
  };
  const QString launcher = launcherFileName(installRoot);
  if (launcher.isEmpty()) return fail(QStringLiteral("launcher not found next to the app"));
  const QString src = QDir(installRoot).filePath(launcher);
  const QString dir = QDir(tempBase).filePath(QStringLiteral("uzume-update-") + QUuid::createUuid().toString(QUuid::Id128));
  if (!QDir().mkpath(dir)) return fail(QStringLiteral("temporary folder not creatable"));
  const QString dst = QDir(dir).filePath(launcher);
  if (!QFile::copy(src, dst)) return fail(QStringLiteral("launcher not copyable"));
  return dst;
}

InstallerCommand msiRelaunchCommand(const QString& launcherCopy, const QString& msiPath, MsiScope scope,
                                    const QString& relaunchExe, const QStringList& relaunchArgs, const QString& sha256, qint64 size,
                                    const QStringList& msiProperties) {
  InstallerCommand c;
  c.program = launcherCopy;
  c.args = {QStringLiteral("--apply-msi-update")};
  if (!sha256.isEmpty()) c.args << QStringLiteral("--sha256") << sha256;
  if (size >= 0) c.args << QStringLiteral("--size") << QString::number(size);
  for (const QString& p : msiProperties) c.args << QStringLiteral("--msi-property") << p;
  c.args << withBackslashes(msiPath) << msiScopeName(scope) << withBackslashes(relaunchExe);
  c.args += relaunchArgs;
  return c;
}

QString perMachinePlayerExe(const QString& programFilesDir) {
  return QDir(programFilesDir).filePath(QStringLiteral("Uzume/App/") + QLatin1String(kPlayerExeName));
}

// ---- Linux AppImage

QString appImagePath() {
  const QString p = qEnvironmentVariable("APPIMAGE");
  if (p.isEmpty()) return {};
  const QFileInfo fi(p);
  return fi.isFile() ? fi.absoluteFilePath() : QString();
}

QString appImageUnsupportedReason(const QString& appImage) {
  if (appImage.isEmpty() || !QFileInfo(appImage).isFile()) {
    return QStringLiteral("Automatic installation is only available for the Windows installer and the Linux AppImage.");
  }
  if (!directoryWritable(QFileInfo(appImage).absolutePath())) return QStringLiteral("The folder of the AppImage is not writable.");
  return {};
}

QStringList appImageRelaunchArguments(const QStringList& args) {
  static const QStringList oneShot{QStringLiteral("--smoke-test"), QStringLiteral("--setup-local-hub"), QStringLiteral("--mcp")};
  QStringList out;
  for (const QString& a : args) {
    if (oneShot.contains(a) || a.startsWith(QLatin1String("uzume://"))) continue;
    out << a;
  }
  return out;
}

void appImageConfirmStart(const QString& appImage) {
  if (!appImage.isEmpty()) QFile::remove(appImageStartMarker(appImage));
}

bool replaceAppImage(const QString& staged, const QString& appImage, const QString& sha256, qint64 size, QString* error) {
  const auto fail = [error](const QString& why) {
    if (error != nullptr) *error = why;
    return false;
  };
#ifdef Q_OS_UNIX
  const QFileInfo target(appImage);
  if (!target.isFile()) return fail(QStringLiteral("the AppImage is not a regular file"));
  const QString dir = target.absolutePath();
  const QString part = QDir(dir).filePath(QLatin1Char('.') + target.fileName() + QStringLiteral(".part"));
  QFile::remove(part);
  const auto failPart = [&](const QString& why) {
    QFile::remove(part);
    return fail(why);
  };
  {
    QFile in(staged);
    QFile out(part);
    if (!in.open(QIODevice::ReadOnly)) return fail(QStringLiteral("staged update not readable"));
    if (!out.open(QIODevice::WriteOnly)) return fail(QStringLiteral("update file not writable next to the AppImage"));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (!in.atEnd()) {
      const QByteArray chunk = in.read(1 << 20);
      if (chunk.isEmpty()) break;
      hash.addData(chunk);
      total += chunk.size();
      if (out.write(chunk) != chunk.size()) {
        out.close();
        return failPart(QStringLiteral("update file could not be written (disk full?)"));
      }
    }
    if (!out.flush() || ::fsync(out.handle()) != 0) {
      out.close();
      return failPart(QStringLiteral("update file could not be flushed to disk"));
    }
    out.close();
    if ((size >= 0 && total != size) ||
        (!sha256.isEmpty() && QString::fromLatin1(hash.result().toHex()) != sha256.toLower())) {
      return failPart(QStringLiteral("staged update does not match the signed index"));
    }
  }
  // The new image keeps the permission bits of the old one (a user may have restricted them); owner-executable is a must.
  const QFile::Permissions perms = target.permissions() | QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
  if (!QFile::setPermissions(part, perms)) return failPart(QStringLiteral("update file could not be made executable"));
  const QString prev = appImagePreviousPath(appImage);
  QFile::remove(prev);
  const QByteArray appBytes = QFile::encodeName(appImage);
  if (::link(appBytes.constData(), QFile::encodeName(prev).constData()) != 0 && !QFile::copy(appImage, prev)) {
    return failPart(QStringLiteral("the current AppImage could not be kept as .previous"));
  }
  if (::rename(QFile::encodeName(part).constData(), appBytes.constData()) != 0) {
    return failPart(QStringLiteral("the new AppImage could not be put in place"));
  }
  const int dfd = ::open(QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY);  // make the rename durable (best effort)
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return true;
#else
  Q_UNUSED(staged) Q_UNUSED(appImage) Q_UNUSED(sha256) Q_UNUSED(size)
  return fail(QStringLiteral("AppImage updates exist on Linux only"));
#endif
}

InstallerCommand appImageRelaunchCommand(const QString& appImage, qint64 oldPid, const QStringList& args) {
  static const char* kWatcher =
      "app=$1; prev=$2; marker=$3; oldpid=$4; shift 4\n"
      "i=0; while kill -0 \"$oldpid\" 2>/dev/null && [ \"$i\" -lt 1500 ]; do sleep 0.2; i=$((i+1)); done\n"
      "unset APPDIR ARGV0 OWD APPIMAGE\n"
      ": > \"$marker\"\n"
      "\"$app\" \"$@\" &\n"
      "pid=$!\n"
      "wait \"$pid\"; rc=$?\n"
      "if [ \"$rc\" -ne 0 ] && [ -e \"$marker\" ]; then\n"  // crashed or failed before confirming: roll back
      "  rm -f \"$marker\"\n"
      "  if [ -f \"$prev\" ] && mv -f \"$prev\" \"$app\"; then exec \"$app\" \"$@\"; fi\n"
      "fi\n"
      "rm -f \"$marker\"\n"
      "exit \"$rc\"\n";
  InstallerCommand c;
  c.program = QStringLiteral("/bin/sh");
  c.args = {QStringLiteral("-c"), QString::fromLatin1(kWatcher), QStringLiteral("uzume-update"), appImage, appImagePreviousPath(appImage),
            appImageStartMarker(appImage), QString::number(oldPid)};
  c.args += args;
  return c;
}

}  // namespace uzume::update
