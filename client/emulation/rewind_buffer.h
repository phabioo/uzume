#pragma once
// RewindBuffer: the ring buffer of serialized core states behind rewind (0.20 package 4). Pure data, no core and no threads: the
// emulation thread pushes a state every kRewindIntervalFrames frames and pops the newest while the rewind key is held. The oldest
// state is dropped when the buffer is full (by count, by bytes), so memory stays bounded by the "Rewind length" setting.
#include <QByteArray>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <utility>

namespace uzume::emu {

// One snapshot every this many frames: 15 per second at 60 fps; rewinding steps back one snapshot per frame, so it plays about
// four times as fast backwards as forwards.
inline constexpr int kRewindIntervalFrames = 4;
// Hard memory cap whatever the length setting says.
inline constexpr qint64 kRewindMaxBytes = 1536ll * 1024 * 1024;

class RewindBuffer {
 public:
  RewindBuffer() = default;
  RewindBuffer(int maxCount, qint64 maxBytes) { setLimits(maxCount, maxBytes); }

  // maxCount <= 0 or maxBytes <= 0 disables the buffer (push refuses everything). Shrinking drops the oldest states.
  void setLimits(int maxCount, qint64 maxBytes) {
    maxCount_ = maxCount;
    maxBytes_ = maxBytes;
    trim();
  }
  int maxCount() const { return maxCount_; }
  qint64 maxBytes() const { return maxBytes_; }
  bool enabled() const { return maxCount_ > 0 && maxBytes_ > 0; }

  void clear() {
    items_.clear();
    bytes_ = 0;
  }
  // Appends a state as the newest one and drops old ones until the limits hold again. false: disabled, empty, or the state alone
  // is larger than the byte limit (the buffer is left as it was).
  bool push(QByteArray state, quint64 frame) {
    if (!enabled() || state.isEmpty() || state.size() > maxBytes_) return false;
    bytes_ += state.size();
    items_.push_back({std::move(state), frame});
    trim();
    return true;
  }
  // Removes the newest state. false: empty.
  bool pop(QByteArray* state, quint64* frame = nullptr) {
    if (items_.empty()) return false;
    Item it = std::move(items_.back());
    items_.pop_back();
    bytes_ -= it.data.size();
    if (state != nullptr) *state = std::move(it.data);
    if (frame != nullptr) *frame = it.frame;
    return true;
  }
  int count() const { return static_cast<int>(items_.size()); }
  bool empty() const { return items_.empty(); }
  qint64 bytes() const { return bytes_; }
  // Frame number of the oldest / newest state (0 when empty).
  quint64 oldestFrame() const { return items_.empty() ? 0 : items_.front().frame; }
  quint64 newestFrame() const { return items_.empty() ? 0 : items_.back().frame; }

  // Number of snapshots that hold `seconds` of play at `fps`.
  static int snapshotsFor(double seconds, double fps) {
    if (seconds <= 0 || fps <= 0) return 0;
    return std::max(1, static_cast<int>(std::ceil(seconds * fps / kRewindIntervalFrames)));
  }
  // Seconds of play that `snapshots` states cover.
  static double secondsFor(int snapshots, double fps) {
    return fps > 0 ? snapshots * static_cast<double>(kRewindIntervalFrames) / fps : 0.0;
  }
  // Memory the setting needs for a state of `stateBytes` (0 for off).
  static qint64 estimateBytes(double seconds, double fps, qint64 stateBytes) {
    return static_cast<qint64>(snapshotsFor(seconds, fps)) * std::max<qint64>(0, stateBytes);
  }
  // Limits for a setting: the count the length needs, capped so that count * stateBytes stays within kRewindMaxBytes.
  static std::pair<int, qint64> limitsFor(double seconds, double fps, qint64 stateBytes) {
    const int want = snapshotsFor(seconds, fps);
    if (want <= 0 || stateBytes <= 0) return {0, 0};
    const qint64 fit = kRewindMaxBytes / stateBytes;
    const int count = static_cast<int>(std::min<qint64>(want, fit));
    return {count, count > 0 ? kRewindMaxBytes : 0};
  }

 private:
  struct Item {
    QByteArray data;
    quint64 frame = 0;
  };
  void trim() {
    while (!items_.empty() && (static_cast<int>(items_.size()) > maxCount_ || bytes_ > maxBytes_)) {
      bytes_ -= items_.front().data.size();
      items_.pop_front();
    }
    if (!enabled()) clear();
  }
  std::deque<Item> items_;
  int maxCount_ = 0;
  qint64 maxBytes_ = 0;
  qint64 bytes_ = 0;
};

}  // namespace uzume::emu
