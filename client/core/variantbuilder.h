#pragma once

#include <QObject>
#include <QStringList>
#include <atomic>
#include <memory>

#include "enginerunner.h"
#include "toolcache.h"
#include "variantcache.h"

namespace uzume {

struct BuildProgress {
  QString step;         // "reading" | "randomizing" | "writing"
  double fraction = 0;  // 0..1 of the whole build, an estimate
  qint64 elapsedMs = 0;
  qint64 etaMs = 0;
};

struct BuildOutcome {
  bool ok = false;
  // Empty when ok. engine_missing | base_missing | base_mismatch | not_enough_memory | disk_full | engine_failed | timed_out |
  // cancelled | output_missing | io_error
  QString error;
  QString engineError;       // see EngineResult
  QStringList log;           // lines without game data ("HH:mm:ss text"), see EngineRunner
  int exitCode = -1;
  qint64 neededBytes = 0;    // not_enough_memory, disk_full
  qint64 freeBytes = 0;
  QString expectedBaseSha;   // base_mismatch: what the recipe pins ...
  QString actualBaseSha;     // ... and what the file on this PC is
  QString layoutNote;        // 3DS: where the engine put the mod folder (names of folders only)
};

// Builds one variant from its recipe: checks the base file against the recipe's pin (Gen 1 to 5), runs the engine into a temp folder
// in the variant cache, and moves the result into the cache atomically; on any failure nothing stays behind and the base file is
// never written. The engine version of the recipe is protected in the tool cache while it runs.
class VariantBuilder : public QObject {
  Q_OBJECT
 public:
  VariantBuilder(ToolCache* tools, VariantCache* variants, QString platform, QObject* parent = nullptr);
  ~VariantBuilder() override;

  bool running() const { return running_; }
  EngineRunner* runner() { return &runner_; }
  // `skipBaseCheck`: build although the base file's SHA-256 differs from (or is not pinned in) the recipe ("Build anyway").
  void start(const VariantRecipe& recipe, const QString& baseFile, bool skipBaseCheck = false);
  void cancel();
  // Tests: job limits.
  void setTimeLimitMs(int ms) { timeLimitMs_ = ms; }

 signals:
  void progress(const uzume::BuildProgress& p);
  void finished(const uzume::BuildOutcome& o);

 private:
  void runEngine();
  void onEngineFinished(const EngineResult& r);
  void done(BuildOutcome o);
  QString findModRoot(const QString& dir, int depth) const;

  ToolCache* tools_;
  VariantCache* variants_;
  QString platform_;
  EngineRunner runner_;
  VariantRecipe recipe_;
  QString baseFile_;
  qint64 baseBytes_ = 0;
  ToolCache::Layout layout_;
  bool running_ = false;
  bool cancelled_ = false;
  bool toolHeld_ = false;
  int timeLimitMs_ = 0;
  std::shared_ptr<std::atomic_bool> cancelFlag_;
  quint64 gen_ = 0;
};

}  // namespace uzume

Q_DECLARE_METATYPE(uzume::BuildProgress)
Q_DECLARE_METATYPE(uzume::BuildOutcome)
