// Shared read-only memory access (debug MCP memory_read / memory_search and the developer console's memory viewer).
#include <QtTest>

#include <vector>

#include "memory_access.h"

using namespace uzume::emu;
namespace ma = uzume::emu::memaccess;

class MemoryAccessTest : public QObject {
  Q_OBJECT
 private slots:
  void readStopsAtUnmappedMemory() {
    std::vector<uint8_t> ram(0x100);
    for (size_t i = 0; i < ram.size(); ++i) ram[i] = static_cast<uint8_t>(i);
    const MemoryMap m = MemoryMap::fromBuffer(ram.data(), ram.size(), 0x1000);
    QCOMPARE(ma::read(m, 0x1010, 4), QByteArray("\x10\x11\x12\x13", 4));
    QCOMPARE(ma::read(m, 0x10FE, 16).size(), 2);  // two bytes, then nothing is mapped
    QVERIFY(ma::read(m, 0x2000, 16).isEmpty());
    QVERIFY(ma::read(m, 0x0, 16).isEmpty());
  }

  void searchFindsMatchesAndHonoursMax() {
    std::vector<uint8_t> ram(0x2000, 0);
    ram[0x10] = 0xAA;
    ram[0x20] = 0xAA;
    ram[0x1800] = 0xAA;
    const MemoryMap m = MemoryMap::fromBuffer(ram.data(), ram.size(), 0x8000);
    const ma::SearchResult all = ma::search(m, 0x8000, 0xA000, QByteArray("\xAA", 1), 100);
    QCOMPARE(all.matches, (QList<quint64>{0x8010, 0x8020, 0x9800}));
    QCOMPARE(all.scanned, quint64(0x2000));
    QVERIFY(!all.truncated);
    const ma::SearchResult two = ma::search(m, 0x8000, 0xA000, QByteArray("\xAA", 1), 2);
    QCOMPARE(two.matches.size(), 2);
    QVERIFY(two.truncated);
    QVERIFY(ma::search(m, 0x8000, 0x8000, QByteArray("\xAA", 1), 2).matches.isEmpty());
    QVERIFY(ma::search(m, 0x8000, 0xA000, QByteArray(), 2).matches.isEmpty());
  }

  void parsing() {
    QCOMPARE(ma::parseHex("DE AD be ef"), QByteArray("\xDE\xAD\xBE\xEF", 4));
    QCOMPARE(ma::parseHex("0xdeadbeef"), QByteArray("\xDE\xAD\xBE\xEF", 4));
    QCOMPARE(ma::parseHex("de:ad"), QByteArray("\xDE\xAD", 2));
    QVERIFY(ma::parseHex("abc").isEmpty());
    QVERIFY(ma::parseHex("zz").isEmpty());
    QVERIFY(ma::parseHex("").isEmpty());
    QCOMPARE(ma::valueBytes(0x1234, "u16"), QByteArray("\x34\x12", 2));
    QCOMPARE(ma::valueBytes(0x2A, "u8"), QByteArray("\x2A", 1));
    QCOMPARE(ma::valueBytes(1, "u32").size(), 4);
    QVERIFY(ma::valueBytes(1, "f32").isEmpty());
    quint64 v = 0;
    QVERIFY(ma::parseAddress(" 0x08001A10 ", &v));
    QCOMPARE(v, quint64(0x08001A10));
    QVERIFY(ma::parseAddress("1234", &v));
    QCOMPARE(v, quint64(1234));
    QVERIFY(!ma::parseAddress("12zz", &v));
    QVERIFY(!ma::parseAddress("", &v));
    QVERIFY(!ma::parseAddress("-1", &v));
  }

  void hexRows() {
    const QByteArray data("AB\x00\x7F\x80xyz0123456789", 18);
    const auto rows = ma::hexRows(data, 0x08000000, 16);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].addrText, QStringLiteral("0x08000000"));
    QCOMPARE(rows[0].hex, QStringLiteral("41 42 00 7F 80 78 79 7A 30 31 32 33 34 35 36 37"));
    QCOMPARE(rows[0].ascii, QStringLiteral("AB...xyz01234567"));
    QCOMPARE(rows[1].addrText, QStringLiteral("0x08000010"));
    QCOMPARE(rows[1].hex, QStringLiteral("38 39"));
    QCOMPARE(ma::hexAddress(0x1A), QStringLiteral("0x0000001A"));
  }
};

QTEST_GUILESS_MAIN(MemoryAccessTest)
#include "memory_access_test.moc"
