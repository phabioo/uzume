#include "statescontroller.h"

#include <QBuffer>
#include <QDateTime>
#include <QImage>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>
#include <cmath>

#include "gamesession.h"
#include "localstoragecontroller.h"
#include "timeformat.h"

Q_LOGGING_CATEGORY(lcStates, "uzume.states")

namespace uzume::ui {

namespace {

using Cmd = emu::EmulationRunner::Command;

QString keyOf(const QString& game, const QString& slot, int n) { return game + QLatin1Char('/') + slot + QLatin1Char('/') + QString::number(n); }

// Numeric parts of a core version ("0.10.3", "2026.10.09", "1.4.0-rc1") for ordering; empty when there is none.
QList<int> versionParts(const QString& v) {
  QList<int> parts;
  for (const QString& p : v.split(QRegularExpression(QStringLiteral("[^0-9]+")), Qt::SkipEmptyParts)) parts.append(p.toInt());
  return parts;
}

}  // namespace

StatesController::StatesController(GameSession* session, PlayerSettings* settings, QObject* parent)
    : QObject(parent), session_(session), settings_(settings) {
  toastTimer_.setSingleShot(true);
  connect(&toastTimer_, &QTimer::timeout, this, [this]() { dismissToast(); });
  if (session_ != nullptr) {
    connect(session_, &GameSession::rewindProgress, this, [this](double left) {
      const QString t = tr("Rewinding · %1 s left").arg(static_cast<int>(std::ceil(left)));
      if (t != rewindText_) {
        rewindText_ = t;
        emit rewindTextChanged();
      }
    });
    connect(session_, &GameSession::rewindFinished, this, [this](bool exhausted) {
      if (!rewindText_.isEmpty()) {
        rewindText_.clear();
        emit rewindTextChanged();
      }
      if (exhausted) showToast(tr("Nothing further back to rewind to"), QStringLiteral("warning"));
    });
  }
}

StatesController::~StatesController() = default;

// ------------------------------------------------------------------ wiring

void StatesController::setLocation(const QString& statesDir) {
  store_ = statesDir.isEmpty() ? nullptr : std::make_unique<StateStore>(statesDir);
  rebuild();
}

void StatesController::setApi(StateApi* api) { api_ = api; }

void StatesController::gameStarted(const GameContext& ctx) {
  ctx_ = ctx;
  running_ = true;
  viewGame_ = ctx.gameId;
  slot_ = 1;
  rewindHeld_ = false;
  rewindText_.clear();
  loadRequest_.clear();
  applyRewind();
  rebuild();
  refresh();
  emit loadRequestChanged();
  emit rewindTextChanged();
}

void StatesController::gameStopped() {
  if (rewindHeld_ && session_) session_->setRewinding(false);
  rewindHeld_ = false;
  running_ = false;
  ctx_ = GameContext();
  rewindText_.clear();
  loadRequest_.clear();
  dismissToast();
  rebuild();
  emit loadRequestChanged();
  emit rewindTextChanged();
}

void StatesController::applyOptions(int rewindSeconds, int runAhead) {
  wantedRewindSeconds_ = (std::max)(0, rewindSeconds);
  wantedRunAhead_ = std::clamp(runAhead, 0, 2);
  applyRewind();
}

StatesController::Rules StatesController::rules() const {
  Rules r;
  if (!running_ || !rulesFn_) return r;
  if (const auto rr = rulesFn_()) {
    r.inRun = true;
    r.rules = *rr;
  }
  return r;
}

// What the emulation thread is told: rewind only when the person set a length, the core can serialize, the system is not the 3DS
// (a state of hundreds of MB) and the run allows it; run-ahead only for cores whose profile says so.
void StatesController::applyRewind() {
  if (!session_) return;
  const bool states = running_ && ctx_.states;
  const Rules r = rules();
  const bool rewindOk = states && rewindAllowedFor(ctx_.systemId) && (!r.inRun || r.rules.rewind);
  session_->setRewindSeconds(rewindOk ? wantedRewindSeconds_ : 0);
  session_->setRunAhead(states && ctx_.runAhead && ctx_.systemId != QLatin1String("3ds") ? wantedRunAhead_ : 0);
  if (!rewindOk && rewindHeld_) {
    rewindHeld_ = false;
    session_->setRewinding(false);
  }
}

void StatesController::rulesChanged() {
  const Rules r = rules();
  const bool allowed = !r.inRun || r.rules.rewind;
  if (allowed != lastRewindAllowed_) {
    lastRewindAllowed_ = allowed;
    applyRewind();
  }
  if (!loadRequest_.isEmpty() && !blockedText().isEmpty()) cancelLoad();
  emit changed();
}

void StatesController::connectionChanged() {
  if (!viewGame_.isEmpty()) refresh();
  syncPending();
}

// ------------------------------------------------------------------ gating

QString StatesController::gate(bool rewind) const {
  if (!running_) return QStringLiteral("no_game");
  if (!ctx_.states) return QStringLiteral("unsupported");
  const Rules r = rules();
  if (r.inRun && !(rewind ? r.rules.rewind : r.rules.states)) return rewind ? QStringLiteral("rule_rewind") : QStringLiteral("rule_states");
  return {};
}

QString StatesController::whyBlocked(const QString& code) {
  if (code == QLatin1String("rule_states")) return tr("Save states are off in this run");
  if (code == QLatin1String("rule_rewind")) return tr("Rewind is off in this run");
  if (code == QLatin1String("unsupported")) return tr("This game's core cannot save states");
  if (code == QLatin1String("no_game")) return tr("Start the game to save or load a state");
  return code;
}

QString StatesController::blockedText() const {
  const QString why = playing() ? gate(false) : QString();
  if (why.isEmpty() || why == QLatin1String("no_game")) return {};
  QString t = whyBlocked(why);
  if (why == QLatin1String("rule_states") || why == QLatin1String("rule_rewind")) {
    const QString who = ruleOwnerFn_ ? ruleOwnerFn_() : QString();
    t += who.isEmpty() ? QString() : tr(" · rule set by %1").arg(who);
  }
  return t;
}

bool StatesController::askBeforeLoad() const { return settings_ == nullptr || settings_->stateLoadAsk(); }

void StatesController::setAskBeforeLoad(bool on) {
  if (settings_ == nullptr || settings_->stateLoadAsk() == on) return;
  settings_->setStateLoadAsk(on);
  emit askBeforeLoadChanged();
}

bool StatesController::rewindAvailable() const {
  return playing() && ctx_.states && rewindAllowedFor(ctx_.systemId) && session_ && session_->rewindSeconds() > 0;
}

// ------------------------------------------------------------------ the list

void StatesController::view(const QString& gameId) {
  if (gameId.isEmpty()) return;
  if (running_ && gameId == ctx_.gameId) {
    viewGame_ = gameId;
  } else {
    viewGame_ = gameId;
    view_ = viewResolver_ ? viewResolver_(gameId) : GameContext();
    view_.gameId = gameId;
  }
  rebuild();
  refresh();
}

void StatesController::setSlot(int n) {
  if (n < 1 || n > kStateSlotCount || n == slot_) return;
  slot_ = n;
  rebuild();
}

int StatesController::nextSlot() {
  slot_ = StateStore::nextSlot(slot_);
  rebuild();
  const auto rec = store() ? store()->record(ctx().gameId, ctx().saveSlot, slot_) : std::nullopt;
  showToast(rec && !rec->deletePending ? tr("State slot %1 · saved %2").arg(slot_).arg(TimeFormat::whenIso(rec->createdAt))
                                       : tr("State slot %1 · empty").arg(slot_));
  return slot_;
}

QList<StateRecord> StatesController::records() const {
  QList<StateRecord> out;
  if (!store() || ctx().gameId.isEmpty()) return out;
  for (const StateRecord& r : store()->list(ctx().gameId, ctx().saveSlot)) {
    if (!r.deletePending && r.n <= kStateSlotCount) out.append(r);
  }
  return out;
}

QString StatesController::versionTag(const QString& recordVersion, const QString& currentVersion) {
  if (recordVersion.isEmpty() || currentVersion.isEmpty() || recordVersion == currentVersion) return {};
  const QList<int> a = versionParts(recordVersion), b = versionParts(currentVersion);
  if (a.isEmpty() || b.isEmpty() || a == b) return tr("Other core version");
  return a < b ? tr("Older core") : tr("Newer core");
}

qint64 StatesController::typicalStateBytes(const QString& systemId) {
  if (systemId == QLatin1String("gb") || systemId == QLatin1String("gbc")) return 150 * 1024;
  if (systemId == QLatin1String("gba")) return 400 * 1024;
  if (systemId == QLatin1String("nds")) return 7500 * 1024;
  return 0;
}

void StatesController::rebuild() {
  slots_.clear();
  summary_.clear();
  const GameContext& c = ctx();
  const bool play = playing();
  const QString block = play ? gate(false) : QStringLiteral("no_game");
  QHash<int, StateRecord> byN;
  int filled = 0;
  if (store() && !c.gameId.isEmpty()) {
    for (const StateRecord& r : store()->list(c.gameId, c.saveSlot)) {
      if (r.deletePending || r.n > kStateSlotCount) continue;
      byN.insert(r.n, r);
      if (r.n >= 1) ++filled;
    }
  }
  bool anyPending = false, anyServer = false;
  for (int n = 0; n <= kStateSlotCount; ++n) {
    QVariantMap m{{QStringLiteral("n"), n}, {QStringLiteral("label"), StateStore::label(n)}, {QStringLiteral("empty"), !byN.contains(n)},
                  {QStringLiteral("current"), n == slot_}};
    if (byN.contains(n)) {
      const StateRecord& r = byN.value(n);
      const QString core = r.coreVersion.isEmpty() ? r.coreId : (r.coreId + QLatin1Char(' ') + r.coreVersion);
      QString tag;
      const bool sameCore = c.coreId.isEmpty() || r.coreId.isEmpty() || r.coreId == c.coreId;
      if (!sameCore) tag = tr("Other core");
      else tag = versionTag(r.coreVersion, c.coreVersion);
      m.insert(QStringLiteral("whenText"), TimeFormat::whenIso(r.createdAt));
      m.insert(QStringLiteral("whenFull"), TimeFormat::fullIso(r.createdAt));
      m.insert(QStringLiteral("coreText"), core);
      m.insert(QStringLiteral("tag"), tag);
      m.insert(QStringLiteral("canLoad"), play && block.isEmpty() && sameCore);
      m.insert(QStringLiteral("loadLabel"), tag.isEmpty() || !sameCore ? tr("Load") : tr("Try to load"));
      m.insert(QStringLiteral("onServer"), r.onServer);
      m.insert(QStringLiteral("pending"), r.pendingUpload);
      m.insert(QStringLiteral("sizeText"), LocalStorageController::formatBytes(r.sizeBytes));
      m.insert(QStringLiteral("device"), r.deviceName);
      const QString pv = store()->previewPath(c.gameId, c.saveSlot, n);
      m.insert(QStringLiteral("previewUrl"), pv.isEmpty() ? QString() : QUrl::fromLocalFile(pv).toString());
      anyPending = anyPending || r.pendingUpload;
      anyServer = anyServer || r.onServer;
    }
    m.insert(QStringLiteral("canSave"), n >= 1 && play && block.isEmpty());
    slots_.append(m);
  }
  const QString where = serverStates() ? (serverName_.isEmpty() ? tr("on the server") : tr("on %1").arg(serverName_)) : tr("on this PC only");
  summary_ = tr("%1 of %2 · %3").arg(filled).arg(kStateSlotCount).arg(where).toUpper();
  Q_UNUSED(anyPending)
  Q_UNUSED(anyServer)
  emit changed();
}

void StatesController::refresh() {
  rebuild();
  syncWithServer();
}

// ------------------------------------------------------------------ server

void StatesController::syncWithServer() {
  const GameContext& c = ctx();
  if (!serverStates() || !store() || c.gameId.isEmpty()) return;
  const QString gameId = c.gameId, slot = c.saveSlot;
  const QString lk = gameId + QLatin1Char('/') + slot;
  if (listing_.contains(lk)) return;
  listing_.insert(lk);
  ++busy_;
  api_->list(gameId, slot, [this, gameId, slot, lk](const StateApiResult& r) {
    listing_.remove(lk);
    --busy_;
    if (r.ok()) applyServerList(r, gameId, slot);
    else rebuild();
  });
}

// The server's list wins for what is on the server; what only exists here and waits for an upload stays; a state that was on the
// server and is gone was deleted on another device.
void StatesController::applyServerList(const StateApiResult& r, const QString& gameId, const QString& slot) {
  if (!store()) return;
  lastQuotaBytes_ = r.quotaBytes;
  lastUsedBytes_ = r.usedBytes;
  QSet<int> onServer;
  for (const StateRecord& s : r.states) {
    if (s.gameId != gameId || s.saveSlot != slot || s.n > kStateSlotCount) continue;
    onServer.insert(s.n);
    auto local = store()->record(gameId, slot, s.n);
    if (local && local->deletePending) continue;  // we are deleting it
    if (local && local->pendingUpload && local->sha256 != s.sha256) continue;  // our newer state is waiting to go up
    StateRecord rec = s;
    rec.onServer = true;
    rec.pendingUpload = false;
    if (local && local->sha256 == s.sha256) {
      rec.hasPreview = local->hasPreview;
      if (rec.deviceName.isEmpty()) rec.deviceName = local->deviceName;
    }
    store()->putRecord(rec);
    if (s.hasPreview && store()->previewPath(gameId, slot, s.n).isEmpty() && api_) {
      const int n = s.n;
      api_->preview(gameId, slot, n, [this, gameId, slot, n](const StateApiResult& pr) {
        if (pr.ok() && store() && store()->putPreview(gameId, slot, n, pr.content)) rebuild();
      });
    }
  }
  for (const StateRecord& l : store()->list(gameId, slot)) {
    if (l.n > kStateSlotCount || onServer.contains(l.n)) continue;
    if (l.deletePending) {
      store()->remove(gameId, slot, l.n);  // already gone on the server
    } else if (l.pendingUpload) {
      uploadRecord(l);
    } else if (l.onServer) {
      store()->remove(gameId, slot, l.n);  // deleted on another device
    } else if (!serverStates()) {
      continue;
    } else {
      StateRecord up = l;  // saved while the server could not take it: send it now
      up.pendingUpload = true;
      store()->updateRecord(up);
      uploadRecord(up);
    }
  }
  rebuild();
}

void StatesController::uploadRecord(const StateRecord& rec) {
  if (!serverStates() || !store()) return;
  const QString k = keyOf(rec.gameId, rec.saveSlot, rec.n);
  if (uploading_.contains(k)) return;
  const QByteArray blob = store()->state(rec.gameId, rec.saveSlot, rec.n);
  if (blob.isEmpty()) return;
  uploading_.insert(k);
  const QByteArray pv = store()->preview(rec.gameId, rec.saveSlot, rec.n);
  api_->put(rec, blob, pv, [this, rec, k](const StateApiResult& r) {
    uploading_.remove(k);
    if (!store()) return;
    auto cur = store()->record(rec.gameId, rec.saveSlot, rec.n);
    if (!cur || cur->sha256 != rec.sha256) return;  // replaced meanwhile: the newer one has its own upload
    if (r.ok()) {
      cur->onServer = true;
      cur->pendingUpload = false;
      if (r.state) cur->deviceName = r.state->deviceName;
      store()->updateRecord(*cur);
    } else if (r.kind == StateApiResult::Kind::QuotaFull || r.kind == StateApiResult::Kind::TooLarge || r.kind == StateApiResult::Kind::Rejected) {
      cur->pendingUpload = false;  // no retry helps: it stays on this PC
      store()->updateRecord(*cur);
      showToast(r.kind == StateApiResult::Kind::QuotaFull ? tr("The server has no room for more states · kept on this PC")
                                                           : tr("The server did not take this state · kept on this PC"),
                QStringLiteral("warning"));
    }
    rebuild();
  });
}

void StatesController::deleteOnServer(const StateRecord& rec) {
  if (!serverStates() || !store()) return;
  api_->remove(rec.gameId, rec.saveSlot, rec.n, [this, rec](const StateApiResult& r) {
    if (!store()) return;
    if (r.ok() || r.kind == StateApiResult::Kind::NotFound) {
      if (auto cur = store()->record(rec.gameId, rec.saveSlot, rec.n); cur && cur->deletePending) store()->remove(rec.gameId, rec.saveSlot, rec.n);
      rebuild();
    }
  });
}

void StatesController::syncPending() {
  if (!serverStates() || !store()) return;
  for (const StateRecord& r : store()->pending()) {
    if (r.deletePending) deleteOnServer(r);
    else uploadRecord(r);
  }
}

// The blob of a state: the local cache, else the server (verified against the ETag, then cached).
void StatesController::ensureBlob(const StateRecord& rec, std::function<void(const QByteArray&, const QString& err)> done) {
  if (!store()) return done({}, QStringLiteral("unavailable"));
  const QByteArray local = store()->state(rec.gameId, rec.saveSlot, rec.n);
  if (!local.isEmpty()) return done(local, QString());
  if (!rec.onServer || !serverStates()) return done({}, QStringLiteral("unavailable"));
  ++busy_;
  emit changed();
  api_->get(rec.gameId, rec.saveSlot, rec.n, [this, rec, done = std::move(done)](const StateApiResult& r) {
    --busy_;
    if (!r.ok()) {
      emit changed();
      return done({}, r.kind == StateApiResult::Kind::NotFound ? QStringLiteral("empty") : QStringLiteral("unavailable"));
    }
    if (store()) {
      StateRecord up = rec;
      up.coreId = r.coreId.isEmpty() ? rec.coreId : r.coreId;
      up.coreVersion = r.coreVersion.isEmpty() ? rec.coreVersion : r.coreVersion;
      up.onServer = true;
      up.pendingUpload = false;
      const QByteArray pv = store()->preview(rec.gameId, rec.saveSlot, rec.n);
      store()->put(up, r.content, pv);
    }
    emit changed();
    done(r.content, QString());
  });
}

// ------------------------------------------------------------------ toasts

void StatesController::showToast(const QString& text, const QString& kind, const QString& action, int ms) {
  toast_ = QVariantMap{{QStringLiteral("text"), text}, {QStringLiteral("kind"), kind}, {QStringLiteral("action"), action}, {QStringLiteral("token"), static_cast<qulonglong>(++toastToken_)}};
  toastTimer_.start(action.isEmpty() ? ms : kUndoMs);
  emit toastChanged();
}

void StatesController::dismissToast() {
  toastTimer_.stop();
  if (toast_.isEmpty()) return;
  toast_.clear();
  emit toastChanged();
}

void StatesController::finish(const Done& done, Result r, const QString& toastKind) {
  if (!toastKind.isEmpty() && !r.message.isEmpty()) showToast(r.message, toastKind);
  if (done) done(r);
}

QByteArray StatesController::previewPng(const QImage& frame) {
  if (frame.isNull()) return {};
  QImage img = frame.convertToFormat(QImage::Format_RGB32);
  if (img.width() > 320) img = img.scaledToWidth(320, Qt::SmoothTransformation);
  QByteArray out;
  QBuffer buf(&out);
  buf.open(QIODevice::WriteOnly);
  if (!img.save(&buf, "PNG") || out.size() > kMaxStatePreviewBytes) return {};
  return out;
}

// ------------------------------------------------------------------ save

void StatesController::save(int n) { save(n, {}); }

void StatesController::save(int n, Done done) {
  if (n < 0 || n > kStateSlotCount) return finish(done, {false, QStringLiteral("invalid"), tr("There are four state slots"), n});
  const QString why = gate(false);
  if (!why.isEmpty()) {
    Result r{false, why, blockedText(), n};
    if (why == QLatin1String("rule_states")) {
      const QString who = ruleOwnerFn_ ? ruleOwnerFn_() : QString();
      r.message = whyBlocked(why) + (who.isEmpty() ? QString() : tr(" · rule set by %1").arg(who));
    } else {
      r.message = whyBlocked(why);
    }
    return finish(done, r, QStringLiteral("warning"));
  }
  if (!store()) return finish(done, {false, QStringLiteral("unavailable"), tr("States need a signed-in account on this PC"), n}, QStringLiteral("warning"));
  doSave(n, false, std::move(done));
}

void StatesController::doSave(int n, bool silent, Done done) {
  QPointer<StatesController> self(this);
  ++busy_;
  const quint64 id = session_ ? session_->submitEmuCommand(Cmd::saveState(), [self, n, silent, done](const emu::EmulationRunner::CommandResult& r) {
    // emulation thread: hand over to the GUI thread
    QMetaObject::invokeMethod(self.data(), [self, n, silent, done, r]() {
      if (!self) return;
      --self->busy_;
      if (!r.ok) {
        Result res{false, r.error, tr("The state could not be saved"), n};
        self->finish(done, res, silent ? QString() : QStringLiteral("error"));
        self->rebuild();
        return;
      }
      self->finishSave(n, r.state, r.preview, silent, done);
    }, Qt::QueuedConnection);
  }) : 0;
  if (id == 0) {
    --busy_;
    finish(done, {false, QStringLiteral("no_game"), whyBlocked(QStringLiteral("no_game")), n}, QStringLiteral("warning"));
  }
}

void StatesController::finishSave(int n, const QByteArray& state, const QImage& preview, bool silent, Done done) {
  if (!store()) return finish(done, {false, QStringLiteral("unavailable"), tr("The state could not be saved"), n});
  StateRecord rec;
  rec.gameId = ctx_.gameId;
  rec.saveSlot = ctx_.saveSlot;
  rec.n = n;
  rec.coreId = ctx_.coreId;
  rec.coreVersion = ctx_.coreVersion;
  rec.deviceName = deviceName_;
  rec.onServer = false;
  rec.pendingUpload = api_ != nullptr && !api_->featureMissing();  // retried when the server comes back; local only without a server
  if (!store()->put(rec, state, previewPng(preview))) {
    Result res{false, QStringLiteral("disk"), tr("The state could not be saved on this PC"), n};
    return finish(done, res, silent ? QString() : QStringLiteral("error"));
  }
  const auto stored = store()->record(rec.gameId, rec.saveSlot, n);
  if (stored) uploadRecord(*stored);
  rebuild();
  if (!silent) showToast(tr("State %1 saved").arg(StateStore::label(n)));
  emit stateSaved(n);
  if (done) done({true, QString(), tr("State %1 saved").arg(StateStore::label(n)), n});
}

// ------------------------------------------------------------------ load

void StatesController::requestLoad(int n) {
  const QString why = gate(false);
  if (!why.isEmpty()) {
    load(n, true, {});  // shows the reason as a toast
    return;
  }
  const auto rec = store() ? store()->record(ctx_.gameId, ctx_.saveSlot, n) : std::nullopt;
  if (!rec || rec->deletePending) {
    showToast(tr("State %1 is empty").arg(StateStore::label(n)), QStringLiteral("warning"));
    return;
  }
  if (!askBeforeLoad()) {
    load(n, true, {});
    return;
  }
  loadRequest_ = QVariantMap{{QStringLiteral("n"), n},
                             {QStringLiteral("label"), StateStore::label(n)},
                             {QStringLiteral("whenText"), TimeFormat::whenIso(rec->createdAt)},
                             {QStringLiteral("title"), tr("Load state %1 from %2?").arg(StateStore::label(n), TimeFormat::whenIso(rec->createdAt))}};
  emit loadRequestChanged();
}

void StatesController::confirmLoad() {
  if (loadRequest_.isEmpty()) return;
  const int n = loadRequest_.value(QStringLiteral("n")).toInt();
  loadRequest_.clear();
  emit loadRequestChanged();
  load(n, true, {});
}

void StatesController::cancelLoad() {
  if (loadRequest_.isEmpty()) return;
  loadRequest_.clear();
  emit loadRequestChanged();
}

void StatesController::undoLoad() {
  dismissToast();
  doLoad(kStateAuto, true, {});
}

void StatesController::load(int n, bool force, Done done) {
  Q_UNUSED(force)
  const QString why = gate(false);
  if (!why.isEmpty()) {
    Result r{false, why, why == QLatin1String("rule_states") ? blockedText() : whyBlocked(why), n};
    return finish(done, r, QStringLiteral("warning"));
  }
  doLoad(n, false, std::move(done));
}

// The progress of the moment is kept as Auto first (not for the undo itself), then the state replaces the running game. Both
// commands go through the same queue, so nothing runs between them.
void StatesController::doLoad(int n, bool isUndo, Done done) {
  if (!store()) return finish(done, {false, QStringLiteral("unavailable"), tr("States need a signed-in account on this PC"), n}, QStringLiteral("warning"));
  const auto rec = store()->record(ctx_.gameId, ctx_.saveSlot, n);
  if (!rec || rec->deletePending) {
    return finish(done, {false, QStringLiteral("empty"), tr("State %1 is empty").arg(StateStore::label(n)), n}, QStringLiteral("warning"));
  }
  if (!ctx_.coreId.isEmpty() && !rec->coreId.isEmpty() && rec->coreId != ctx_.coreId) {
    return finish(done, {false, QStringLiteral("other_core"), tr("State %1 was saved with another core").arg(StateStore::label(n)), n}, QStringLiteral("warning"));
  }
  const StateRecord record = *rec;
  QPointer<StatesController> self(this);
  const QString gameId = ctx_.gameId;
  ensureBlob(record, [self, record, n, isUndo, done, gameId](const QByteArray& blob, const QString& err) {
    if (!self) return;
    if (!self->running_ || self->ctx_.gameId != gameId) return;
    if (blob.isEmpty()) {
      const QString msg = err == QLatin1String("empty") ? tr("State %1 is no longer on the server").arg(StateStore::label(n))
                                                         : tr("State %1 is not on this PC and the server is not reachable").arg(StateStore::label(n));
      return self->finish(done, {false, err, msg, n}, QStringLiteral("warning"));
    }
    ++self->busy_;
    if (!isUndo && self->session_) {
      // The Auto state first, in the same queue.
      self->session_->submitEmuCommand(Cmd::saveState(), [self](const emu::EmulationRunner::CommandResult& r) {
        if (!r.ok) return;
        QMetaObject::invokeMethod(self.data(), [self, r]() {
          if (!self) return;
          self->finishSave(kStateAuto, r.state, r.preview, true, {});
        }, Qt::QueuedConnection);
      });
    }
    const quint64 id = self->session_ ? self->session_->submitEmuCommand(Cmd::loadState(blob), [self, n, isUndo, done, record](const emu::EmulationRunner::CommandResult& r) {
      QMetaObject::invokeMethod(self.data(), [self, n, isUndo, done, record, r]() {
        if (!self) return;
        --self->busy_;
        if (!r.ok) {
          const QString msg = tr("State %1 could not be loaded").arg(StateStore::label(n)) +
                              (r.error == QLatin1String("failed") ? tr(" · the core refused it") : QString());
          self->finish(done, {false, r.error, msg, n}, QStringLiteral("error"));
          self->rebuild();
          return;
        }
        self->postRunEvent(record);
        self->rebuild();
        if (isUndo) {
          self->showToast(tr("Back to where you were"));
        } else {
          self->showToast(tr("State %1 loaded").arg(StateStore::label(n)), QStringLiteral("done"), tr("Undo"));
        }
        emit self->stateLoaded(n);
        if (done) done({true, QString(), tr("State %1 loaded").arg(StateStore::label(n)), n});
      }, Qt::QueuedConnection);
    }) : 0;
    if (id == 0) {
      --self->busy_;
      self->finish(done, {false, QStringLiteral("no_game"), whyBlocked(QStringLiteral("no_game")), n}, QStringLiteral("warning"));
    }
  });
}

void StatesController::postRunEvent(const StateRecord& rec) {
  if (!runEventFn_ || !rules().inRun) return;
  runEventFn_(QStringLiteral("state.loaded"), tr("Loaded state %1").arg(StateStore::label(rec.n)),
              QJsonObject{{QStringLiteral("n"), rec.n}, {QStringLiteral("saved_at"), rec.createdAt}});
}

// ------------------------------------------------------------------ delete

void StatesController::remove(int n) { remove(n, {}); }

void StatesController::remove(int n, Done done) {
  const GameContext& c = ctx();
  if (!store() || c.gameId.isEmpty()) return finish(done, {false, QStringLiteral("unavailable"), tr("There is no state to delete"), n});
  auto rec = store()->record(c.gameId, c.saveSlot, n);
  if (!rec || rec->deletePending) return finish(done, {false, QStringLiteral("empty"), tr("State %1 is empty").arg(StateStore::label(n)), n});
  if (rec->onServer || rec->pendingUpload) {
    if (serverStates() && rec->onServer) {
      // gone from this PC now; the server copy follows (retried when offline)
      StateRecord tomb = *rec;
      tomb.deletePending = true;
      store()->remove(c.gameId, c.saveSlot, n);
      store()->putRecord(tomb);
      deleteOnServer(tomb);
    } else if (rec->onServer) {
      StateRecord tomb = *rec;
      tomb.deletePending = true;
      store()->remove(c.gameId, c.saveSlot, n);
      store()->putRecord(tomb);
    } else {
      store()->remove(c.gameId, c.saveSlot, n);
    }
  } else {
    store()->remove(c.gameId, c.saveSlot, n);
  }
  rebuild();
  showToast(tr("State %1 deleted").arg(StateStore::label(n)));
  if (done) done({true, QString(), tr("State %1 deleted").arg(StateStore::label(n)), n});
}

// ------------------------------------------------------------------ rewind

void StatesController::rewindKey(bool down) {
  if (!session_) return;
  if (!down) {
    if (rewindHeld_) {
      rewindHeld_ = false;
      session_->setRewinding(false);
    }
    return;
  }
  if (rewindHeld_) return;
  const QString why = gate(true);
  if (!why.isEmpty()) {
    if (why == QLatin1String("rule_rewind")) {
      const QString who = ruleOwnerFn_ ? ruleOwnerFn_() : QString();
      showToast(whyBlocked(why) + (who.isEmpty() ? QString() : tr(" · rule set by %1").arg(who)), QStringLiteral("warning"));
    } else if (why != QLatin1String("no_game")) {
      showToast(tr("Rewind is not available for this game"), QStringLiteral("warning"));
    }
    return;
  }
  if (!rewindAllowedFor(ctx_.systemId) || session_->rewindSeconds() <= 0) {
    showToast(tr("Rewind is off · turn it on in Emulation settings"), QStringLiteral("warning"));
    return;
  }
  if (session_->rewindInfo().snapshots <= 0) {
    showToast(tr("Nothing to rewind to yet"), QStringLiteral("warning"));
    return;
  }
  rewindHeld_ = true;
  session_->setRewinding(true);
}

}  // namespace uzume::ui

namespace uzume::ui {

void StatesController::rewindBy(int snapshots, Done done) {
  const QString why = gate(true);
  if (!why.isEmpty()) return finish(done, {false, why, why == QLatin1String("rule_rewind") ? blockedText() : whyBlocked(why), 0}, QStringLiteral("warning"));
  if (!session_ || session_->rewindSeconds() <= 0 || !rewindAllowedFor(ctx_.systemId)) {
    return finish(done, {false, QStringLiteral("off"), tr("Rewind is off for this game"), 0}, QStringLiteral("warning"));
  }
  QPointer<StatesController> self(this);
  const quint64 id = session_->submitEmuCommand(Cmd::rewind(snapshots), [self, done](const emu::EmulationRunner::CommandResult& r) {
    QMetaObject::invokeMethod(self.data(), [self, done, r]() {
      if (!self) return;
      Result res{r.ok, r.error, r.ok ? tr("Rewound %1 states").arg(r.applied) : tr("Nothing to rewind to"), r.applied};
      if (done) done(res);
    }, Qt::QueuedConnection);
  });
  if (id == 0) finish(done, {false, QStringLiteral("no_game"), whyBlocked(QStringLiteral("no_game")), 0});
}

}  // namespace uzume::ui
