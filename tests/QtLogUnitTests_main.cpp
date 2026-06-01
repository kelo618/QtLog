/*
 * =====================================================================
 * File: QtLogUnitTests_main.cpp
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   Unit tests for queue logging, rotation, and shutdown behavior.
 *   针对队列写入、滚动与退出行为的单元测试。
 * =====================================================================
 */

#include "QtLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <thread>
#include <vector>

namespace {
// Reads full UTF-8 text content from file path.
// 读取指定文件的完整 UTF-8 文本内容。
QString readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

// Counts non-empty lines in a text file.
// 统计文本文件中的非空行数量。
int lineCount(const QString& path)
{
    const QString content = readFile(path);
    if (content.isEmpty()) {
        return 0;
    }
    const QStringList lines = content.split('\n', Qt::SkipEmptyParts);
    return lines.size();
}

// Child-process entry used to verify post-routine flush on exit.
// 子进程入口，用于验证进程退出时的后置刷盘行为。
int runExitFlushChild(const QString& logPath)
{
    QtLog::setMinimumLogLevel(QwtLogger::Info);
    QtLog::setMaxFileSizeBytes(0);
    QtLog::setDailyRotationEnabled(false);
    QtLog::setRetentionDays(0);
    QtLog::setCompressionEnabled(false);

    QtLog* logger = QtLog::Instance(logPath);
    for (int i = 0; i < 200; ++i) {
        logger->writeLog(QString("exit_flush_%1").arg(i), QwtLogger::Info, QwtLogger::FileOnly);
    }
    return 0;
}
} // namespace

/**
 * @brief Unit test suite for QtLog behavior. / QtLog 行为单元测试集合。
 */
class QtLogUnitTests : public QObject
{
    Q_OBJECT

private slots:
    /**
     * @brief Resets runtime options before each case. / 每个用例前重置运行时配置。
     */
    void init();

    /**
     * @brief Flushes pending logs after each case. / 每个用例后刷出残留日志。
     */
    void cleanup();

    /**
     * @brief Verifies concurrent writes preserve total count. / 验证并发写入总量正确。
     */
    void concurrentWrite();

    /**
     * @brief Verifies independent writing across multiple files. / 验证多文件独立写入。
     */
    void multiFileWrite();

    /**
     * @brief Verifies open-file failure path is handled safely. / 验证打开文件失败分支安全处理。
     */
    void openFileFailure();

    /**
     * @brief Verifies size rotation, compression, and retention cleanup.
     *        验证按大小滚动、压缩与保留清理策略。
     */
    void rollingBySizeCompressionRetention();

    /**
     * @brief Verifies process-exit flush through child process.
     *        通过子进程验证进程退出时刷盘行为。
     */
    void exitFlushOnProcessExit();

private:
    /**
     * @brief Builds absolute path under temporary workspace. / 在临时目录下构建绝对路径。
     * @param name File name relative to temporary directory. / 相对临时目录的文件名。
     * @return Absolute path in temporary workspace. / 临时工作目录下的绝对路径。
     */
    QString makePath(const QString& name) const;

private:
    QTemporaryDir tempDir; ///< Per-test temporary workspace. / 每次测试使用的临时工作目录。
};

// Initializes test fixture and resets global logger options.
// 初始化测试夹具并重置全局日志配置。
void QtLogUnitTests::init()
{
    QVERIFY2(tempDir.isValid(), "Temporary test directory must be valid.");

    QtLog::setMinimumLogLevel(QwtLogger::Info);
    QtLog::setMaxFileSizeBytes(0);
    QtLog::setDailyRotationEnabled(false);
    QtLog::setRetentionDays(0);
    QtLog::setCompressionEnabled(false);
}

// Flushes pending records after each test case.
// 每个测试用例后刷出剩余记录。
void QtLogUnitTests::cleanup()
{
    QtLog::flush();
}

// Creates absolute file path in temporary directory.
// 在临时目录内创建绝对文件路径。
QString QtLogUnitTests::makePath(const QString& name) const
{
    return QDir(tempDir.path()).absoluteFilePath(name);
}

