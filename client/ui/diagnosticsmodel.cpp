#include "diagnosticsmodel.h"

#include <QCoreApplication>
#include <QStringList>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace uzume::ui {

namespace {

QString dash() { return QStringLiteral("—"); }

QString fixed(double v, int decimals) { return QString::number(v, 'f', decimals); }

QVariantList toList(const QVector<float>& v) {
  QVariantList out;
  out.reserve(v.size());
  for (const float f : v) {
    out.append(static_cast<double>(f));
  }
  return out;
}

}  // namespace

void DiagnosticsModel::setSettings(PlayerSettings* settings) {
  settings_ = settings;
  if (settings_) {
    open_ = settings_->diagnosticsOpen();
    emulationOpen_ = settings_->diagnosticsEmulationOpen();
    streamingOpen_ = settings_->diagnosticsStreamingOpen();
    emit statesChanged();
  }
}

void DiagnosticsModel::setOpen(bool on) {
  if (on == open_) {
    return;
  }
  open_ = on;
  if (settings_) settings_->setDiagnosticsOpen(on);
  emit statesChanged();
}

void DiagnosticsModel::setEmulationOpen(bool on) {
  if (on == emulationOpen_) {
    return;
  }
  emulationOpen_ = on;
  if (settings_) settings_->setDiagnosticsEmulationOpen(on);
  emit statesChanged();
}

void DiagnosticsModel::setStreamingOpen(bool on) {
  if (on == streamingOpen_) {
    return;
  }
  streamingOpen_ = on;
  if (settings_) settings_->setDiagnosticsStreamingOpen(on);
  emit statesChanged();
}

// ---------------------------------------------------------------- feeding

void DiagnosticsModel::setEmulation(const EmulationDiagnostics& d) {
  const QVariantMap map = d.valid ? emulationMap(d) : QVariantMap();
  const QString summary = d.valid ? map.value(QStringLiteral("summary")).toString() : tr("No game");
  if (map == emulation_ && summary == emulationSummary_) {
    return;
  }
  emulation_ = map;
  emulationSummary_ = summary;
  emit dataChanged();
}

void DiagnosticsModel::setTiles(const QVariantList& tiles) {
  QString summary = tr("No local game");
  for (const QVariant& t : tiles) {
    const QVariantMap m = t.toMap();
    if (m.value(QStringLiteral("local")).toBool()) {
      summary = tr("Tile %1 · %2").arg(m.value(QStringLiteral("index")).toInt()).arg(m.value(QStringLiteral("emulation")).toMap().value(QStringLiteral("summary")).toString());
      break;
    }
  }
  if (tiles == tiles_ && summary == tilesSummary_) {
    return;
  }
  tiles_ = tiles;
  tilesSummary_ = summary;
  emit dataChanged();
}

void DiagnosticsModel::setStreaming(const QVariantList& groups) {
  int n = 0, relayed = 0;
  for (const QVariant& g : groups) {
    for (const QVariant& p : g.toMap().value(QStringLiteral("participants")).toList()) {
      ++n;
      relayed += p.toMap().value(QStringLiteral("relayed")).toBool() ? 1 : 0;
    }
  }
  const QString summary = n == 0 ? tr("No session") : tr("%1 · %2 relayed").arg(n).arg(relayed);
  if (groups == streaming_ && summary == streamingSummary_) {
    return;
  }
  streaming_ = groups;
  participants_ = n;
  streamingSummary_ = summary;
  emit dataChanged();
}

// ---------------------------------------------------------------- formatting

