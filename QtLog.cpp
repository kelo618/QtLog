/*
 * =====================================================================
 * File: QtLog.cpp
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   QtLog public facade implementation.
 *   QtLog 对外门面实现。
 * =====================================================================
 */

#include "QtLog.h"

#include <QMetaObject>
#include "LogWorker.h"

QMutex QtLog::instanceMutex;
QHash<QString, QSharedPointer<QtLog>> QtLog::instances;

std::once_flag QtLog::workerOnce;
QPointer<QThread> QtLog::workerThread;
QPointer<LogWorker> QtLog::worker;
std::atomic<int> QtLog::minLogLevel{static_cast<int>(QwtLogger::Info)};
std::atomic<qint64> QtLog::maxFileSizeBytesValue{0};
std::atomic<int> QtLog::retentionDaysValue{0};
std::atomic<bool> QtLog::dailyRotationEnabledValue{false};
std::atomic<bool> QtLog::compressArchivesValue{false};

// Initializes shared worker objects exactly once during process lifetime.
// 在进程生命周期内只初始化一次共享工作对象。
void QtLog::initWorkerOnce()
{
    std::call_once(workerOnce, []() {
        QCoreApplication *app = QCoreApplication::instance();
        Q_ASSERT_X(app, "QtLog", "QtLog must be used after QCoreApplication");

        workerThread = new QThread();
        workerThread->setObjectName("QtLogWorkerThread");

        worker = new LogWorker();
        worker->moveToThread(workerThread);

        QObject::connect(workerThread, &QThread::started, worker, &LogWorker::start);
        QObject::connect(workerThread, &QThread::finished, worker, &QObject::deleteLater);

        workerThread->start();
        qAddPostRoutine(&QtLog::shutdownWorker);
    });
}

// Flushes pending logs and stops worker thread during process teardown.
// 在进程退出阶段刷空日志并停止工作线程。
void QtLog::shutdownWorker()
{
    if (!workerThread) {
        return;
    }

    if (worker) {
        QMetaObject::invokeMethod(worker, &LogWorker::flush, Qt::BlockingQueuedConnection);
    }

    workerThread->quit();
    workerThread->wait();

    delete workerThread;
    workerThread = nullptr;
    worker = nullptr;
}

// Returns a singleton logger bound to the specified file path.
// 返回绑定到指定文件路径的单例日志对象。
QtLog *QtLog::Instance(const QString &targetFilePath)
{
    initWorkerOnce();

    QMutexLocker locker(&instanceMutex);
    const auto it = instances.find(targetFilePath);
    if (it != instances.end()) {
        return it.value().data();
    }

    QSharedPointer<QtLog> logger(new QtLog(targetFilePath));
    instances.insert(targetFilePath, logger);
    return logger.data();
}

// Updates global minimum log level threshold.
// 更新全局最小日志等级阈值。
void QtLog::setMinimumLogLevel(QwtLogger::LogLevel level)
{
    minLogLevel.store(static_cast<int>(level), std::memory_order_relaxed);
}

// Reads global minimum log level threshold.
// 读取全局最小日志等级阈值。
QwtLogger::LogLevel QtLog::minimumLogLevel()
{
    return static_cast<QwtLogger::LogLevel>(minLogLevel.load(std::memory_order_relaxed));
}

// Updates maximum log file size used for rotation.
// 更新按大小滚动使用的最大日志文件大小。
void QtLog::setMaxFileSizeBytes(qint64 bytes)
{
    maxFileSizeBytesValue.store(bytes < 0 ? 0 : bytes, std::memory_order_relaxed);
}

// Reads maximum log file size used for rotation.
// 读取按大小滚动使用的最大日志文件大小。
qint64 QtLog::maxFileSizeBytes()
{
    return maxFileSizeBytesValue.load(std::memory_order_relaxed);
}

// Enables or disables daily rotation.
// 启用或关闭按天滚动。
void QtLog::setDailyRotationEnabled(bool enabled)
{
    dailyRotationEnabledValue.store(enabled, std::memory_order_relaxed);
}

// Returns whether daily rotation is enabled.
// 返回是否启用按天滚动。
bool QtLog::dailyRotationEnabled()
{
    return dailyRotationEnabledValue.load(std::memory_order_relaxed);
}

// Updates archive retention days.
// 更新归档保留天数。
void QtLog::setRetentionDays(int days)
{
    retentionDaysValue.store(days < 0 ? 0 : days, std::memory_order_relaxed);
}

// Reads archive retention days.
// 读取归档保留天数。
int QtLog::retentionDays()
{
    return retentionDaysValue.load(std::memory_order_relaxed);
}

// Enables or disables archive compression.
// 启用或关闭归档压缩。
void QtLog::setCompressionEnabled(bool enabled)
{
    compressArchivesValue.store(enabled, std::memory_order_relaxed);
}

// Returns whether archive compression is enabled.
// 返回是否启用归档压缩。
bool QtLog::compressionEnabled()
{
    return compressArchivesValue.load(std::memory_order_relaxed);
}

// Forces queued logs to flush through worker thread.
// 通过工作线程强制刷出队列中的日志。
void QtLog::flush()
{
    LogWorker *workerSnapshot = worker;
    if (!workerSnapshot) {
        return;
    }
    QMetaObject::invokeMethod(workerSnapshot, &LogWorker::flush, Qt::BlockingQueuedConnection);
}

// Captures current writer options as an immutable snapshot for one enqueue.
// 捕获当前写入配置快照供单次入队使用。
QwtLogger::WriterOptions QtLog::writerOptionsSnapshot()
{
    QwtLogger::WriterOptions options;
    options.maxFileSizeBytes = maxFileSizeBytesValue.load(std::memory_order_relaxed);
    options.retentionDays = retentionDaysValue.load(std::memory_order_relaxed);
    options.dailyRotationEnabled = dailyRotationEnabledValue.load(std::memory_order_relaxed);
    options.compressArchives = compressArchivesValue.load(std::memory_order_relaxed);
    return options;
}

// Creates a logger bound to one file path.
// 创建绑定到单一文件路径的日志实例。
QtLog::QtLog(const QString &targetFilePath)
    : QObject(nullptr)
    , logFilePath(targetFilePath)
{}

// Filters by minimum level, formats message, and enqueues asynchronously.
// 先按最小等级过滤，再格式化并异步入队。
void QtLog::writeLog(const QString &message,
                     QwtLogger::LogLevel level,
                     QwtLogger::OutputOption option)
{
    if (static_cast<int>(level) < minLogLevel.load(std::memory_order_relaxed)) {
        return;
    }

    LogWorker *workerSnapshot = worker;
    if (!workerSnapshot) {
        return;
    }

    const QString logMessage = QString("%1 %2 : %3")
                                   .arg(logLevelToString(level), getCurrentTimestamp(), message);
    workerSnapshot->enqueueLog(logFilePath, logMessage, level, option, writerOptionsSnapshot());
}

// Converts enum level to string prefix used in final log text.
// 将枚举日志等级转换为最终日志文本前缀。
QString QtLog::logLevelToString(QwtLogger::LogLevel level) const
{
    switch (level) {
    case QwtLogger::Info:
        return "[INFO]";
    case QwtLogger::Warning:
        return "[WARNING]";
    case QwtLogger::Error:
        return "[ERROR]";
    default:
        return "[UNKNOWN]";
    }
}

// Generates local timestamp text in fixed format.
// 生成固定格式的本地时间戳文本。
QString QtLog::getCurrentTimestamp() const
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}