// Stresses concurrent producers and validates final line count.
// 压测并发生产者并校验最终日志行数。
void QtLogUnitTests::concurrentWrite()
{
    const QString path = makePath("concurrent.log");
    QtLog* logger = QtLog::Instance(path);

    constexpr int threadCount = 8;
    constexpr int logsPerThread = 300;

    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (int t = 0; t < threadCount; ++t) {
        workers.emplace_back([logger, t]() {
            for (int i = 0; i < logsPerThread; ++i) {
                logger->writeLog(QString("thread_%1_%2").arg(t).arg(i),
                                 QwtLogger::Info,
                                 QwtLogger::FileOnly);
            }
        });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    QtLog::flush();
    QCOMPARE(lineCount(path), threadCount * logsPerThread);
}

// Writes to two files and validates no cross-file interference.
// 分别写入两个文件并校验互不干扰。
void QtLogUnitTests::multiFileWrite()
{
    const QString aPath = makePath("multi_a.log");
    const QString bPath = makePath("multi_b.log");
    QtLog* loggerA = QtLog::Instance(aPath);
    QtLog* loggerB = QtLog::Instance(bPath);

    for (int i = 0; i < 50; ++i) {
        loggerA->writeLog(QString("A_%1").arg(i), QwtLogger::Info, QwtLogger::FileOnly);
        loggerB->writeLog(QString("B_%1").arg(i), QwtLogger::Warning, QwtLogger::FileOnly);
    }

    QtLog::flush();
    QCOMPARE(lineCount(aPath), 50);
    QCOMPARE(lineCount(bPath), 50);
}

// Uses a directory path as file target to trigger open failure branch.
// 使用目录路径作为文件目标以触发打开失败分支。
void QtLogUnitTests::openFileFailure()
{
    const QString dirPath = makePath("not_a_file");
    QDir().mkpath(dirPath);

    QtLog* logger = QtLog::Instance(dirPath);
    logger->writeLog("should_fail_to_open_file", QwtLogger::Error, QwtLogger::FileOnly);
    QtLog::flush();

    QVERIFY(QFileInfo(dirPath).isDir());
}

// Verifies rolling archive generation and retention cleanup behavior.
// 验证滚动归档生成及保留清理行为。
void QtLogUnitTests::rollingBySizeCompressionRetention()
{
    const QString logPath = makePath("rolling.log");
    QtLog::setMaxFileSizeBytes(256);
    QtLog::setCompressionEnabled(true);
    QtLog::setRetentionDays(1);

    const QString staleArchive = makePath("rolling_20000101_000000_000.log.qz");
    {
        QFile stale(staleArchive);
        QVERIFY(stale.open(QIODevice::WriteOnly | QIODevice::Truncate));
        stale.write("stale");
        stale.close();
        stale.setFileTime(QDateTime::currentDateTime().addDays(-10),
                          QFileDevice::FileModificationTime);
    }

    QtLog* logger = QtLog::Instance(logPath);
    const QString payload(80, 'X');
    for (int i = 0; i < 200; ++i) {
        logger->writeLog(QString("roll_%1_%2").arg(i).arg(payload),
                         QwtLogger::Info,
                         QwtLogger::FileOnly);
    }
    QtLog::flush();

    const QDir dir(tempDir.path());
    const QStringList archives = dir.entryList(QStringList() << "rolling_*.log.qz", QDir::Files);
    QVERIFY2(!archives.isEmpty(), "Expected compressed rolling archives.");
    QVERIFY2(!QFile::exists(staleArchive), "Expected stale archive to be deleted by retention.");
}

// Runs child process and verifies logs are flushed on normal process exit.
// 拉起子进程并验证正常退出时日志已刷盘。
void QtLogUnitTests::exitFlushOnProcessExit()
{
    const QString path = makePath("exit_flush.log");
    QProcess process;
    process.start(QCoreApplication::applicationFilePath(), {"--exit-flush-child", path});
    QVERIFY2(process.waitForStarted(5000), "Child process failed to start.");
    QVERIFY2(process.waitForFinished(10000), "Child process did not finish in time.");
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);

    QVERIFY2(lineCount(path) > 0, "Expected logs to be flushed on process exit.");
}

// Test entry point: dispatches child mode or executes QTest suite.
// 测试入口：分发子进程模式或执行 QTest 用例集。
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 3 && args.at(1) == "--exit-flush-child") {
        return runExitFlushChild(args.at(2));
    }

    QtLogUnitTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "QtLogUnitTests_main.moc"