QVariantMap DiagnosticsModel::emulationMap(const EmulationDiagnostics& d) {
  QVariantMap m;
  const bool fallback = d.hwRequested && !d.hwActive;
  const bool hw = d.hwActive;
  m.insert(QStringLiteral("valid"), true);
  m.insert(QStringLiteral("core"), d.core.isEmpty() ? dash() : d.core);

  // Renderer
  QString renderer = hw ? (d.api.isEmpty() ? tr("OpenGL") : d.api) : tr("Software");
  QString rendererSub;
  if (hw) {
    rendererSub = d.gpu;
    if (!d.vulkanFallback.isEmpty()) {
      rendererSub = rendererSub.isEmpty() ? tr("Vulkan requested · not used (%1)").arg(d.vulkanFallback)
                                          : tr("%1 · Vulkan requested, not used (%2)").arg(rendererSub, d.vulkanFallback);
    }
  } else if (fallback) {
    rendererSub = tr("requested: OpenGL");
  } else {
    rendererSub = d.cpuThreads > 0 ? tr("CPU · %1 threads").arg(d.cpuThreads) : tr("CPU");
  }
  m.insert(QStringLiteral("renderer"), renderer);
  m.insert(QStringLiteral("rendererSub"), rendererSub);
  m.insert(QStringLiteral("fallback"), fallback);
  m.insert(QStringLiteral("fallbackText"),
           fallback ? (d.fallbackReason.isEmpty() ? tr("OpenGL requested · fell back to software")
                                                  : tr("OpenGL requested · fell back to software (%1)").arg(d.fallbackReason))
                    : QString());

  // Resolution: scale and pixels; the per-screen size, or what the core asked for while software runs (D10).
  QString resolution = dash(), resolutionSub;
  if (!d.frameSize.isEmpty()) {
    const int scale = d.baseSize.width() > 0 ? std::max(1, static_cast<int>(std::lround(static_cast<double>(d.frameSize.width()) / d.baseSize.width()))) : 1;
    resolution = tr("%1× · %2×%3").arg(scale).arg(d.frameSize.width()).arg(d.frameSize.height());
    if (!d.readbackSize.isEmpty() && d.readbackSize != d.frameSize) {
      resolution += tr(" → read back %1×%2").arg(d.readbackSize.width()).arg(d.readbackSize.height());
    } else if (d.zeroCopy && !d.displaySize.isEmpty() && d.displaySize != d.frameSize) {  // zero-copy display, no readback running
      resolution += tr(" → shown %1×%2").arg(d.displaySize.width()).arg(d.displaySize.height());
    }
    if (fallback && d.requestedScale > 1) {
      resolutionSub = tr("%1× requested · needs OpenGL").arg(d.requestedScale);
    } else if (d.screens > 1) {
      // Equal screens: "2 screens of W×H"; different ones (3DS) name each size.
      QList<QSize> sizes;
      for (const QSize& s : d.screenSizes) sizes.append(s * scale);
      bool equal = true;
      for (const QSize& s : sizes) equal = equal && s == sizes.first();
      if (sizes.size() == d.screens && !equal && d.screens == 2)
        resolutionSub = tr("Top %1×%2 · bottom %3×%4").arg(sizes[0].width()).arg(sizes[0].height()).arg(sizes[1].width()).arg(sizes[1].height());
      else if (sizes.size() == d.screens && !equal) {
        QStringList parts;
        for (const QSize& s : sizes) parts << tr("%1×%2").arg(s.width()).arg(s.height());
        resolutionSub = parts.join(QStringLiteral(" · "));
      } else if (sizes.size() == d.screens)
        resolutionSub = tr("%1 screens of %2×%3").arg(d.screens).arg(sizes.first().width()).arg(sizes.first().height());
      else
        resolutionSub = tr("%1 screens of %2×%3").arg(d.screens).arg(d.frameSize.width()).arg(d.frameSize.height() / d.screens);
    }
  }
  m.insert(QStringLiteral("resolution"), resolution);
  m.insert(QStringLiteral("resolutionSub"), resolutionSub);

  // FPS actual / core target
  const bool running = d.fps > 0.0;
  const QString actual = running ? fixed(d.fps, 1) : dash();
  m.insert(QStringLiteral("fps"), d.targetFps > 0 ? tr("%1 / %2 fps").arg(actual, fixed(d.targetFps, 2)) : tr("%1 fps").arg(actual));

  // Frame time split
  QString frame = dash();
  if (running) {
    // Zero-copy display (ADR 0027): the readback part drops out while none runs, a zero-copy part takes its place.
    const bool zeroCopy = hw && d.displaysPerSec > 0.0;
    if (zeroCopy && d.readbacksPerSec <= 0.0) {
      frame = tr("%1 ms · emu %2").arg(fixed(d.frameMs, 1), fixed(d.emuMs, 1));
    } else {
      frame = hw ? tr("%1 ms · emu %2 · readback %3 (%4/s)")
                       .arg(fixed(d.frameMs, 1), fixed(d.emuMs, 1), fixed(d.readbackMs, 1), QString::number(qRound(d.readbacksPerSec)))
                 : tr("%1 ms · emu %2 · no readback").arg(fixed(d.frameMs, 1), fixed(d.emuMs, 1));
    }
    if (zeroCopy) {
      frame += tr(" · zero-copy %1 (%2/s)").arg(fixed(d.displayMs, 1), QString::number(qRound(d.displaysPerSec)));
    }
    if (hw && d.gpuCopiesPerSec > 0.0) {  // GPU-direct Session encoding (ADR 0019): the copy into the encoder's texture
      frame += tr(" · GPU copy %1 (%2/s)").arg(fixed(d.gpuCopyMs, 1), QString::number(qRound(d.gpuCopiesPerSec)));
    }
  }
  m.insert(QStringLiteral("frame"), frame);

  // Audio
  QString audio, audioSub;
  if (!d.audioActive) {
    audio = tr("No audio output");
  } else {
    audio = d.underruns == 1 ? tr("Buffer %1 ms · 1 underrun").arg(qRound(d.audioBufferMs))
                             : tr("Buffer %1 ms · %2 underruns").arg(qRound(d.audioBufferMs)).arg(d.underruns);
    if (d.underruns > 0) {
      audioSub = tr("the audio output ran dry · check the CPU load");
    }
  }
  m.insert(QStringLiteral("audio"), audio);
  m.insert(QStringLiteral("audioSub"), audioSub);

  // Sparkline data: total and emulation time of the last 5 s, reference = the core's frame time
  QVariantMap spark;
  spark.insert(QStringLiteral("total"), toList(d.totalHistory));
  spark.insert(QStringLiteral("emu"), toList(d.emuHistory));
  spark.insert(QStringLiteral("targetMs"), d.targetFps > 0 ? 1000.0 / d.targetFps : 16.7);
  m.insert(QStringLiteral("spark"), spark);

  m.insert(QStringLiteral("summary"), running ? tr("%1 fps · %2 ms").arg(fixed(d.fps, 1), fixed(d.frameMs, 1)) : tr("not running"));

  // Multiview tile text: one wrapped mono line each
  QStringList lines;
  lines << (d.core.isEmpty() ? dash() : d.core);
  lines << (renderer + (d.frameSize.isEmpty() ? QString() : QStringLiteral(" · ") + resolution));
  lines << m.value(QStringLiteral("fps")).toString();
  lines << tr("frame %1").arg(frame);
  lines << (d.audioActive ? (d.underruns == 1 ? tr("audio %1 ms · 1 underrun").arg(qRound(d.audioBufferMs))
                                             : tr("audio %1 ms · %2 underruns").arg(qRound(d.audioBufferMs)).arg(d.underruns))
                          : tr("no audio output"));
  m.insert(QStringLiteral("lines"), lines);
  return m;
}

