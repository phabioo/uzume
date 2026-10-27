#include "variantbuilder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLoggingCategory>
#include <QtConcurrent>

#include "romcache.h"

namespace uzume {

Q_LOGGING_CATEGORY(lcVariantBuild, "uzume.variants")

VariantBuilder::VariantBuilder(ToolCache* tools, VariantCache* variants, QString platform, QObject* parent)
    : QObject(parent), tools_(tools), variants_(variants), platform_(std::move(platform)) {
  qRegisterMetaType<uzume::BuildProgress>("uzume::BuildProgress");
  qRegisterMetaType<uzume::BuildOutcome>("uzume::BuildOutcome");
  connect(&runner_, &EngineRunner::progress, this, [this](const EngineProgress& p) {
    BuildProgress b;
    b.step = p.step;
    b.fraction = 0.05 + 0.9 * p.fraction;
    b.elapsedMs = p.elapsedMs;
    b.etaMs = p.etaMs;
    emit progress(b);
  });
  connect(&runner_, &EngineRunner::finished, this, &VariantBuilder::onEngineFinished);
}

VariantBuilder::~VariantBuilder() {
  if (cancelFlag_) cancelFlag_->store(true);
  if (running_) {
    if (toolHeld_) tools_->unprotect(recipe_.toolId, recipe_.toolVersion);
    variants_->discardTemp(recipe_.variantId);
  }
}

void VariantBuilder::start(const VariantRecipe& recipe, const QString& baseFile, bool skipBaseCheck) {
  if (running_) return;
  running_ = true;
  cancelled_ = false;
  recipe_ = recipe;
  baseFile_ = baseFile;
  const quint64 gen = ++gen_;
  cancelFlag_ = std::make_shared<std::atomic_bool>(false);
  const auto fail = [this, gen](const QString& code) {
    QMetaObject::invokeMethod(this, [this, gen, code]() {
      if (gen != gen_ || !running_) return;
      BuildOutcome o;
      o.error = code;
      done(o);
    }, Qt::QueuedConnection);
  };
  if (!VariantCache::isValidId(recipe.variantId)) return fail(QStringLiteral("io_error"));
  const auto layout = tools_->layout(recipe.toolId, recipe.toolVersion, platform_);
  if (!layout) return fail(QStringLiteral("engine_missing"));
  layout_ = *layout;
  const QFileInfo bi(baseFile);
  if (baseFile.isEmpty() || !bi.isFile()) return fail(QStringLiteral("base_missing"));
  baseBytes_ = bi.size();
  if (recipe_.romFileName.isEmpty()) recipe_.romFileName = bi.fileName();
  tools_->protect(recipe.toolId, recipe.toolVersion);
  toolHeld_ = true;
  BuildProgress p;
  p.step = QStringLiteral("reading");
  emit progress(p);
  if (recipe.isMod() || skipBaseCheck || recipe.baseSha256.isEmpty()) {
    QMetaObject::invokeMethod(this, [this, gen]() {
      if (gen == gen_ && running_) runEngine();
    }, Qt::QueuedConnection);
    return;
  }
  // Reading the game file: its SHA-256 must be the one the recipe pins (the result depends on the exact dump).
  auto* watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, gen]() {
    const QString sha = watcher->result();
    watcher->deleteLater();
    if (gen != gen_ || !running_) return;
    if (cancelled_) {
      BuildOutcome o;
      o.error = QStringLiteral("cancelled");
      return done(o);
    }
    if (sha.isEmpty()) {
      BuildOutcome o;
      o.error = QStringLiteral("base_missing");
      return done(o);
    }
    if (sha.compare(recipe_.baseSha256, Qt::CaseInsensitive) != 0) {
      BuildOutcome o;
      o.error = QStringLiteral("base_mismatch");
      o.expectedBaseSha = recipe_.baseSha256.toLower();
      o.actualBaseSha = sha;
      return done(o);
    }
    runEngine();
  });
  const std::shared_ptr<std::atomic_bool> flag = cancelFlag_;
  watcher->setFuture(QtConcurrent::run([baseFile, flag]() {
    QString hex;
    return RomCache::sha256OfFile(baseFile, &hex, flag.get()) ? hex : QString();
  }));
}

void VariantBuilder::cancel() {
  if (!running_) return;
  cancelled_ = true;
  if (cancelFlag_) cancelFlag_->store(true);
  if (runner_.running()) runner_.cancel();
}

