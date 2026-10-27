// RomFS overlay: the files of a LayeredFS mod win over the game's RomFS, listings merge. Synthetic RomFS only.
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "format_helpers.h"
#include "synth_formats.h"

using namespace uzume::formats;
namespace syn = uzume::synth;

class RomfsOverlayTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QString rom_, mod_;

    static void put(const QString& path, const QByteArray& data) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(data);
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        std::map<std::string, std::string> files;
        files["/a/base.bin"] = "base";
        files["/a/changed.bin"] = "original";
        files["/b/only-base.txt"] = "x";
        const std::string cxi = syn::makeNcchWithRomfs(files);
        rom_ = dir_.filePath(QStringLiteral("g.cxi"));
        put(rom_, QByteArray(cxi.data(), static_cast<qsizetype>(cxi.size())));
        mod_ = dir_.filePath(QStringLiteral("mod"));
        put(mod_ + QStringLiteral("/romfs/a/changed.bin"), "randomized!");
        put(mod_ + QStringLiteral("/romfs/a/new.bin"), "new");
        put(mod_ + QStringLiteral("/romfs/c/deep.bin"), "deep");
        put(mod_ + QStringLiteral("/code.bin"), "not part of the romfs");
    }

    void modFilesWinAndTheRestComesFromTheGame() {
        QCOMPARE(romfsOverlayRead(rom_, mod_, "/a/changed.bin").data, std::string("randomized!"));
        QCOMPARE(romfsOverlayRead(rom_, mod_, "a//new.bin").data, std::string("new"));
        QCOMPARE(romfsOverlayRead(rom_, mod_, "/a/base.bin").data, std::string("base"));
        QCOMPARE(romfsOverlayRead(rom_, QString(), "/a/changed.bin").data, std::string("original"));  // no variant: the plain game
        QVERIFY(!romfsOverlayRead(rom_, mod_, "/missing").ok);
        QVERIFY(!romfsOverlayRead(rom_, mod_, "/../code.bin").ok);  // cannot leave <mod>/romfs
        QVERIFY(!romfsOverlayRead(rom_, mod_, "/a/../../code.bin").ok);
    }

    void listingsMerge() {
        std::string err;
        auto names = [](const std::vector<RomfsEntry>& v) {
            std::vector<std::string> n;
            for (const auto& e : v) n.push_back((e.dir ? "d:" : "f:") + e.name + ":" + std::to_string(e.size));
            std::sort(n.begin(), n.end());
            return n;
        };
        const auto a = romfsOverlayList(rom_, mod_, "/a", &err);
        QCOMPARE(names(a), (std::vector<std::string>{"f:base.bin:4", "f:changed.bin:11", "f:new.bin:3"}));
        const auto root = romfsOverlayList(rom_, mod_, "/", &err);
        QCOMPARE(names(root), (std::vector<std::string>{"d:a:0", "d:b:0", "d:c:0"}));
        QCOMPARE(names(romfsOverlayList(rom_, mod_, "/c", &err)), (std::vector<std::string>{"f:deep.bin:4"}));
        QCOMPARE(names(romfsOverlayList(rom_, QString(), "/a", &err)).size(), size_t(2));
    }
};

QTEST_APPLESS_MAIN(RomfsOverlayTest)
#include "romfs_overlay_test.moc"