DiagnosticsModel::Pill DiagnosticsModel::connectionPill(const QString& type) {
  if (type.isEmpty() || type == QLatin1String("unknown")) {
    return {tr("Connecting"), QStringLiteral("neutral")};
  }
  if (type.startsWith(QLatin1String("relay"))) {
    return type.contains(QLatin1String("tcp"), Qt::CaseInsensitive) ? Pill{tr("Relayed (TURN/TCP)"), QStringLiteral("warn")}
                                                                    : Pill{tr("Relayed (TURN)"), QStringLiteral("warn")};
  }
  return {tr("Direct"), QStringLiteral("ok")};  // direct (host | srflx | prflx), or a bare candidate type
}

QString DiagnosticsModel::mbit(double kbps) { return fixed(kbps / 1000.0, 1); }

QString DiagnosticsModel::hostLine(const SessionStats& s) {
  if (s.encoderName.isEmpty()) {
    return tr("Encoder starts with the first viewer");
  }
  QString rate = mbit(s.videoBitrateKbps);
  if (s.targetBitrateKbps > 0) {
    rate = tr("%1 / target %2").arg(rate, mbit(s.targetBitrateKbps));
  }
  QString line = tr("Encoder %1 · H.264 · %2 Mbit/s · %3 fps").arg(s.encoderName, rate, fixed(s.fps, 1));
  if (s.gpuInput) {  // ADR 0019: h264_nvenc takes CUDA frames straight from the GPU
    line += tr(" · GPU-direct");
  } else if (s.gpuInputFailed) {
    line += tr(" · readback (GPU-direct off)");
  }
  return line;
}

QString DiagnosticsModel::hostSendingLine(int viewers) {
  return viewers == 1 ? tr("Sending to 1 viewer · adaptive bitrate") : tr("Sending to %1 viewers · adaptive bitrate").arg(viewers);
}

QString DiagnosticsModel::decoderLine(const QString& decoder, std::optional<double> kbps, std::optional<double> fps) {
  return tr("Decoder %1 · H.264 · %2 · %3")
      .arg(decoder.isEmpty() ? dash() : decoder,
           kbps ? tr("%1 Mbit/s").arg(mbit(*kbps)) : dash(),
           fps ? tr("%1 fps").arg(fixed(*fps, 1)) : dash());
}

QString DiagnosticsModel::linkLine(std::optional<double> rttMs, std::optional<double> lossPercent, const QString& connectionType,
                                   const QString& turnEndpoint, RelayReason reason) {
  QStringList parts;
  parts << (rttMs ? tr("RTT %1 ms").arg(qRound(*rttMs)) : tr("RTT —"));
  parts << (lossPercent ? tr("Loss %1 %").arg(fixed(*lossPercent, 1)) : tr("Loss —"));
  if (isRelayed(connectionType) && !turnEndpoint.isEmpty()) {
    parts << tr("via %1").arg(turnEndpoint);
  }
  if (isRelayed(connectionType) && reason != RelayReason::None) {
    parts << relayReasonText(reason);
  }
  return parts.join(QStringLiteral(" · "));
}

