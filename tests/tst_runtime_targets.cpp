#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QtTest>

namespace {

QString evidenceDir()
{
#ifdef PHOTOPRINT_EVIDENCE_DIR
    return QStringLiteral(PHOTOPRINT_EVIDENCE_DIR);
#else
    return QDir::currentPath() + QStringLiteral("/build/evidence");
#endif
}

QJsonObject runJson(const QString &program, const QStringList &arguments, qint64 *elapsedMs,
                    bool noCups)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (noCups)
        environment.insert(QStringLiteral("PHOTOPRINT_NO_CUPS"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    QElapsedTimer timer;
    timer.start();
    process.start(program, arguments);
    if (!process.waitForFinished(60000))
        return {};
    *elapsedMs = timer.elapsed();
    const QList<QByteArray> lines = process.readAllStandardOutput().split('\n');
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.startsWith('{'))
            return QJsonDocument::fromJson(line).object();
    }
    return {};
}

} // namespace

class TestRuntimeTargets : public QObject
{
    Q_OBJECT

private slots:
    void measuredTargets();
};

void TestRuntimeTargets::measuredTargets()
{
    if (!qEnvironmentVariableIsSet("PHOTOPRINT_RUNTIME_TARGETS"))
        QSKIP("Set PHOTOPRINT_RUNTIME_TARGETS=1 to run the measured targets.");
#ifdef PHOTOPRINT_BINARY
    const QString binary = QStringLiteral(PHOTOPRINT_BINARY);
#else
    const QString binary = QStringLiteral("photoprint");
#endif
    QVERIFY2(QFile::exists(binary), qPrintable(binary));

    qint64 probeMs = 0;
    const QJsonObject probe = runJson(binary, {QStringLiteral("--startup-probe")}, &probeMs, true);
    QVERIFY2(!probe.isEmpty(), "startup probe produced no JSON");
    const double coldStartMs = probeMs - 350.0; // the probe deliberately waits 350 ms

    qint64 benchmarkMs = 0;
    const QJsonObject benchmark =
        runJson(binary,
                {QStringLiteral("--benchmark"),
                 QStringLiteral("/home/lavx/Downloads/IMG20260830150231.jpg")},
                &benchmarkMs, true);
    QVERIFY2(!benchmark.isEmpty(), "benchmark produced no JSON");

    const double rss = probe.value(QStringLiteral("rssMiB")).toDouble();
    const double previewMs = benchmark.value(QStringLiteral("firstPreviewMs")).toDouble();
    const double exportMs = benchmark.value(QStringLiteral("a4ExportMs")).toDouble();
    const double peak = benchmark.value(QStringLiteral("peakMiB")).toDouble();

    QJsonObject report;
    report.insert(QStringLiteral("cold_start_no_printer"),
                  QJsonObject{{QStringLiteral("value"), coldStartMs},
                              {QStringLiteral("unit"), QStringLiteral("ms")},
                              {QStringLiteral("target"), 2000},
                              {QStringLiteral("passed"), coldStartMs <= 2000.0}});
    report.insert(QStringLiteral("empty_window_rss"),
                  QJsonObject{{QStringLiteral("value"), rss},
                              {QStringLiteral("unit"), QStringLiteral("MiB")},
                              {QStringLiteral("target"), 120},
                              {QStringLiteral("passed"), rss <= 120.0}});
    report.insert(QStringLiteral("first_12mp_preview"),
                  QJsonObject{{QStringLiteral("value"), previewMs},
                              {QStringLiteral("unit"), QStringLiteral("ms")},
                              {QStringLiteral("target"), 1000},
                              {QStringLiteral("passed"), previewMs <= 1000.0}});
    report.insert(QStringLiteral("a4_300ppi_export"),
                  QJsonObject{{QStringLiteral("value"), exportMs},
                              {QStringLiteral("unit"), QStringLiteral("ms")},
                              {QStringLiteral("target"), 3000},
                              {QStringLiteral("passed"), exportMs <= 3000.0}});
    report.insert(QStringLiteral("a4_export_peak_rss"),
                  QJsonObject{{QStringLiteral("value"), peak},
                              {QStringLiteral("unit"), QStringLiteral("MiB")},
                              {QStringLiteral("target"), 768},
                              {QStringLiteral("passed"), peak <= 768.0}});
    report.insert(QStringLiteral("platform"),
                  QJsonObject{{QStringLiteral("value"),
                               QString::fromLatin1(qgetenv("QT_QPA_PLATFORM"))}});

    QDir().mkpath(evidenceDir());
    QFile file(evidenceDir() + QStringLiteral("/runtime-targets.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    file.close();

    QVERIFY2(coldStartMs <= 2000.0, qPrintable(QStringLiteral("cold start %1 ms").arg(coldStartMs)));
    QVERIFY2(rss <= 120.0, qPrintable(QStringLiteral("RSS %1 MiB").arg(rss)));
    QVERIFY2(previewMs <= 1000.0, qPrintable(QStringLiteral("preview %1 ms").arg(previewMs)));
    QVERIFY2(exportMs <= 3000.0, qPrintable(QStringLiteral("export %1 ms").arg(exportMs)));
    QVERIFY2(peak <= 768.0, qPrintable(QStringLiteral("peak %1 MiB").arg(peak)));
    Q_UNUSED(benchmarkMs)
}

QTEST_GUILESS_MAIN(TestRuntimeTargets)
#include "tst_runtime_targets.moc"
