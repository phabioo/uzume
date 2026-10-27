#include "enginerunner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStorageInfo>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace uzume {

Q_LOGGING_CATEGORY(lcEngine, "uzume.engine")

namespace {
constexpr int kKeepLines = 20;
constexpr int kMaxLineChars = 200;
}  // namespace

EngineRunner::EngineRunner(QObject* parent) : QObject(parent) {
  qRegisterMetaType<uzume::EngineProgress>("uzume::EngineProgress");
  qRegisterMetaType<uzume::EngineResult>("uzume::EngineResult");
  ticker_.setInterval(250);
  connect(&ticker_, &QTimer::timeout, this, &EngineRunner::tick);
  limit_.setSingleShot(true);
  connect(&limit_, &QTimer::timeout, this, [this]() {
    if (!running_) return;
    timedOut_ = true;
    if (proc_) proc_->kill();
  });
}

EngineRunner::~EngineRunner() {
  if (proc_) {
    proc_->disconnect(this);
    proc_->kill();
    proc_->waitForFinished(2000);
  }
}

qint64 EngineRunner::estimateMs(bool isMod, qint64 baseBytes) {
  const double mb = static_cast<double>(baseBytes) / (1024.0 * 1024.0);
  return isMod ? static_cast<qint64>(60000 + mb * 90.0 * 1000.0 / 1024.0) : static_cast<qint64>(3000 + mb * 150.0);
}

qint64 EngineRunner::diskNeededBytes(bool isMod, qint64 baseBytes) {
  return isMod ? qint64(512) * 1024 * 1024 : baseBytes + baseBytes / 10 + 64 * 1024 * 1024;
}

qint64 EngineRunner::availableMemoryBytes() {
#ifdef Q_OS_WIN
  MEMORYSTATUSEX m;
  m.dwLength = sizeof(m);
  if (GlobalMemoryStatusEx(&m)) return static_cast<qint64>(m.ullAvailPhys);
  return -1;
#else
  QFile f(QStringLiteral("/proc/meminfo"));
  if (!f.open(QIODevice::ReadOnly)) return -1;
  while (!f.atEnd()) {
    const QByteArray line = f.readLine();
    if (line.startsWith("MemAvailable:")) {
      const QList<QByteArray> parts = line.simplified().split(' ');
      if (parts.size() >= 2) return parts.at(1).toLongLong() * 1024;
    }
  }
  return -1;
#endif
}

void EngineRunner::note(const QString& text) { result_.log.append(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss ")) + text); }

void EngineRunner::start(const EngineJob& job) {
  if (running_) return;
  job_ = job;
  running_ = true;
  cancelled_ = timedOut_ = finishing_ = false;
  lineBuf_.clear();
  engineLines_.clear();
  result_ = {};
  if (!QFileInfo(job.javaPath).isFile() || !QFileInfo(job.workDir + QLatin1Char('/') + job.jarName).isFile()) {
    QMetaObject::invokeMethod(this, [this]() { finish(QStringLiteral("engine_missing")); }, Qt::QueuedConnection);
    return;
  }
  if (!QFileInfo(job.baseFile).isFile()) {
    QMetaObject::invokeMethod(this, [this]() { finish(QStringLiteral("base_missing")); }, Qt::QueuedConnection);
    return;
  }
  const qint64 needMem = memoryNeededBytes(job.heapMb);
  const qint64 freeMem = memoryProbe_ ? memoryProbe_() : availableMemoryBytes();
  if (freeMem >= 0 && freeMem < needMem) {
    result_.neededBytes = needMem;
    result_.freeBytes = freeMem;
    QMetaObject::invokeMethod(this, [this]() { finish(QStringLiteral("not_enough_memory")); }, Qt::QueuedConnection);
    return;
  }
  const QString outParent = QFileInfo(job.outPath).absolutePath();
  QDir().mkpath(outParent);
  const qint64 needDisk = diskNeededBytes(job.isMod, job.baseBytes);
  const qint64 freeDisk = diskProbe_ ? diskProbe_(outParent) : QStorageInfo(outParent).bytesAvailable();
  if (freeDisk >= 0 && freeDisk < needDisk) {
    result_.neededBytes = needDisk;
    result_.freeBytes = freeDisk;
    QMetaObject::invokeMethod(this, [this]() { finish(QStringLiteral("disk_full")); }, Qt::QueuedConnection);
    return;
  }

  QStringList args{QStringLiteral("-Xmx%1M").arg(job.heapMb), QStringLiteral("-jar"), job.jarName, QStringLiteral("cli"),
                   QStringLiteral("-i"), job.baseFile, QStringLiteral("-o"), job.outPath, QStringLiteral("-S"), job.settings,
                   QStringLiteral("-z"), job.seed};
  if (job.directoryOutput) args << QStringLiteral("-d");
  if (job.spoilerLog) args << QStringLiteral("-l");
  proc_ = new QProcess(this);
  proc_->setProgram(job.javaPath);
  proc_->setArguments(args);
  proc_->setWorkingDirectory(job.workDir);
  proc_->setProcessChannelMode(QProcess::SeparateChannels);
  connect(proc_, &QProcess::readyReadStandardOutput, this, &EngineRunner::readOutput);
  connect(proc_, &QProcess::readyReadStandardError, this, [this]() {
    if (proc_) proc_->readAllStandardError();  // dropped on purpose (stack traces name classes and files)
  });
  connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart && running_) {
      note(QStringLiteral("the engine could not be started"));
      finish(QStringLiteral("engine_failed"));
    }
  });
  connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
    if (!running_) return;
    readOutput();
    result_.exitCode = st == QProcess::NormalExit ? code : -1;
    if (cancelled_) return finish(QStringLiteral("cancelled"));
    if (timedOut_) return finish(QStringLiteral("timed_out"));
    note(QStringLiteral("exit code %1").arg(result_.exitCode));
    finish(st == QProcess::NormalExit && code == 0 ? QString() : QStringLiteral("engine_failed"));
  });
  note(QStringLiteral("engine started"));
  clock_.start();
  limit_.start(job.timeLimitMs > 0 ? job.timeLimitMs : (job.isMod ? kTimeLimit3dsMs : kTimeLimitOtherMs));
  ticker_.start();
  qCInfo(lcEngine) << "start" << (job.isMod ? "mod" : "rom") << "base bytes" << job.baseBytes << "heap" << job.heapMb;
  proc_->start();
  tick();
}

