#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "version.h"

class CoreVersionTest : public QObject {
  Q_OBJECT
 private slots:
  void versionIsSet() { QVERIFY(!uzume::playerVersion().empty()); }

  void channelIsKnown() {
    const std::string_view c = uzume::playerChannel();
    QVERIFY(c == "stable" || c == "beta" || c == "dev");
  }

  void displayVersionDropsTheReservedThirdField() {
    QCOMPARE(QString::fromStdString(uzume::displayVersion("21.1.0")), QString("21.1"));
    QCOMPARE(QString::fromStdString(uzume::displayVersion("21.37.0")), QString("21.37"));
    QCOMPARE(QString::fromStdString(uzume::displayVersion("21.0.0-dev.5")), QString("21.0-dev.5"));
    QCOMPARE(QString::fromStdString(uzume::displayVersion("0.20.15")), QString("0.20.15"));
    QCOMPARE(QString::fromStdString(uzume::displayVersion("21.1.10")), QString("21.1.10"));
    QCOMPARE(QString::fromStdString(uzume::displayVersion("21.1")), QString("21.1"));
    QCOMPARE(QString::fromStdString(uzume::displayVersion("dev")), QString("dev"));
    QCOMPARE(QString::fromStdString(uzume::displayVersion("")), QString(""));
  }

  void versionJsonHasContractKeys() {
    const std::string s = uzume::playerVersionJson();
    QVERIFY(s.find('\n') == std::string::npos);
    const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(s)).object();
    QCOMPARE(o.value("product").toString(), QString("player"));
    QCOMPARE(o.value("version").toString().toStdString(), std::string(uzume::playerVersion()));
    QCOMPARE(o.value("channel").toString().toStdString(), std::string(uzume::playerChannel()));
    QVERIFY(o.contains("commit"));
    QCOMPARE(o.value("protocol_version").toInt(), 1);
    QCOMPARE(o.value("min_protocol_version").toInt(), 1);
    QVERIFY(o.value("msi_downgrade").toBool());  // this build installs over a newer version (rollback target)
  }
};

QTEST_GUILESS_MAIN(CoreVersionTest)
#include "core_version_test.moc"
