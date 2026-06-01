/*
 * =====================================================================
 * File: QtLogStress_main.cpp
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   Throughput stress test for high-concurrency log writing.
 *   高并发日志写入吞吐压力测试。
 * =====================================================================
 */

#include "QtLog.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QThread>
#include <thread>
#include <vector>

namespace {
// Counts non-empty lines in one log file.
// 统计单个日志文件中的非空行数。
int countLines(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0;
    }

    int lines = 0;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        if (!line.isEmpty()) {
            ++lines;
        }
    }
    return lines;
}
} // namespace

// Stress-test entry for high-concurrency write throughput.
// 高并发写入吞吐压力测试入口。
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription("QtLog stress test");
    parser.addHelpOption();
    parser.addOption(QCommandLineOption("threads", "Thread count", "threads", "8"));
    parser.addOption(QCommandLineOption("messages", "Messages per thread", "messages", "20000"));
    parser.addOption(QCommandLineOption("files", "Log file count", "files", "4"));
    parser.addOption(QCommandLineOption("dir",
                                        "Output directory",
                                        "dir",
                                        QDir::temp().absoluteFilePath("QtLogStress")));
    parser.process(app);

    const int threadCount = qMax(1, parser.value("threads").toInt());
    const int messagesPerThread = qMax(1, parser.value("messages").toInt());
    const int fileCount = qMax(1, parser.value("files").toInt());
    const QString outputDir = parser.value("dir");
    QDir().mkpath(outputDir);

    QtLog::setMinimumLogLevel(QwtLogger::Info);
    QtLog::setMaxFileSizeBytes(8 * 1024 * 1024);
    QtLog::setDailyRotationEnabled(false);
    QtLog::setRetentionDays(3);
    QtLog::setCompressionEnabled(false);

    QElapsedTimer timer;
    timer.start();

    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (int t = 0; t < threadCount; ++t) {
        workers.emplace_back([t, messagesPerThread, fileCount, outputDir]() {
            for (int i = 0; i < messagesPerThread; ++i) {
                const int fileIndex = (t + i) % fileCount;
                const QString filePath =
                    QDir(outputDir).absoluteFilePath(QString("stress_%1.log").arg(fileIndex));
                QtLog* logger = QtLog::Instance(filePath);
                logger->writeLog(
                    QString("tid=%1 idx=%2 ts=%3")
                        .arg(t)
                        .arg(i)
                        .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)),
                    QwtLogger::Info,
                    QwtLogger::FileOnly);
            }
        });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    QtLog::flush();
    const qint64 elapsedMs = timer.elapsed();

    int totalLines = 0;
    for (int i = 0; i < fileCount; ++i) {
        const QString filePath = QDir(outputDir).absoluteFilePath(QString("stress_%1.log").arg(i));
        totalLines += countLines(filePath);
    }

    const qint64 totalMessages = static_cast<qint64>(threadCount) * messagesPerThread;
    const double throughput = elapsedMs > 0 ? (1000.0 * totalMessages / elapsedMs) : totalMessages;

    qInfo().noquote() << QString("threads=%1 messages_per_thread=%2 files=%3 total_messages=%4")
                             .arg(threadCount)
                             .arg(messagesPerThread)
                             .arg(fileCount)
                             .arg(totalMessages);
    qInfo().noquote() << QString("elapsed_ms=%1 throughput_msg_per_sec=%2")
                             .arg(elapsedMs)
                             .arg(QString::number(throughput, 'f', 2));
    qInfo().noquote() << QString("written_lines=%1 output_dir=%2").arg(totalLines).arg(outputDir);

    return totalLines == totalMessages ? 0 : 2;
}
