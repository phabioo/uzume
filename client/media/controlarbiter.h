#pragma once

// Host-side arbitration of the controller hand-over (ADR 0033 D8): who may play which port, pending requests, and which
// input frames are applied. Pure logic with an injected clock (milliseconds), no Qt event loop, no network.
//  - one holder per port, one port per viewer; a request for a held port is denied with the reason "busy";
//  - an unanswered request is denied after kControlAskTimeoutMs;
//  - frames are dropped when out of order (16-bit sequence), from a viewer that holds nothing, or stale (they arrive more than
//    kInputStaleMs later than the fastest frame of this grant, judged by the sender's timestamp);
//  - a port without a frame for kInputNeutralMs is neutral (all up) until the next frame; the grant stays.

#include <QHash>
#include <QList>
#include <QString>
#include <optional>

#include "controlproto.h"

namespace uzume {

class ControlArbiter {
 public:
  struct Pending {
    QString viewerId;
    int port = 0;
    qint64 deadlineMs = 0;
  };
  struct Request {
    enum Kind { Pending, Granted, Denied } kind = Denied;
    QString reason;  // denial: unsupported | busy | holding
  };
  struct Answer {
    bool known = false;  // there was a pending request of that viewer
    bool granted = false;
    int port = 0;
  };
  struct End {
    QString viewerId;
    int port = 0;
    QString reason;  // host | viewer | left
  };
  struct Expired {
    QString viewerId;
    int port = 0;
  };
  enum class Verdict { Applied, NoGrant, WrongPort, OutOfOrder, Stale };

  // Ports the host's system offers (1 or 2); 0 = no hand-over. Lowering it ends holders and drops requests above it.
  QList<End> setPortCount(int ports);
  int portCount() const { return ports_; }

  Request request(const QString& viewerId, int port, qint64 nowMs);
  // The host's answer for the oldest pending request of that viewer. With `allow` the viewer becomes the holder (a port that was
  // taken meanwhile cannot happen: pending requests are only accepted for free ports).
  Answer answer(const QString& viewerId, bool allow, qint64 nowMs);
  QList<Expired> expire(qint64 nowMs);
  // The oldest pending request (the one the host's card shows), if any.
  std::optional<Pending> firstPending() const { return pending_.isEmpty() ? std::nullopt : std::optional<Pending>(pending_.first()); }
  const QList<Pending>& pending() const { return pending_; }

  // Ends the hold of a viewer (release, leaving, disconnect) or of a port (take back); also withdraws a pending request.
  std::optional<End> release(const QString& viewerId, const QString& reason);
  std::optional<End> releasePort(int port, const QString& reason);
  QList<End> releaseAll(const QString& reason);

  QString holder(int port) const { return port >= 0 && port < 2 ? holders_[port].viewerId : QString(); }
  int portOf(const QString& viewerId) const;  // -1: holds nothing
  bool hasPending(const QString& viewerId) const;

  Verdict accept(const QString& viewerId, const InputFrame& f, qint64 nowMs);
  // Ports that went neutral because no frame arrived for kInputNeutralMs (each reported once).
  QList<int> expireNeutral(qint64 nowMs);
  // Current input of a port (all zero while neutral or free).
  InputFrame input(int port) const { return port >= 0 && port < 2 ? holders_[port].state : InputFrame(); }
  bool neutral(int port) const { return port >= 0 && port < 2 ? holders_[port].neutral : true; }

 private:
  struct Holder {
    QString viewerId;
    InputFrame state;
    bool haveSeq = false;
    quint16 lastSeq = 0;
    bool haveBase = false;
    int base = 0;  // smallest (arrival - sender ts) seen, mod 65536 as a signed 16-bit offset
    qint64 lastFrameMs = 0;
    bool neutral = true;
  };
  int ports_ = 0;
  QList<Pending> pending_;
  Holder holders_[2];
};

}  // namespace uzume