QString DiagnosticsModel::relayReasonText(RelayReason reason) {
  switch (reason) {
    case RelayReason::Forced: return tr("relay forced");
    case RelayReason::NoLocalSrflx: return tr("no STUN reachable on this side");
    case RelayReason::NoRemoteSrflx: return tr("other side has no public address");
    case RelayReason::DirectFailed: return tr("direct path blocked (NAT or firewall)");
    case RelayReason::None: break;
  }
  return {};
}

QString DiagnosticsModel::turnEndpoint(const QList<TurnServer>& servers) {
  for (const TurnServer& t : servers) {
    for (const QString& u : t.urls) {
      // turn:host:port?transport=udp (not a real URL authority: "turn:" has no "//")
      QString rest = u.section(QLatin1Char(':'), 1);
      rest = rest.section(QLatin1Char('?'), 0, 0);
      if (!rest.isEmpty()) {
        return rest;
      }
    }
  }
  return {};
}

QVariantMap DiagnosticsModel::participant(const QString& name, const QString& role, const Pill& pill, const QString& line2, const QString& line3,
                                          const QVariantMap& link) {
  QVariantMap m{{QStringLiteral("name"), name},
                {QStringLiteral("role"), role},
                {QStringLiteral("pill"), pill.text},
                {QStringLiteral("tone"), pill.tone},
                {QStringLiteral("relayed"), pill.tone == QLatin1String("warn")},
                {QStringLiteral("line2"), line2},
                {QStringLiteral("line3"), line3}};
  m.insert(link);
  return m;
}

QVariantMap DiagnosticsModel::linkFields(std::optional<double> rttMs, std::optional<double> lossPercent, const QString& connectionType,
                                         RelayReason reason) {
  const bool known = !connectionType.isEmpty() && connectionType != QLatin1String("unknown");
  const bool relayed = isRelayed(connectionType);
  return QVariantMap{{QStringLiteral("path"), !known ? QString() : (relayed ? tr("Relay") : tr("Direct"))},
                     {QStringLiteral("rtt"), rttMs ? tr("%1 ms").arg(qRound(*rttMs)) : QString()},
                     {QStringLiteral("loss"), lossPercent ? tr("%1 %").arg(fixed(*lossPercent, 1)) : QString()},
                     {QStringLiteral("reason"), relayed ? relayReasonSentence(reason) : QString()}};
}

QString DiagnosticsModel::relayReasonSentence(RelayReason reason) {
  switch (reason) {
    case RelayReason::Forced: return tr("Relay because it is forced on this PC.");
    case RelayReason::NoLocalSrflx: return tr("Relay because no STUN server could be reached from this PC.");
    case RelayReason::NoRemoteSrflx: return tr("Relay because the other side has no public address.");
    case RelayReason::DirectFailed: return tr("Relay because no direct path was found: strict NAT or a firewall.");
    case RelayReason::None: break;
  }
  return {};
}

namespace {
QVariantList detailRows(const QString& codecLabel, const QString& codecValue, const SessionStats& s) {
  QVariantList rows;
  if (!codecValue.isEmpty()) {
    rows.append(QVariant(QVariantList{codecLabel, codecValue}));
  }
  if (s.videoBitrateKbps > 0.0) {
    rows.append(QVariant(QVariantList{QCoreApplication::translate("uzume::ui::DiagnosticsModel", "Bitrate"),
                                      QCoreApplication::translate("uzume::ui::DiagnosticsModel", "%1 Mbit/s").arg(DiagnosticsModel::mbit(s.videoBitrateKbps))}));
  }
  if (s.width > 0 && s.height > 0) {
    QString v = QStringLiteral("%1 × %2").arg(s.width).arg(s.height);
    if (s.fps > 0.0) {
      v += QCoreApplication::translate("uzume::ui::DiagnosticsModel", " · %1 fps").arg(qRound(s.fps));
    }
    rows.append(QVariant(QVariantList{QCoreApplication::translate("uzume::ui::DiagnosticsModel", "Resolution"), v}));
  }
  return rows;
}
}  // namespace

QVariantList DiagnosticsModel::hostDetailRows(const SessionStats& s) {
  QString enc = s.encoderName;
  if (!enc.isEmpty()) {
    enc += QStringLiteral(" · H.264");
    if (s.gpuInput) {
      enc += tr(" · GPU-direct");
    }
  }
  return detailRows(tr("Encoder"), enc, s);
}

QVariantList DiagnosticsModel::viewerDetailRows(const SessionStats& s) {
  return detailRows(tr("Decoder"), s.decoderName.isEmpty() ? QString() : s.decoderName + QStringLiteral(" · H.264"), s);
}

}  // namespace uzume::ui
