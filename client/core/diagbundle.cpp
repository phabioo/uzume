#include "diagbundle.h"
#include "envvar.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>
#include <algorithm>
#include <array>

#include "corecache.h"
#include "hubprotocol.h"
#include "playersettings.h"
#include "profilestore.h"
#include "version.h"

namespace uzume::diag {

namespace {

const QString kRedacted = QStringLiteral("[redacted]");

// Same key list as the Hub: a value behind one of these names is a secret.
const QString kSensitiveKey =
    QStringLiteral("(?:password|passwd|passphrase|secret|token|credential|username|api[_-]?key|invite(?:[_-]?code)?|"
                   "session[_-]?id|csrf|signing[_-]?key|private[_-]?key|cookie|nonce)");

struct Redactor {
  QRegularExpression re;
  QString repl;
};

const std::vector<Redactor>& redactors() {
  static const std::vector<Redactor> list = [] {
    const auto ci = QRegularExpression::CaseInsensitiveOption;
    const auto make = [](const QString& pattern, QRegularExpression::PatternOptions o, const QString& repl) {
      Redactor r{QRegularExpression(pattern, o | QRegularExpression::UseUnicodePropertiesOption), repl};
      r.re.optimize();
      return r;
    };
    const QString value = QStringLiteral(R"re(("(?:[^"\\]|\\.)*"|'[^']*'|[^\s,;&"'}\]]+))re");
    std::vector<Redactor> v;
    // Authorization / Cookie style headers: everything up to the end of the line.
    v.push_back(make(QStringLiteral(R"re(\b((?:proxy-)?authorization|set-cookie|cookie)\b(["']?\s*[:=]\s*)[^\r\n]*)re"), ci,
                     QStringLiteral("\\1\\2") + kRedacted));
    // "Bearer <token>" anywhere.
    v.push_back(make(QStringLiteral(R"re(\b(bearer)\s+[A-Za-z0-9._~+/=-]{8,})re"), ci, QStringLiteral("\\1 ") + kRedacted));
    // key=value, key: value, "key":"value" with a sensitive key name.
    v.push_back(make(QStringLiteral(R"re((\b[a-z0-9_.-]*)re") + kSensitiveKey + QStringLiteral(R"re([a-z0-9_.-]*["']?\s*[:=]\s*))re") + value,
                     ci, QStringLiteral("\\1") + kRedacted));
    // A bare "code" key (invite and pairing codes); status_code etc. are not touched.
    v.push_back(make(QStringLiteral(R"re(((?:^|[?&\s"'])code["']?\s*[:=]\s*))re") + value, ci, QStringLiteral("\\1") + kRedacted));
    // Hub cookie names and token prefixes (fba_ access, fbd_ device, fbp_ poll, fbw_ web session).
    v.push_back(make(QStringLiteral(R"re(((?:__Host-)?fb_(?:session|csrf))=[^\s;,"']+)re"), {}, QStringLiteral("\\1=") + kRedacted));
    v.push_back(make(QStringLiteral(R"re(\bfb[adpw]_[A-Za-z0-9_-]{8,})re"), {}, kRedacted));
    // Invite links carry the code in the URL fragment.
    v.push_back(make(QStringLiteral(R"re((/invite#)[^\s"'<>]+)re"), {}, QStringLiteral("\\1") + kRedacted));
    // user:password@host in URLs and turn(s):user:password@host.
    v.push_back(make(QStringLiteral(R"re((://[^/\s:@]+:)[^@\s/]+@)re"), ci, QStringLiteral("\\1") + kRedacted + QLatin1Char('@')));
    v.push_back(make(QStringLiteral(R"re(\b(turns?:)[^:@\s?/]+:[^@\s?/]+@)re"), ci, QStringLiteral("\\1") + kRedacted + QLatin1Char('@')));
    return v;
  }();
  return list;
}

// Cheap pre-check so the bulk of ordinary log lines skips the regular expressions.
bool mayContainSecret(const QByteArray& line) {
  static const std::array<const char*, 22> needles = {"password", "passwd", "passphrase", "secret", "token", "credential", "username",
                                                      "key",      "invite", "session",    "csrf",   "cookie", "nonce",      "authorization",
                                                      "bearer",   "code",   "fb",         "@",      "turn",   "api",        "private", "signing"};
  const QByteArray lower = line.toLower();
  for (const char* n : needles) {
    if (lower.contains(n)) return true;
  }
  return false;
}

QByteArray redactLine(const QByteArray& line) {
  if (!mayContainSecret(line)) return line;
  QString s = QString::fromUtf8(line);
  for (const Redactor& r : redactors()) {
    s.replace(r.re, r.repl);
  }
  return s.toUtf8();
}

// ---- zip (store + deflate, no new dependency) ----

quint32 crc32(const QByteArray& data) {
  static const std::array<quint32, 256> table = [] {
    std::array<quint32, 256> t{};
    for (quint32 i = 0; i < 256; ++i) {
      quint32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  quint32 c = 0xFFFFFFFFu;
  for (const char ch : data) c = table[(c ^ static_cast<quint8>(ch)) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void put16(QByteArray& b, quint16 v) {
  b.append(static_cast<char>(v & 0xFF));
  b.append(static_cast<char>(v >> 8));
}
void put32(QByteArray& b, quint32 v) {
  put16(b, static_cast<quint16>(v & 0xFFFF));
  put16(b, static_cast<quint16>(v >> 16));
}
quint16 get16(const QByteArray& b, qsizetype o) {
  return static_cast<quint16>(static_cast<quint8>(b[o]) | (static_cast<quint8>(b[o + 1]) << 8));
}
quint32 get32(const QByteArray& b, qsizetype o) { return get16(b, o) | (static_cast<quint32>(get16(b, o + 2)) << 16); }

void dosTime(const QDateTime& t, quint16* time, quint16* date) {
  const QDateTime u = t.toUTC();
  const int year = std::clamp(u.date().year(), 1980, 2107);
  *time = static_cast<quint16>((u.time().hour() << 11) | (u.time().minute() << 5) | (u.time().second() / 2));
  *date = static_cast<quint16>(((year - 1980) << 9) | (u.date().month() << 5) | u.date().day());
}

// Raw deflate via qCompress (zlib stream: 4 byte length, 2 byte header, ..., 4 byte Adler-32). Empty on failure.
QByteArray rawDeflate(const QByteArray& data) {
  const QByteArray z = qCompress(data, 6);
  if (z.size() < 4 + 2 + 4) return {};
  return z.mid(4 + 2, z.size() - 4 - 2 - 4);
}

class ZipWriter {
 public:
  ZipWriter(QIODevice* out, const QDateTime& now) : out_(out) { dosTime(now, &time_, &date_); }

  bool add(const QString& name, const QByteArray& data) {
    const QByteArray nameUtf8 = name.toUtf8();
    const quint32 crc = crc32(data);
    QByteArray body = data;
    quint16 method = 0;
    if (data.size() > 64) {
      const QByteArray d = rawDeflate(data);
      if (!d.isEmpty() && d.size() < data.size()) {
        body = d;
        method = 8;
      }
    }
    QByteArray h;
    put32(h, 0x04034b50);
    put16(h, 20);       // version needed
    put16(h, 0x0800);   // UTF-8 names
    put16(h, method);
    put16(h, time_);
    put16(h, date_);
    put32(h, crc);
    put32(h, static_cast<quint32>(body.size()));
    put32(h, static_cast<quint32>(data.size()));
    put16(h, static_cast<quint16>(nameUtf8.size()));
    put16(h, 0);
    h += nameUtf8;
    Entry e{nameUtf8, method, crc, static_cast<quint32>(body.size()), static_cast<quint32>(data.size()), offset_};
    if (out_->write(h) != h.size() || out_->write(body) != body.size()) return false;
    offset_ += static_cast<quint32>(h.size() + body.size());
    entries_.push_back(e);
    return true;
  }

  bool finish() {
    QByteArray cd;
    for (const Entry& e : entries_) {
      put32(cd, 0x02014b50);
      put16(cd, 20);  // version made by
      put16(cd, 20);
      put16(cd, 0x0800);
      put16(cd, e.method);
      put16(cd, time_);
      put16(cd, date_);
      put32(cd, e.crc);
      put32(cd, e.csize);
      put32(cd, e.size);
      put16(cd, static_cast<quint16>(e.name.size()));
      put16(cd, 0);
      put16(cd, 0);
      put16(cd, 0);
      put16(cd, 0);
      put32(cd, 0);
      put32(cd, e.offset);
      cd += e.name;
    }
    QByteArray end;
    put32(end, 0x06054b50);
    put16(end, 0);
    put16(end, 0);
    put16(end, static_cast<quint16>(entries_.size()));
    put16(end, static_cast<quint16>(entries_.size()));
    put32(end, static_cast<quint32>(cd.size()));
    put32(end, offset_);
    put16(end, 0);
    return out_->write(cd) == cd.size() && out_->write(end) == end.size();
  }

 private:
  struct Entry {
    QByteArray name;
    quint16 method;
    quint32 crc, csize, size, offset;
  };
  QIODevice* out_;
  quint16 time_ = 0, date_ = 0;
  quint32 offset_ = 0;
  std::vector<Entry> entries_;
};

// ---- minimal inflate (after zlib's puff.c, Copyright (C) 2002-2013 Mark Adler, zlib license, altered; full notice in plugins/uzplugin.cpp), for readZip only ----

struct Bits {
  const QByteArray& in;
  qsizetype pos = 0;
  quint32 buf = 0;
  int cnt = 0;
  bool bad = false;
  int get(int need) {
    quint32 v = buf;
    while (cnt < need) {
      if (pos >= in.size()) {
        bad = true;
        return 0;
      }
      v |= static_cast<quint32>(static_cast<quint8>(in[pos++])) << cnt;
      cnt += 8;
    }
    buf = v >> need;
    cnt -= need;
    return static_cast<int>(v & ((1u << need) - 1));
  }
};

struct Huff {
  short count[16]{};
  short symbol[288]{};
};

void buildHuff(Huff* h, const short* length, int n) {
  for (short& c : h->count) c = 0;
  for (int i = 0; i < n; ++i) ++h->count[length[i]];
  short offs[16];
  offs[1] = 0;
  for (int l = 1; l < 15; ++l) offs[l + 1] = static_cast<short>(offs[l] + h->count[l]);
  for (int i = 0; i < n; ++i) {
    if (length[i] != 0) h->symbol[offs[length[i]]++] = static_cast<short>(i);
  }
}

int decodeSym(Bits* s, const Huff& h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= 15; ++len) {
    code |= s->get(1);
    if (s->bad) return -1;
    const int count = h.count[len];
    if (code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}

bool inflateCodes(Bits* s, QByteArray* out, const Huff& lencode, const Huff& distcode) {
  static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for (;;) {
    int sym = decodeSym(s, lencode);
    if (sym < 0) return false;
    if (sym < 256) {
      out->append(static_cast<char>(sym));
    } else if (sym == 256) {
      return true;
    } else {
      sym -= 257;
      if (sym >= 29) return false;
      const int len = lens[sym] + s->get(lext[sym]);
      const int ds = decodeSym(s, distcode);
      if (ds < 0 || ds >= 30) return false;
      const int dist = dists[ds] + s->get(dext[ds]);
      if (s->bad || dist > out->size()) return false;
      for (int i = 0; i < len; ++i) out->append(out->at(out->size() - dist));
    }
  }
}

bool inflateRaw(const QByteArray& in, quint32 expectedSize, QByteArray* out) {
  Bits s{in};
  out->clear();
  out->reserve(static_cast<qsizetype>(expectedSize));
  int last = 0;
  do {
    last = s.get(1);
    const int type = s.get(2);
    if (s.bad) return false;
    if (type == 0) {
      s.buf = 0;
      s.cnt = 0;
      if (s.pos + 4 > in.size()) return false;
      const int len = get16(in, s.pos);
      s.pos += 4;
      if (s.pos + len > in.size()) return false;
      out->append(in.mid(s.pos, len));
      s.pos += len;
    } else if (type == 1) {
      short lengths[288];
      int i = 0;
      for (; i < 144; ++i) lengths[i] = 8;
      for (; i < 256; ++i) lengths[i] = 9;
      for (; i < 280; ++i) lengths[i] = 7;
      for (; i < 288; ++i) lengths[i] = 8;
      Huff lc, dc;
      buildHuff(&lc, lengths, 288);
      for (i = 0; i < 30; ++i) lengths[i] = 5;
      buildHuff(&dc, lengths, 30);
      if (!inflateCodes(&s, out, lc, dc)) return false;
    } else if (type == 2) {
      static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      const int nlen = s.get(5) + 257, ndist = s.get(5) + 1, ncode = s.get(4) + 4;
      if (s.bad || nlen > 286 || ndist > 30) return false;
      short lengths[320]{};
      for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<short>(s.get(3));
      Huff lc, dc;
      buildHuff(&lc, lengths, 19);
      short all[320]{};
      int idx = 0;
      while (idx < nlen + ndist) {
        int sym = decodeSym(&s, lc);
        if (sym < 0) return false;
        if (sym < 16) {
          all[idx++] = static_cast<short>(sym);
        } else {
          int len = 0, rep;
          if (sym == 16) {
            if (idx == 0) return false;
            len = all[idx - 1];
            rep = 3 + s.get(2);
          } else if (sym == 17) {
            rep = 3 + s.get(3);
          } else {
            rep = 11 + s.get(7);
          }
          if (idx + rep > nlen + ndist) return false;
          while (rep--) all[idx++] = static_cast<short>(len);
        }
      }
      buildHuff(&lc, all, nlen);
      buildHuff(&dc, all + nlen, ndist);
      if (!inflateCodes(&s, out, lc, dc)) return false;
    } else {
      return false;
    }
    if (s.bad) return false;
  } while (last == 0);
  return true;
}

// ---- logs ----

struct LogFile {
  QString name, path;
  QDateTime mod;
};

QList<LogFile> logFiles(const QString& dir) {
  QList<LogFile> out;
  const QFileInfoList list = QDir(dir).entryInfoList(QDir::Files | QDir::NoSymLinks | QDir::Readable);
  for (const QFileInfo& fi : list) out.append({fi.fileName(), fi.absoluteFilePath(), fi.lastModified()});
  std::sort(out.begin(), out.end(), [](const LogFile& a, const LogFile& b) {
    return a.mod != b.mod ? a.mod > b.mod : a.name < b.name;
  });
  return out;
}

// Redacts line by line. When the file is larger than `budget`, only its newest lines are used (cut at a line
// boundary) and kCutMarker comes first. *used = source bytes consumed.
QByteArray redactedTail(const QString& path, qint64 budget, qint64* used, QString* err) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    *err = f.errorString();
    return {};
  }
  const qint64 size = f.size();
  QByteArray prefix;
  *used = size;
  if (size > budget) {
    f.seek(size - budget);
    f.readLine();  // drop the cut first line
    prefix = kCutMarker;
    *used = budget;
  }
  QByteArray out = prefix;
  while (!f.atEnd()) {
    const QByteArray line = f.readLine(1 << 20);
    if (line.isEmpty()) break;
    out += redactLine(line);
  }
  return out;
}

const char* kReadme =
    "Uzume diagnostics bundle\n"
    "\n"
    "player-info.json   versions, system, servers (no tokens), installed cores, media encoders, controllers, settings\n"
    "connection-check.json  result of the connection check, only when it was added to the bundle\n"
    "logs/              the app's log files, newest first, and plugin logs (logs/plugins) when they exist\n"
    "\n"
    "This bundle contains IP addresses and host names (they are needed to debug connections), but no passwords or\n"
    "tokens. Removed before packing: TURN username and credential, device tokens, tokens, passwords, invite codes,\n"
    "Authorization headers and cookies. Please look through the files before you share them.\n";

}  // namespace

QByteArray redact(const QByteArray& text) {
  QByteArray out;
  out.reserve(text.size());
  qsizetype pos = 0;
  while (pos < text.size()) {
    qsizetype nl = text.indexOf('\n', pos);
    const qsizetype end = nl < 0 ? text.size() : nl + 1;
    out += redactLine(text.mid(pos, end - pos));
    pos = end;
  }
  return out;
}

QString fileName(const QDateTime& when) {
  return QStringLiteral("uzume-diagnostics-") + when.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'")) +
         QStringLiteral(".zip");
}

QJsonObject offlineInfo(const QString& dataDir) {
  const bool portable = QDir::cleanPath(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"))) ==
                        QDir::cleanPath(dataDir);
  const auto sv = [](std::string_view v) { return QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size())); };
  QJsonObject info;
  info.insert(QStringLiteral("info_version"), kInfoVersion);
  info.insert(QStringLiteral("generated_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  info.insert(QStringLiteral("mode"), QStringLiteral("offline"));
  info.insert(QStringLiteral("player"),
              QJsonObject{{QStringLiteral("version"), sv(playerVersion())},
                          {QStringLiteral("channel"), sv(playerChannel())},
                          {QStringLiteral("commit"), sv(playerCommit())},
                          {QStringLiteral("protocol_version"), kProtocolVersion}});
  const QStorageInfo st(dataDir);
  info.insert(QStringLiteral("system"),
              QJsonObject{{QStringLiteral("os"), QSysInfo::prettyProductName()},
                          {QStringLiteral("kernel"), QSysInfo::kernelType() + QLatin1Char(' ') + QSysInfo::kernelVersion()},
                          {QStringLiteral("arch"), QSysInfo::currentCpuArchitecture()},
                          {QStringLiteral("cpu_threads"), QThread::idealThreadCount()},
                          {QStringLiteral("gpu"), QString()},  // not known without a GL context; the GUI fills it when it is
                          {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
                          {QStringLiteral("data_dir"), QDir::toNativeSeparators(dataDir)},
                          {QStringLiteral("portable"), portable},
                          {QStringLiteral("free_bytes"), st.isValid() ? static_cast<double>(st.bytesAvailable()) : 0.0}});

  // Hubs from profiles.json: no tokens (those live in the credential store), connection state unknown.
  QJsonArray hubs;
  {
    ProfileStore ps(dataDir);
    for (const HubProfile& p : ps.profiles()) {
      hubs.append(QJsonObject{{QStringLiteral("name"), p.name},
                              {QStringLiteral("address"), p.address},
                              {QStringLiteral("hub_version"), QString()},
                              {QStringLiteral("connected"), false},
                              {QStringLiteral("last_connected"), p.lastConnected.isValid() ? p.lastConnected.toUTC().toString(Qt::ISODate) : QString()},
                              {QStringLiteral("features"), QJsonArray()}});
    }
  }
  info.insert(QStringLiteral("hubs"), hubs);

  // Cores found in the core cache of this platform.
  QJsonArray cores;
  {
    const CoreCache cache(QDir(dataDir).filePath(QStringLiteral("cache/cores")));
    const QString platform = CoreCache::currentPlatform();
    for (const QString& id : QDir(cache.root()).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
      const QStringList versions = cache.versions(id, platform);
      if (!versions.isEmpty()) {
        cores.append(QJsonObject{{QStringLiteral("core"), id}, {QStringLiteral("version"), versions.first()}});
      }
    }
  }
  info.insert(QStringLiteral("emulation"),
              QJsonObject{{QStringLiteral("cores"), cores},
                          {QStringLiteral("hw_render"), uzume::env::get("UZUME_DISABLE_HW_RENDER") != QLatin1String("1")},
                          {QStringLiteral("gl_renderer"), QString()},
                          {QStringLiteral("gl_version"), QString()}});
  info.insert(QStringLiteral("media"),
              QJsonObject{{QStringLiteral("encoders_available"), QJsonArray()},
                          {QStringLiteral("encoder_last_chosen"), QString()},
                          {QStringLiteral("forced_encoder"), uzume::env::get("UZUME_H264_ENCODER")}});
  info.insert(QStringLiteral("controllers"), QJsonObject{{QStringLiteral("connected"), QJsonArray()}});

  const PlayerSettings s(dataDir);
  QJsonObject settings{{QStringLiteral("appearance"), PlayerSettings::appearanceName(s.appearance())},
                       {QStringLiteral("update_channel"), s.updateChannel().isEmpty() ? QStringLiteral("default") : s.updateChannel()},
                       {QStringLiteral("update_auto_install"), s.updateAutoInstall().has_value() ? QJsonValue(*s.updateAutoInstall()) : QJsonValue(QStringLiteral("default"))},
                       {QStringLiteral("developer_mode"), s.developerMode()},
                       {QStringLiteral("rom_cache_limit_bytes"), static_cast<double>(s.romCacheLimitBytes())},
                       {QStringLiteral("force_relay"), uzume::env::get("UZUME_FORCE_RELAY") == QLatin1String("1")},
                       {QStringLiteral("disable_gpu_encode"), uzume::env::get("UZUME_DISABLE_GPU_ENCODE") == QLatin1String("1")}};
  info.insert(QStringLiteral("settings"), settings);
  return info;
}

bool writeBundle(const QString& outPath, const QJsonObject& info, const QString& dataDir, const Options& options, QString* error) {
  const auto fail = [error](const QString& m) {
    if (error != nullptr) *error = m;
    return false;
  };
  const QDateTime now = options.now.isValid() ? options.now : QDateTime::currentDateTimeUtc();
  QSaveFile out(outPath);
  if (!out.open(QIODevice::WriteOnly)) return fail(out.errorString());
  ZipWriter zip(&out, now);
  if (!zip.add(QString::fromLatin1(kEntryReadme), QByteArray(kReadme))) return fail(QStringLiteral("write failed"));
  const QByteArray json = redact(QJsonDocument(info).toJson(QJsonDocument::Indented));
  if (!zip.add(QString::fromLatin1(kEntryInfo), json)) return fail(QStringLiteral("write failed"));
  if (!options.connectionCheckJson.isEmpty() &&
      !zip.add(QString::fromLatin1(kEntryConnectionCheck), redact(options.connectionCheckJson))) {
    return fail(QStringLiteral("write failed"));
  }

  qint64 budget = options.maxLogBytes > 0 ? options.maxLogBytes : kDefaultMaxLogBytes;
  const QString logDir = options.logDir.isEmpty() ? QDir(dataDir).filePath(QStringLiteral("logs")) : options.logDir;
  const auto addLogs = [&](const QString& dir, const QString& prefix) {
    for (const LogFile& f : logFiles(dir)) {
      if (budget <= 0) break;
      qint64 used = 0;
      QString err;
      QByteArray content = redactedTail(f.path, budget, &used, &err);
      if (!err.isEmpty()) content += QByteArray("\n[could not read this file completely: ") + err.toUtf8() + "]\n";
      budget -= used;
      if (!zip.add(prefix + f.name, content)) return false;
    }
    return true;
  };
  if (!addLogs(logDir, QString::fromLatin1(kLogsPrefix))) return fail(QStringLiteral("write failed"));
  if (!addLogs(QDir(logDir).filePath(QStringLiteral("plugins")), QString::fromLatin1(kPluginLogsPrefix))) return fail(QStringLiteral("write failed"));
  if (!zip.finish() || !out.commit()) return fail(out.errorString());
  return true;
}

bool writeZip(const QString& outPath, const QList<QPair<QString, QByteArray>>& entries, const QDateTime& now, QString* error) {
  const auto fail = [error](const QString& m) {
    if (error != nullptr) *error = m;
    return false;
  };
  QSaveFile out(outPath);
  if (!out.open(QIODevice::WriteOnly)) return fail(out.errorString());
  ZipWriter zip(&out, now.isValid() ? now : QDateTime::currentDateTimeUtc());
  for (const auto& e : entries) {
    if (!zip.add(e.first, e.second)) return fail(QStringLiteral("write failed"));
  }
  if (!zip.finish() || !out.commit()) return fail(out.errorString());
  return true;
}

namespace {
// The timestamp a log line starts with (filelog::formatLine: local time, ISO with milliseconds); invalid for other lines.
QDateTime lineTime(const QByteArray& line) {
  if (line.size() < 23 || line[4] != '-' || line[10] != 'T') return {};
  return QDateTime::fromString(QString::fromLatin1(line.constData(), 23), Qt::ISODateWithMs);
}
}  // namespace

QByteArray recentLogs(const QString& logDir, int days, qint64 maxBytes, const QDateTime& now) {
  const QDateTime cutoff = (now.isValid() ? now : QDateTime::currentDateTime()).addDays(-days);
  QByteArray out;
  qint64 budget = maxBytes;
  for (const LogFile& f : logFiles(logDir)) {
    if (budget <= 0) break;
    if (f.mod < cutoff) break;  // newest first: every further file is older
    QFile file(f.path);
    if (!file.open(QIODevice::ReadOnly)) continue;
    QByteArray body;
    while (!file.atEnd()) {
      const QByteArray line = file.readLine(1 << 20);
      if (line.isEmpty()) break;
      const QDateTime t = lineTime(line);
      if (t.isValid() && t < cutoff) continue;
      body += redactLine(line);
    }
    if (body.isEmpty()) continue;
    const QByteArray header = "=== " + f.name.toUtf8() + " ===\n";
    if (header.size() + body.size() <= budget) {
      out += header + body;
      budget -= header.size() + body.size();
      continue;
    }
    // Does not fit: its newest lines, cut at a line boundary, after the marker.
    const qint64 room = budget - header.size() - qstrlen(kCutMarker);
    if (room > 0) {
      QByteArray tail = body.right(room);
      const qsizetype nl = tail.indexOf('\n');
      tail = nl >= 0 ? tail.mid(nl + 1) : QByteArray();
      out += header + kCutMarker + tail;
    }
    break;
  }
  return out;
}

bool readZip(const QString& path, QMap<QString, QByteArray>* entries, QString* error) {
  const auto fail = [error](const QString& m) {
    if (error != nullptr) *error = m;
    return false;
  };
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return fail(f.errorString());
  const QByteArray b = f.readAll();
  const qsizetype eocd = b.lastIndexOf(QByteArray("PK\x05\x06", 4));
  if (eocd < 0 || eocd + 22 > b.size()) return fail(QStringLiteral("no end of central directory"));
  const int count = get16(b, eocd + 10);
  qsizetype p = get32(b, eocd + 16);
  for (int i = 0; i < count; ++i) {
    if (p + 46 > b.size() || get32(b, p) != 0x02014b50) return fail(QStringLiteral("bad central directory"));
    const quint16 method = get16(b, p + 10);
    const quint32 crc = get32(b, p + 16), csize = get32(b, p + 20), size = get32(b, p + 24);
    const int nlen = get16(b, p + 28), xlen = get16(b, p + 30), clen = get16(b, p + 32);
    const qsizetype lho = get32(b, p + 42);
    const QString name = QString::fromUtf8(b.mid(p + 46, nlen));
    p += 46 + nlen + xlen + clen;
    if (lho + 30 > b.size() || get32(b, lho) != 0x04034b50) return fail(QStringLiteral("bad local header"));
    const qsizetype data = lho + 30 + get16(b, lho + 26) + get16(b, lho + 28);
    if (data + csize > b.size()) return fail(QStringLiteral("truncated entry"));
    QByteArray raw = b.mid(data, csize), content;
    if (method == 0) {
      content = raw;
    } else if (method == 8) {
      if (!inflateRaw(raw, size, &content)) return fail(QStringLiteral("cannot inflate %1").arg(name));
    } else {
      return fail(QStringLiteral("unsupported method"));
    }
    if (static_cast<quint32>(content.size()) != size || crc32(content) != crc) return fail(QStringLiteral("CRC mismatch in %1").arg(name));
    entries->insert(name, content);
  }
  return true;
}

}  // namespace uzume::diag