void EngineRunner::cancel() {
  if (!running_ || finishing_) return;
  cancelled_ = true;
  if (proc_ && proc_->state() != QProcess::NotRunning) proc_->kill();
  else finish(QStringLiteral("cancelled"));
}

void EngineRunner::tick() {
  if (!running_) return;
  EngineProgress p;
  p.elapsedMs = clock_.elapsed();
  const qint64 est = std::max<qint64>(1, estimateMs(job_.isMod, job_.baseBytes));
  p.fraction = std::min(0.95, static_cast<double>(p.elapsedMs) / static_cast<double>(est));
  p.step = p.fraction < 0.8 ? QStringLiteral("randomizing") : QStringLiteral("writing");
  p.etaMs = est > p.elapsedMs ? est - p.elapsedMs : 0;
  emit progress(p);
}

void EngineRunner::readOutput() {
  if (!proc_) return;
  lineBuf_.append(proc_->readAllStandardOutput());
  qsizetype nl;
  while ((nl = lineBuf_.indexOf('\n')) >= 0) {
    const QString line = QString::fromUtf8(lineBuf_.left(nl)).trimmed();
    lineBuf_.remove(0, nl + 1);
    // Only the engine's own error and warning lines are kept; the rest of its output may describe the game.
    if (line.startsWith(QLatin1String("ERROR:")) || line.startsWith(QLatin1String("WARNING:"))) {
      engineLines_.append(line.left(kMaxLineChars));
      if (engineLines_.size() > kKeepLines) engineLines_.removeFirst();
    }
  }
  if (lineBuf_.size() > 64 * 1024) lineBuf_.clear();  // a line without end: not kept
}

void EngineRunner::finish(const QString& error) {
  if (!running_ || finishing_) return;
  finishing_ = true;
  ticker_.stop();
  limit_.stop();
  if (proc_) {
    proc_->disconnect(this);
    if (proc_->state() != QProcess::NotRunning) {
      proc_->kill();
      proc_->waitForFinished(3000);
    }
    proc_->deleteLater();
    proc_.clear();
  }
  for (const QString& l : std::as_const(engineLines_)) note(l);
  result_.error = error;
  result_.ok = error.isEmpty();
  if (result_.ok) {
    // FVX does not write exactly -o for ROMs: FileNameFunctions.fixFilename appends the default extension (red.gb -> red.gb.gbc)
    // and the -l log is "<actual output>.log". So the one produced file in the output folder (not *.log) is the result.
    QString produced = job_.outPath;
    bool present = false;
    if (job_.directoryOutput) {
      present = QFileInfo(produced).isDir();
    } else {
      const QFileInfo want(job_.outPath);
      const QFileInfoList files = QDir(want.absolutePath()).entryInfoList(QDir::Files, QDir::Name);
      QList<QFileInfo> cand;
      for (const QFileInfo& f : files)
        if (!f.fileName().endsWith(QLatin1String(".log"), Qt::CaseInsensitive) && f.size() > 0) cand.append(f);
      if (cand.size() == 1) {
        produced = cand.first().absoluteFilePath();
        present = true;
      } else {
        note(cand.isEmpty() ? QStringLiteral("no output file") : QStringLiteral("several output files"));
      }
    }
    if (!present) {
      result_.ok = false;
      result_.error = job_.directoryOutput ? QStringLiteral("output_missing") : QStringLiteral("engine_failed");
    } else {
      result_.producedPath = produced;
      if (job_.spoilerLog && QFileInfo::exists(produced + QStringLiteral(".log"))) result_.spoilerLogPath = produced + QStringLiteral(".log");
    }
  } else if (error == QLatin1String("engine_failed")) {
    // A failure on a full disk is a disk problem, not an engine problem.
    const QString parent = QFileInfo(job_.outPath).absolutePath();
    const qint64 freeDisk = diskProbe_ ? diskProbe_(parent) : QStorageInfo(parent).bytesAvailable();
    if (freeDisk >= 0 && freeDisk < 16ll * 1024 * 1024) {
      result_.error = QStringLiteral("disk_full");
      result_.freeBytes = freeDisk;
      result_.neededBytes = diskNeededBytes(job_.isMod, job_.baseBytes);
    }
  }
  qCInfo(lcEngine) << "finished" << (result_.ok ? "ok" : result_.error) << "exit" << result_.exitCode;
  running_ = false;
  const EngineResult res = result_;
  emit finished(res);
}

}  // namespace uzume
