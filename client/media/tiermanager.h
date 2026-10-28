#pragma once

// Which encoders the host keeps alive and which one feeds which viewer (ADR 0033 D5). Pure logic, no clock of its own
// (every call gets `nowMs` from any monotonic clock, so tests use a fake one) and no encoder: SessionHost applies the result.
//
//  - Tiers are Auto (the adaptive encoder), High, Medium, Low. One encoder per tier in use, at most kMaxEncoders alive.
//  - A viewer starts on Auto. An encoder is created on the first viewer that wants its tier and dropped kIdleDropMs after
//    its last viewer left (a viewer coming back inside that time finds it alive).
//  - Everything on Auto: exactly one encoder, the adaptive one (the behaviour before quality picks existed).
//  - At the cap, a wanted tier without an encoder first evicts an encoder nobody uses; if none is idle the viewer gets the
//    alive tier closest in bitrate ("picked Low, gets Medium") and is moved to its pick once room appears (tick()).

#include <QHash>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cstdlib>

#include "qualitytier.h"

namespace uzume {

class TierManager {
 public:
  static constexpr int kMaxEncoders = 3;
  static constexpr qint64 kIdleDropMs = 10'000;
  static constexpr int kAutoNominalKbps = 2000;  // only to rank Auto against the fixed tiers when substituting

  struct Change {
    Quality effective = Quality::Auto;
    bool switched = false;  // the viewer's encoder changed: the new encoder must send it a keyframe
  };

  void clear() {
    viewers_.clear();
    for (Enc& e : enc_) e = Enc();
  }

  Change addViewer(const QString& id, qint64 nowMs) {
    if (const auto it = viewers_.constFind(id); it != viewers_.constEnd()) return {it->effective, false};
    const Quality eff = resolve(Quality::Auto, nowMs);
    viewers_.insert(id, V{Quality::Auto, eff});
    return {eff, true};
  }

  void removeViewer(const QString& id, qint64 nowMs) {
    const auto it = viewers_.find(id);
    if (it == viewers_.end()) return;
    const Quality old = it->effective;
    viewers_.erase(it);
    touchIdle(old, nowMs);
  }

  Change setPick(const QString& id, Quality pick, qint64 nowMs) {
    const auto it = viewers_.find(id);
    if (it == viewers_.end()) return {};
    const Quality old = it->effective;
    it->pick = pick;
    if (old == pick) return {old, false};
    V moved = *it;
    viewers_.erase(it);  // leave the old encoder first: it may be the idle one the new tier evicts at the cap
    touchIdle(old, nowMs);
    moved.effective = resolve(pick, nowMs);
    viewers_.insert(id, moved);
    return {moved.effective, moved.effective != old};
  }

  // Drops encoders that were idle for kIdleDropMs and moves substituted viewers to their pick when there is room.
  // True when the set of encoders or any viewer's tier changed.
  bool tick(qint64 nowMs) {
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
      Enc& e = enc_[i];
      if (e.alive && users(static_cast<Quality>(i)) == 0 && nowMs - e.emptySince >= kIdleDropMs) {
        e = Enc();
        changed = true;
      }
    }
    for (auto it = viewers_.begin(); it != viewers_.end(); ++it) {
      if (it->effective == it->pick) continue;
      if (alive(it->pick) || encoderCount() < kMaxEncoders) {
        const Quality old = it->effective;
        it->effective = resolve(it->pick, nowMs);
        touchIdle(old, nowMs);
        changed = true;
      }
    }
    return changed;
  }

  bool alive(Quality q) const { return enc_[idx(q)].alive; }
  int encoderCount() const {
    int n = 0;
    for (const Enc& e : enc_) n += e.alive ? 1 : 0;
    return n;
  }
  // Alive encoders in tier order (Auto first).
  QList<Quality> aliveTiers() const {
    QList<Quality> out;
    for (int i = 0; i < 4; ++i) {
      if (enc_[i].alive) out.append(static_cast<Quality>(i));
    }
    return out;
  }
  // Bit (1 << tier) per alive fixed tier; bit 0 (Auto) is never set here, see adaptiveAlive().
  int fixedMask() const {
    int m = 0;
    for (int i = 1; i < 4; ++i) m |= enc_[i].alive ? (1 << i) : 0;
    return m;
  }
  bool adaptiveAlive() const { return enc_[0].alive; }
  int users(Quality q) const {
    int n = 0;
    for (const V& v : viewers_) n += v.effective == q ? 1 : 0;
    return n;
  }
  bool hasViewer(const QString& id) const { return viewers_.contains(id); }
  Quality pick(const QString& id) const { return viewers_.value(id).pick; }
  Quality effective(const QString& id) const { return viewers_.value(id).effective; }
  // Viewers whose bitrate the AIMD controller follows: those on the adaptive encoder.
  bool isAuto(const QString& id) const { return viewers_.contains(id) && viewers_.value(id).effective == Quality::Auto; }

 private:
  struct Enc {
    bool alive = false;
    qint64 emptySince = 0;
  };
  struct V {
    Quality pick = Quality::Auto;
    Quality effective = Quality::Auto;
  };
  static int idx(Quality q) { return static_cast<int>(q); }
  static int kbpsOf(Quality q) { return q == Quality::Auto ? kAutoNominalKbps : qualityKbps(q); }

  void touchIdle(Quality q, qint64 nowMs) {
    if (enc_[idx(q)].alive && users(q) == 0) enc_[idx(q)].emptySince = nowMs;
  }
  void create(Quality q, qint64 nowMs) {
    enc_[idx(q)].alive = true;
    enc_[idx(q)].emptySince = nowMs;
  }

  // The tier a viewer that wants `want` is served by (creating or evicting as needed).
  Quality resolve(Quality want, qint64 nowMs) {
    if (alive(want)) return want;
    if (encoderCount() < kMaxEncoders) {
      create(want, nowMs);
      return want;
    }
    for (int i = 0; i < 4; ++i) {  // an encoder nobody uses makes room
      if (enc_[i].alive && users(static_cast<Quality>(i)) == 0) {
        enc_[i] = Enc();
        create(want, nowMs);
        return want;
      }
    }
    Quality best = Quality::Auto;
    int bestDist = 1 << 30;
    for (int i = 0; i < 4; ++i) {
      if (!enc_[i].alive) continue;
      const int d = std::abs(kbpsOf(static_cast<Quality>(i)) - kbpsOf(want));
      if (d < bestDist) {
        bestDist = d;
        best = static_cast<Quality>(i);
      }
    }
    return best;
  }

  QHash<QString, V> viewers_;
  Enc enc_[4];
};

}  // namespace uzume
