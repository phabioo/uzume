#include "toolcache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

#include "corecache.h"
#include "zipextract.h"

namespace uzume {

namespace {
constexpr const char* kMarker = ".ok";
constexpr const char* kEngineJar = "UPR-FVX.jar";

QString sha256OfFile(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash h(QCryptographicHash::Sha256);
  if (!h.addData(&f)) return {};
  return QString::fromLatin1(h.result().toHex());
}

QString javaIn(const QString& dir) {
#ifdef Q_OS_WIN
  const QString j = dir + QStringLiteral("/java/bin/java.exe");
#else
  const QString j = dir + QStringLiteral("/java/bin/java");
#endif
  return QFileInfo(j).isFile() ? j : QString();
}

// Finds the folder with the jar and the runtime: the package root or its single top-level folder.
std::optional<ToolCache::Layout> findLayout(const QString& pkgDir) {
  const auto at = [&](const QString& dir) -> std::optional<ToolCache::Layout> {
    if (!QFileInfo(dir + QLatin1Char('/') + QLatin1String(kEngineJar)).isFile()) return std::nullopt;
    const QString java = javaIn(dir);
    if (java.isEmpty()) return std::nullopt;
    return ToolCache::Layout{java, QString::fromLatin1(kEngineJar), dir};
  };
  if (auto l = at(pkgDir)) return l;
  const QFileInfoList subs = QDir(pkgDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  if (subs.size() == 1) return at(subs.first().absoluteFilePath());
  return std::nullopt;
}
}  // namespace

ToolCache::ToolCache(const QString& root) : root_(QDir::cleanPath(root)) {}

QString ToolCache::normalizePlatform(const QString& platform) {
  QString p = platform;
  p.replace(QLatin1String("x86_64"), QLatin1String("x64")).replace(QLatin1String("aarch64"), QLatin1String("arm64"));
  return p;
}

bool ToolCache::isValidId(const QString& s) {
  static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
  return re.match(s).hasMatch() && s != QLatin1String(".") && s != QLatin1String("..");
}

QString ToolCache::packageDir(const QString& tool, const QString& version, const QString& platform) const {
  if (root_.isEmpty() || !isValidId(tool) || !isValidId(version) || !isValidId(platform)) return {};
  return root_ + QLatin1Char('/') + tool + QLatin1Char('/') + version + QLatin1Char('/') + platform;
}

bool ToolCache::isReady(const QString& tool, const QString& version, const QString& platform) const {
  const QString d = packageDir(tool, version, platform);
  return !d.isEmpty() && QFileInfo(d + QLatin1Char('/') + QLatin1String(kMarker)).isFile();
}

std::optional<ToolCache::Layout> ToolCache::layout(const QString& tool, const QString& version, const QString& platform) const {
  if (!isReady(tool, version, platform)) return std::nullopt;
  return findLayout(packageDir(tool, version, platform));
}

QStringList ToolCache::versions(const QString& tool, const QString& platform) const {
  QStringList out;
  if (root_.isEmpty() || !isValidId(tool)) return out;
  const QFileInfoList dirs = QDir(root_ + QLatin1Char('/') + tool).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QFileInfo& d : dirs) {
    if (d.fileName().startsWith(QLatin1Char('.'))) continue;
    if (isReady(tool, d.fileName(), platform)) out.append(d.fileName());
  }
  std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) { return CoreCache::compareVersions(a, b) > 0; });
  return out;
}

qint64 ToolCache::packageBytes(const QString& tool, const QString& version, const QString& platform) const {
  const QString d = packageDir(tool, version, platform);
  if (d.isEmpty()) return 0;
  qint64 n = 0;
  QDirIterator it(d, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    n += it.fileInfo().size();
  }
  return n;
}

ToolCache::InstallResult ToolCache::installZip(const QString& tool, const QString& version, const QString& platform, const QString& zipPath,
                                               const QString& expectedSha256, QString* error) {
  const auto fail = [&](InstallResult r, const QString& why) {
    if (error) *error = why;
    return r;
  };
  const QString dest = packageDir(tool, version, platform);
  if (dest.isEmpty()) return fail(InstallResult::InvalidArgument, QStringLiteral("invalid tool, version or platform"));
  const QString actual = sha256OfFile(zipPath);
  if (actual.isEmpty()) return fail(InstallResult::IoError, QStringLiteral("cannot read the downloaded file"));
  if (actual.compare(expectedSha256, Qt::CaseInsensitive) != 0) return fail(InstallResult::HashMismatch, QStringLiteral("the download does not match the checksum of the catalog"));
  const QString tmp = dest + QStringLiteral(".tmp");
  QDir(tmp).removeRecursively();
  QString err;
  if (!extractZip(zipPath, tmp, &err)) {
    QDir(tmp).removeRecursively();
    return fail(InstallResult::ExtractFailed, err);
  }
  if (!findLayout(tmp)) {
    QDir(tmp).removeRecursively();
    return fail(InstallResult::LayoutInvalid, QStringLiteral("the package has no engine jar or Java runtime"));
  }
  QFile marker(tmp + QLatin1Char('/') + QLatin1String(kMarker));
  if (!marker.open(QIODevice::WriteOnly) || marker.write(actual.toLatin1()) < 0) {
    QDir(tmp).removeRecursively();
    return fail(InstallResult::IoError, QStringLiteral("cannot write the cache"));
  }
  marker.close();
  QDir(dest).removeRecursively();
  if (!QDir().rename(tmp, dest)) {
    QDir(tmp).removeRecursively();
    return fail(InstallResult::IoError, QStringLiteral("cannot move the package into the cache"));
  }
  return InstallResult::Ok;
}

void ToolCache::protect(const QString& tool, const QString& version) { ++protect_[tool + QLatin1Char('/') + version]; }

void ToolCache::unprotect(const QString& tool, const QString& version) {
  const QString k = tool + QLatin1Char('/') + version;
  auto it = protect_.find(k);
  if (it == protect_.end()) return;
  if (--it.value() <= 0) protect_.erase(it);
}

bool ToolCache::isProtected(const QString& tool, const QString& version) const { return protect_.value(tool + QLatin1Char('/') + version) > 0; }

bool ToolCache::removeVersion(const QString& tool, const QString& version, const QString& platform) {
  if (isProtected(tool, version)) return false;
  const QString d = packageDir(tool, version, platform);
  if (d.isEmpty() || !QFileInfo(d).isDir()) return false;
  return QDir(d).removeRecursively();
}

qint64 ToolCache::totalBytes() const {
  if (root_.isEmpty()) return 0;
  qint64 n = 0;
  QDirIterator it(root_, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    n += it.fileInfo().size();
  }
  return n;
}

}  // namespace uzume
