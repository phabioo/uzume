#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <optional>

namespace uzume {

// Tool packages from the server's tools catalog (ADR 0031 D3), e.g. the randomizer engine: <root>/<tool_id>/<version>/<platform>/
// holds the extracted release zip plus the marker file `.ok` (the SHA-256 of the zip, written last). root = <data>/cache/tools.
// A package counts only with the marker. Versions in use by a running variant build are protected from removal.
class ToolCache {
 public:
  enum class InstallResult { Ok, HashMismatch, ExtractFailed, LayoutInvalid, IoError, InvalidArgument };

  // What a runnable engine consists of (randomizer): the Java launcher, the jar and the folder the engine runs in.
  struct Layout {
    QString javaPath;
    QString jarName;  // UPR-FVX.jar
    QString workDir;  // the folder that contains the jar (the package root or its single top folder)
  };

  explicit ToolCache(const QString& root);

  const QString& root() const { return root_; }
  // "linux-x86_64" -> "linux-x64" and so on (the catalog may use either spelling); empty stays empty.
  static QString normalizePlatform(const QString& platform);
  static bool isValidId(const QString& s);  // [A-Za-z0-9._-]{1,64}, not "." / ".."

  QString packageDir(const QString& tool, const QString& version, const QString& platform) const;  // empty if invalid
  bool isReady(const QString& tool, const QString& version, const QString& platform) const;
  std::optional<Layout> layout(const QString& tool, const QString& version, const QString& platform) const;
  // Ready versions of a tool for this platform, newest first.
  QStringList versions(const QString& tool, const QString& platform) const;
  qint64 packageBytes(const QString& tool, const QString& version, const QString& platform) const;

  // Verifies the SHA-256 of `zipPath`, extracts it next to the final folder, checks the layout, renames and writes the marker.
  // On any failure nothing stays behind.
  InstallResult installZip(const QString& tool, const QString& version, const QString& platform, const QString& zipPath,
                           const QString& expectedSha256, QString* error = nullptr);

  // A build in progress holds its version: removeVersion() / pruneKeepNewest() skip it.
  void protect(const QString& tool, const QString& version);
  void unprotect(const QString& tool, const QString& version);
  bool isProtected(const QString& tool, const QString& version) const;
  bool removeVersion(const QString& tool, const QString& version, const QString& platform);  // false when protected or missing
  qint64 totalBytes() const;

 private:
  QString root_;
  QHash<QString, int> protect_;  // "tool/version" -> holders
};

}  // namespace uzume
