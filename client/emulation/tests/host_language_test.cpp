// retroLanguageFor: system locale -> RETRO_LANGUAGE_* (English fallback).
#include <QtTest>

#include "host_language.h"

using uzume::emu::retroLanguageFor;

class HostLanguageTest : public QObject {
  Q_OBJECT
 private slots:
  void mapsKnownLanguages() {
    QCOMPARE(retroLanguageFor(QLocale(QLocale::English, QLocale::UnitedStates)), 0u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Japanese, QLocale::Japan)), 1u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::French, QLocale::France)), 2u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Spanish, QLocale::Spain)), 3u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::German, QLocale::Germany)), 4u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::German, QLocale::Austria)), 4u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Italian, QLocale::Italy)), 5u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Dutch, QLocale::Netherlands)), 6u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Portuguese, QLocale::Brazil)), 7u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Portuguese, QLocale::Portugal)), 8u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Russian, QLocale::Russia)), 9u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Korean, QLocale::SouthKorea)), 10u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Chinese, QLocale::Taiwan)), 11u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Chinese, QLocale::China)), 12u);
  }
  void unsupportedFallsBackToEnglish() {
    QCOMPARE(retroLanguageFor(QLocale(QLocale::Swahili, QLocale::Kenya)), 0u);
    QCOMPARE(retroLanguageFor(QLocale(QLocale::C)), 0u);
  }
};

QTEST_MAIN(HostLanguageTest)
#include "host_language_test.moc"
