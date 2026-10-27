#pragma once

#include <QString>

namespace uzume {

// Extracts a .zip file (stored or deflate, no zip64, no encryption) into an empty or missing directory. Made for tool
// packages (randomizer engine: a jlink Java runtime of a few hundred files, up to a few hundred MB). Names are checked like
// in the plugin reader (relative, no "..", no drive letter, no backslash); every file is written below `destDir` only.
// Unix modes of the archive are applied (at least the owner execute bit); names below a `bin` folder get the execute bit
// even when the archive has no modes. Symbolic links are created only when the target is a relative path that stays
// inside `destDir`; anything else is refused. Limits: kMaxFiles entries, kMaxFileBytes per file, kMaxTotalBytes in total.
struct ZipExtractLimits {
  int maxFiles = 20000;
  qint64 maxFileBytes = 1024ll * 1024 * 1024;
  qint64 maxTotalBytes = 3ll * 1024 * 1024 * 1024;
};

// false + *error (English, no file contents) on any problem; the caller removes `destDir` then.
bool extractZip(const QString& zipPath, const QString& destDir, QString* error = nullptr, const ZipExtractLimits& limits = {});

}  // namespace uzume