void VariantBuilder::runEngine() {
  const QString tmp = variants_->tempDir(recipe_.variantId);
  variants_->discardTemp(recipe_.variantId);
  if (!QDir().mkpath(tmp)) {
    BuildOutcome o;
    o.error = QStringLiteral("io_error");
    return done(o);
  }
  EngineJob job;
  job.javaPath = layout_.javaPath;
  job.workDir = layout_.workDir;
  job.jarName = layout_.jarName;
  job.baseFile = baseFile_;
  job.settings = recipe_.settings;
  job.seed = recipe_.seed;
  job.isMod = recipe_.isMod();
  job.directoryOutput = job.isMod;
  job.spoilerLog = recipe_.spoilerLog;
  job.baseBytes = baseBytes_;
  job.heapMb = job.isMod ? EngineRunner::kHeapMb3ds : EngineRunner::kHeapMbOther;
  job.timeLimitMs = timeLimitMs_ > 0 ? timeLimitMs_ : (job.isMod ? EngineRunner::kTimeLimit3dsMs : EngineRunner::kTimeLimitOtherMs);
  if (job.isMod) {
    job.outPath = tmp + QStringLiteral("/engine-out");
  } else {
    QDir().mkpath(tmp + QStringLiteral("/engine"));
    job.outPath = tmp + QStringLiteral("/engine/") + recipe_.romFileName;
  }
  qCInfo(lcVariantBuild) << "build" << recipe_.variantId << (job.isMod ? "mod" : "rom") << "engine" << recipe_.toolVersion;
  runner_.start(job);
}

QString VariantBuilder::findModRoot(const QString& dir, int depth) const {
  const QDir d(dir);
  if (d.exists(QStringLiteral("romfs")) || d.exists(QStringLiteral("exefs")) || d.exists(QStringLiteral("code.bin"))) return dir;
  if (depth <= 0) return {};
  const QFileInfoList subs = d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo& s : subs) {
    const QString r = findModRoot(s.absoluteFilePath(), depth - 1);
    if (!r.isEmpty()) return r;
  }
  return {};
}

void VariantBuilder::onEngineFinished(const EngineResult& r) {
  if (!running_) return;
  BuildOutcome o;
  o.log = r.log;
  o.exitCode = r.exitCode;
  o.neededBytes = r.neededBytes;
  o.freeBytes = r.freeBytes;
  if (!r.ok) {
    o.error = r.error;
    return done(o);
  }
  const QString tmp = variants_->tempDir(recipe_.variantId);
  const QString out = tmp + QStringLiteral("/out");
  bool moved = false;
  if (recipe_.isMod()) {
    const QString root = findModRoot(tmp + QStringLiteral("/engine-out"), 3);
    if (root.isEmpty()) {
      o.error = QStringLiteral("output_missing");
      return done(o);
    }
    QStringList names;
    for (const QFileInfo& e : QDir(root).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name)) names.append(e.fileName());
    o.layoutNote = QStringLiteral("mod folder at '%1' holding: %2")
                       .arg(QDir(tmp + QStringLiteral("/engine-out")).relativeFilePath(root), names.join(QLatin1Char(' ')));
    qCInfo(lcVariantBuild) << o.layoutNote;
    moved = QDir().rename(root, out);
  } else {
    QDir().mkpath(out);
    moved = QFile::rename(r.producedPath, out + QLatin1Char('/') + recipe_.romFileName);
  }
  if (!moved) {
    o.error = QStringLiteral("io_error");
    return done(o);
  }
  if (!r.spoilerLogPath.isEmpty()) QFile::rename(r.spoilerLogPath, tmp + QStringLiteral("/spoiler.log"));
  QDir(tmp + QStringLiteral("/engine")).removeRecursively();
  QDir(tmp + QStringLiteral("/engine-out")).removeRecursively();
  QFile::remove(tmp + QStringLiteral("/engine-out.log"));
  if (!variants_->commit(recipe_)) {
    o.error = QStringLiteral("io_error");
    return done(o);
  }
  o.ok = true;
  done(o);
}

void VariantBuilder::done(BuildOutcome o) {
  if (!running_) return;
  running_ = false;
  if (toolHeld_) tools_->unprotect(recipe_.toolId, recipe_.toolVersion);
  toolHeld_ = false;
  if (!o.ok) variants_->discardTemp(recipe_.variantId);
  if (o.error == QLatin1String("cancelled")) o.ok = false;
  emit finished(o);
}

}  // namespace uzume
