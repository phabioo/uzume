#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <map>

#include "format_helpers.h"

namespace uzume::formats {

namespace {

// "/a//b" -> "a/b"; empty when the path leaves the folder ("..") or holds a backslash / NUL.
bool cleanRel(std::string_view in, QString* out) {
    QStringList parts;
    size_t i = 0;
    while (i <= in.size()) {
        const size_t j = in.find('/', i);
        const std::string_view seg = in.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i);
        if (seg == "..") return false;
        if (seg.find('\\') != std::string_view::npos || seg.find('\0') != std::string_view::npos) return false;
        if (!seg.empty() && seg != ".") parts << QString::fromUtf8(seg.data(), static_cast<qsizetype>(seg.size()));
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    *out = parts.join(QLatin1Char('/'));
    return true;
}

}  // namespace

Result romfsOverlayRead(const QString& romPath, const QString& modDir, std::string_view path, size_t maxOut) {
    if (modDir.isEmpty()) return romfsRead(romPath, path, maxOut);
    QString rel;
    if (cleanRel(path, &rel) && !rel.isEmpty()) {
        const QString file = modDir + QStringLiteral("/romfs/") + rel;
        const QFileInfo fi(file);
        if (fi.isFile() && !fi.isSymLink()) {
            Result r;
            if (static_cast<quint64>(fi.size()) > maxOut) {
                r.error = "file too large";
                return r;
            }
            QFile f(file);
            if (!f.open(QIODevice::ReadOnly)) {
                r.error = "cannot read the mod file";
                return r;
            }
            const QByteArray b = f.readAll();
            r.ok = true;
            r.data.assign(b.constData(), static_cast<size_t>(b.size()));
            return r;
        }
    }
    return romfsRead(romPath, path, maxOut);
}

std::vector<RomfsEntry> romfsOverlayList(const QString& romPath, const QString& modDir, std::string_view dir, std::string* error) {
    if (modDir.isEmpty()) return romfsList(romPath, dir, error);
    std::string baseErr;
    std::vector<RomfsEntry> out = romfsList(romPath, dir, &baseErr);
    QString rel;
    bool modHas = false;
    if (cleanRel(dir, &rel)) {
        const QString d = modDir + QStringLiteral("/romfs") + (rel.isEmpty() ? QString() : QStringLiteral("/") + rel);
        if (QFileInfo(d).isDir()) {
            std::map<std::string, RomfsEntry> mod;
            for (const QFileInfo& e : QDir(d).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name)) {
                if (e.isSymLink()) continue;
                RomfsEntry re;
                re.name = e.fileName().toStdString();
                re.dir = e.isDir();
                re.size = e.isDir() ? 0 : static_cast<uint64_t>(e.size());
                mod[re.name] = re;
            }
            modHas = true;
            for (RomfsEntry& b : out) {
                const auto it = mod.find(b.name);
                if (it == mod.end()) continue;
                if (!it->second.dir && !b.dir) b.size = it->second.size;  // the mod's file replaces the game's
                mod.erase(it);
            }
            for (auto& kv : mod) out.push_back(kv.second);
        }
    }
    if (error) *error = (out.empty() && !modHas) ? baseErr : std::string();
    return out;
}

}  // namespace uzume::formats
