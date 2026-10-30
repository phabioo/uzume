#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <functional>

namespace uzume {

// One run of the randomizer engine (ADR 0031 D3): `<java> -Xmx<heap>M -jar <jar> cli -i <base> -o <out> -S <settings> -z <seed>
// [-d] [-l]` as a child process with a fixed argument list and no shell, in the engine's folder. Exit code 0 is success. Progress
// For a 3DS (mod) job the call is `<java> -Xmx<heap>M -cp <jar><sep>uzume-fvx-launcher.jar UzumeFvxLauncher cli ...` when the launcher is
// compiled in (UZUME_HAVE_FVX_LAUNCHER, issue #414): the FVX CLI cannot open 3DS games without it. Progress
// is an estimate from the size of the base file (the engine reports none). Only the engine's own ERROR / WARNING lines are kept
// (at most the last 20, each cut to 200 characters); everything else it prints (it may carry game data) is dropped, and stderr
// is dropped altogether. Error codes (EngineResult::error): engine_missing, base_missing, not_enough_memory, disk_full,
// engine_failed, timed_out, cancelled, output_missing.
struct EngineJob {
  QString javaPath;
  QString workDir;      // the folder with the jar
  QString jarName = QStringLiteral("UPR-FVX.jar");
  QString baseFile;     // -i
  QString outPath;      // -o: a file (GB to NDS) or, with directoryOutput, a folder (3DS LayeredFS)
  QString settings;     // -S
  QString seed;         // -z (decimal integer text)
  bool directoryOutput = false;  // -d
  bool spoilerLog = false;       // -l: the engine writes <outPath>.log
  int heapMb = 1024;
  int timeLimitMs = 3 * 60 * 1000;
  qint64 baseBytes = 0;          // for the estimate and the disk check
  bool isMod = false;            // 3DS: other estimate and disk need
};

struct EngineProgress {
  QString step;          // "randomizing" | "writing"
  double fraction = 0;   // 0..1, an estimate
  qint64 elapsedMs = 0;
  qint64 etaMs = 0;      // estimated time left, 0 = unknown / overdue
};

struct EngineResult {
  bool ok = false;
  QString error;                 // empty when ok
  int exitCode = -1;
  qint64 neededBytes = 0;        // not_enough_memory / disk_full: what the build needs ...
  qint64 freeBytes = 0;          // ... and what is free
  QStringList log;               // "HH:mm:ss text" lines without game data: start, filtered ERROR / WARNING lines, exit code
  QString producedPath;  // ROM output: the one file the engine wrote (its name may carry an extra extension); mod: the folder
  QString spoilerLogPath;        // <outPath>.log when requested and written
  bool launcherUsed = false;     // the call went through uzume-fvx-launcher.jar (3DS jobs)
};

class EngineRunner : public QObject {
  Q_OBJECT
 public:
  explicit EngineRunner(QObject* parent = nullptr);
  ~EngineRunner() override;

  static constexpr int kHeapMb3ds = 4096;
  static constexpr int kHeapMbOther = 1024;
  static constexpr int kTimeLimit3dsMs = 15 * 60 * 1000;
  static constexpr int kTimeLimitOtherMs = 3 * 60 * 1000;
  // Estimated duration of a build from the size of the base file.
  static qint64 estimateMs(bool isMod, qint64 baseBytes);
  // Memory a build wants free (three quarters of the heap) / disk it wants free.
  static qint64 memoryNeededBytes(int heapMb) { return static_cast<qint64>(heapMb) * 1024 * 1024 * 3 / 4; }
  static qint64 diskNeededBytes(bool isMod, qint64 baseBytes);
  static qint64 availableMemoryBytes();  // -1 = unknown
  static constexpr const char* kLauncherJarName = "uzume-fvx-launcher.jar";
  static constexpr const char* kLauncherClass = "UzumeFvxLauncher";
  // Writes the embedded launcher jar to <dir>/uzume-fvx-launcher.jar unless a file with the same bytes is there; false when it is
  // not compiled in or cannot be written.
  static bool extractLauncher(const QString& dir);
  // The arguments up to (not including) "cli": heap option and either -jar <jar> or -cp <jar><sep><launcher> <class>.
  static QStringList javaPrefix(const EngineJob& job, bool useLauncher);

  // Tests: replace the probes for free memory and free disk space (bytes; <0 = unknown).
  void setMemoryProbe(std::function<qint64()> fn) { memoryProbe_ = std::move(fn); }
  void setDiskProbe(std::function<qint64(const QString&)> fn) { diskProbe_ = std::move(fn); }

  bool running() const { return running_; }
  void start(const EngineJob& job);  // result via finished(); progress via progress()
  void cancel();                     // kills the process; finished() with "cancelled"

 signals:
  void progress(const uzume::EngineProgress& p);
  void finished(const uzume::EngineResult& r);

 private:
  void finish(const QString& error);
  void readOutput();
  void note(const QString& text);
  void tick();

  EngineJob job_;
  QPointer<QProcess> proc_;
  QTimer ticker_;
  QTimer limit_;
  QElapsedTimer clock_;
  bool running_ = false;
  bool cancelled_ = false;
  bool timedOut_ = false;
  bool finishing_ = false;
  QByteArray lineBuf_;
  QStringList engineLines_;  // last filtered ERROR / WARNING lines
  EngineResult result_;
  std::function<qint64()> memoryProbe_;
  std::function<qint64(const QString&)> diskProbe_;
};

}  // namespace uzume

Q_DECLARE_METATYPE(uzume::EngineProgress)
Q_DECLARE_METATYPE(uzume::EngineResult)
