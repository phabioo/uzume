// CoreLogFilter (issue 435): debug dropped by default, kept with the opt-in, identical repeats collapsed with a count.
#include <QtTest>

#include <atomic>
#include <thread>
#include <vector>

#include "core_log_filter.h"

using uzume::emu::CoreLogFilter;
using uzume::emu::CoreLogLevel;

class CoreLogFilterTest : public QObject {
  Q_OBJECT

  static bool accept(CoreLogFilter& f, CoreLogLevel l, const QString& text, qint64 t, QString* out = nullptr) {
    QString m = text;
    const bool r = f.accept(l, m, t);
    if (out) *out = m;
    return r;
  }

 private slots:
  void debugDroppedByDefault() {
    CoreLogFilter f;
    QVERIFY(!f.debugEnabled());
    QVERIFY(!f.wantsLevel(CoreLogLevel::Debug));
    QVERIFY(f.wantsLevel(CoreLogLevel::Warn));
    QVERIFY(!accept(f, CoreLogLevel::Debug, "a debug line", 0));
    QVERIFY(accept(f, CoreLogLevel::Info, "an info line", 0));
  }

  void debugKeptWithOptIn() {
    CoreLogFilter f(true);
    QVERIFY(f.wantsLevel(CoreLogLevel::Debug));
    QVERIFY(accept(f, CoreLogLevel::Debug, "a debug line", 0));
  }

  void repeatedWarningCollapsedWithCount() {
    CoreLogFilter f(false, 60000);
    const QString w = "nwm::UDS SetProbeResponseParam unimplemented";
    QVERIFY(accept(f, CoreLogLevel::Warn, w, 0));
    for (int i = 1; i <= 4684; ++i) QVERIFY(!accept(f, CoreLogLevel::Warn, w, i * 10));  // 46.8 s
    QString out;
    QVERIFY(accept(f, CoreLogLevel::Warn, w, 60000, &out));
    QVERIFY2(out.contains("repeated 4684 times"), qPrintable(out));
    QVERIFY(!accept(f, CoreLogLevel::Warn, w, 60001));  // new window started
    QVERIFY(accept(f, CoreLogLevel::Warn, "another text", 60002));
  }

  void errorsAreCollapsedButFirstAlwaysShown() {
    CoreLogFilter f;
    QVERIFY(accept(f, CoreLogLevel::Error, "boom", 0));
    QVERIFY(!accept(f, CoreLogLevel::Error, "boom", 5));
    QString out;
    QVERIFY(accept(f, CoreLogLevel::Error, "boom", 61000, &out));
    QVERIFY(out.contains("repeated 1 times"));
  }

  void tableStaysBounded() {
    CoreLogFilter f(false, 60000, 4);
    // digits are folded into the repeat key, so the distinct messages differ in letters
    const auto name = [](int i) { return QStringLiteral("e") + QChar(u'a' + i / 26) + QChar(u'a' + i % 26); };
    for (int i = 0; i < 100; ++i) QVERIFY(accept(f, CoreLogLevel::Error, name(i), i));
    // the oldest were forgotten: reported again instead of growing without limit
    QVERIFY(accept(f, CoreLogLevel::Error, name(0), 200));
  }

  void linesDifferingOnlyInNumbersAreOneMessage() {
    CoreLogFilter f(false, 60000);
    const auto line = [](int i) {
      return QStringLiteral("[%1.%2] Service.NWM <Warning> nwm::UDS SetProbeResponseParam unimplemented").arg(100 + i).arg(i * 7 % 1000, 6, 10, QLatin1Char('0'));
    };
    QVERIFY(accept(f, CoreLogLevel::Warn, line(0), 0));
    for (int i = 1; i <= 4684; ++i) QVERIFY(!accept(f, CoreLogLevel::Warn, line(i), i * 10));
    QString out;
    QVERIFY(accept(f, CoreLogLevel::Warn, line(5000), 60000, &out));
    QVERIFY2(out.contains("repeated 4684 times"), qPrintable(out));
    QVERIFY(out.startsWith(line(5000)));  // the reported text is the real line, only the key is folded
    QCOMPARE(CoreLogFilter::repeatKey(QStringLiteral("at 0x1F2A, 12 ms")), QStringLiteral("at #x#F#A, # ms"));
  }

  void threadSafe() {
    CoreLogFilter f;
    std::atomic<int> reported{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
      ts.emplace_back([&] {
        for (int i = 0; i < 5000; ++i) {
          QString m = "same message";
          if (f.accept(CoreLogLevel::Warn, m, 1000)) ++reported;
        }
      });
    for (auto& t : ts) t.join();
    QCOMPARE(reported.load(), 1);
  }
};

QTEST_GUILESS_MAIN(CoreLogFilterTest)
#include "core_log_filter_test.moc"
