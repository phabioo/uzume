#include "controlarbiter.h"

namespace uzume {

QList<ControlArbiter::End> ControlArbiter::setPortCount(int ports) {
  ports_ = ports < 0 ? 0 : (ports > 2 ? 2 : ports);
  QList<End> ended;
  for (int p = ports_; p < 2; ++p) {
    if (const auto e = releasePort(p, QStringLiteral("host"))) ended.append(*e);
  }
  for (int i = pending_.size() - 1; i >= 0; --i) {
    if (pending_.at(i).port >= ports_) pending_.removeAt(i);
  }
  return ended;
}

int ControlArbiter::portOf(const QString& viewerId) const {
  for (int p = 0; p < 2; ++p) {
    if (!viewerId.isEmpty() && holders_[p].viewerId == viewerId) return p;
  }
  return -1;
}

bool ControlArbiter::hasPending(const QString& viewerId) const {
  for (const Pending& p : pending_) {
    if (p.viewerId == viewerId) return true;
  }
  return false;
}

ControlArbiter::Request ControlArbiter::request(const QString& viewerId, int port, qint64 nowMs) {
  Request r;
  if (viewerId.isEmpty() || port < 0 || port >= ports_) {
    r.reason = QStringLiteral("unsupported");
    return r;
  }
  if (portOf(viewerId) >= 0 || hasPending(viewerId)) {
    r.reason = QStringLiteral("holding");
    return r;
  }
  if (!holders_[port].viewerId.isEmpty()) {
    r.reason = QStringLiteral("busy");
    return r;
  }
  for (const Pending& p : pending_) {
    if (p.port == port) {  // somebody else asked first and waits for the host
      r.reason = QStringLiteral("busy");
      return r;
    }
  }
  pending_.append({viewerId, port, nowMs + kControlAskTimeoutMs});
  r.kind = Request::Pending;
  return r;
}

ControlArbiter::Answer ControlArbiter::answer(const QString& viewerId, bool allow, qint64 nowMs) {
  Answer a;
  for (int i = 0; i < pending_.size(); ++i) {
    if (pending_.at(i).viewerId != viewerId) continue;
    const Pending p = pending_.takeAt(i);
    a.known = true;
    a.port = p.port;
    if (allow && p.port < ports_ && holders_[p.port].viewerId.isEmpty()) {
      Holder h;
      h.viewerId = viewerId;
      h.lastFrameMs = nowMs;
      h.state.port = static_cast<quint8>(p.port);
      holders_[p.port] = h;
      a.granted = true;
    }
    return a;
  }
  return a;
}

QList<ControlArbiter::Expired> ControlArbiter::expire(qint64 nowMs) {
  QList<Expired> out;
  for (int i = 0; i < pending_.size();) {
    if (nowMs >= pending_.at(i).deadlineMs) {
      out.append({pending_.at(i).viewerId, pending_.at(i).port});
      pending_.removeAt(i);
    } else {
      ++i;
    }
  }
  return out;
}

std::optional<ControlArbiter::End> ControlArbiter::release(const QString& viewerId, const QString& reason) {
  for (int i = 0; i < pending_.size(); ++i) {
    if (pending_.at(i).viewerId == viewerId) {
      pending_.removeAt(i);
      break;
    }
  }
  const int p = portOf(viewerId);
  if (p < 0) return std::nullopt;
  return releasePort(p, reason);
}

std::optional<ControlArbiter::End> ControlArbiter::releasePort(int port, const QString& reason) {
  if (port < 0 || port > 1 || holders_[port].viewerId.isEmpty()) return std::nullopt;
  const End e{holders_[port].viewerId, port, reason};
  holders_[port] = Holder();
  return e;
}

QList<ControlArbiter::End> ControlArbiter::releaseAll(const QString& reason) {
  QList<End> out;
  for (int p = 0; p < 2; ++p) {
    if (const auto e = releasePort(p, reason)) out.append(*e);
  }
  pending_.clear();
  return out;
}

ControlArbiter::Verdict ControlArbiter::accept(const QString& viewerId, const InputFrame& f, qint64 nowMs) {
  const int p = portOf(viewerId);
  if (p < 0) return Verdict::NoGrant;
  if (f.port != p) return Verdict::WrongPort;
  Holder& h = holders_[p];
  if (h.haveSeq && static_cast<qint16>(static_cast<quint16>(f.seq - h.lastSeq)) <= 0) return Verdict::OutOfOrder;
  const int offset = static_cast<qint16>(static_cast<quint16>(static_cast<quint16>(nowMs & 0xFFFF) - f.tsMs));
  if (!h.haveBase) {
    h.haveBase = true;
    h.base = offset;
  }
  int delta = static_cast<qint16>(static_cast<quint16>(offset - h.base));
  if (delta < 0) {  // a faster path than seen so far
    h.base = offset;
    delta = 0;
  }
  if (delta > kInputStaleMs) return Verdict::Stale;
  h.haveSeq = true;
  h.lastSeq = f.seq;
  h.state = f;
  h.lastFrameMs = nowMs;
  h.neutral = false;
  return Verdict::Applied;
}

QList<int> ControlArbiter::expireNeutral(qint64 nowMs) {
  QList<int> out;
  for (int p = 0; p < 2; ++p) {
    Holder& h = holders_[p];
    if (h.viewerId.isEmpty() || h.neutral || nowMs - h.lastFrameMs < kInputNeutralMs) continue;
    h.neutral = true;
    h.state = InputFrame();
    h.state.port = static_cast<quint8>(p);
    out.append(p);
  }
  return out;
}

}  // namespace uzume
