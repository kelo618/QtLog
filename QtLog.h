/*
 * =====================================================================
 * File: QtLog.h
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   Public asynchronous logger facade.
 *   对外异步日志门面接口。
 * =====================================================================
 */

#ifndef QTLOG_H
#define QTLOG_H

#include "QtLog_global.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QSharedPointer>
#include <QThread>
#include <atomic>
#include <mutex>

class LogWorker;

/**
 * @brief Public asynchronous logger facade. / 对外异步日志门面。
 *
 * Manages logger instances keyed by file path and forwards records to a
 * dedicated worker thread. / 维护按文件路径区分的日志实例，并将日志转发到独立工作线程。
 */
class QTLOG_EXPORT QtLog : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Returns or creates logger instance for a file path. / 返回或创建指定文件路径的日志实例。
     * @param targetFilePath Target log file path. / 目标日志文件路径。
     * @return Shared singleton pointer for that path. / 该路径对应的共享单例指针。
     */
    static QtLog* Instance(const QString& targetFilePath);

    /**
     * @brief Sets global minimum log level filter. / 设置全局最小日志等级过滤阈值。
     * @param level Minimum accepted level. / 可写入的最小日志等级。
     */
    static void setMinimumLogLevel(QwtLogger::LogLevel level);

    /**
     * @brief Gets global minimum log level filter. / 获取全局最小日志等级过滤阈值。
     * @return Current minimum accepted level. / 当前可写入的最小日志等级。
     */
    static QwtLogger::LogLevel minimumLogLevel();

    /**
     * @brief Sets size-based rotation threshold in bytes. / 设置按大小滚动阈值（字节）。
     * @param bytes Max file size before rotation, <=0 disables size rotation. /
     *              触发滚动的最大文件大小，<=0 表示关闭按大小滚动。
     */
    static void setMaxFileSizeBytes(qint64 bytes);

    /**
     * @brief Gets size-based rotation threshold. / 获取按大小滚动阈值。
     * @return Max file size in bytes. / 最大文件大小（字节）。
     */
    static qint64 maxFileSizeBytes();

    /**
     * @brief Enables or disables daily rotation. / 启用或关闭按天滚动。
     * @param enabled True to rotate when date changes. / true 表示跨天时滚动。
     */
    static void setDailyRotationEnabled(bool enabled);

    /**
     * @brief Checks daily rotation switch. / 查询按天滚动开关状态。
     * @return True when daily rotation is enabled. / 返回是否启用按天滚动。
     */
    static bool dailyRotationEnabled();

    /**
     * @brief Sets retention days for rolled archives. / 设置滚动归档保留天数。
     * @param days Number of days to keep, <=0 disables retention cleanup. /
     *             保留天数，<=0 表示关闭过期清理。
     */
    static void setRetentionDays(int days);

    /**
     * @brief Gets retention days for rolled archives. / 获取滚动归档保留天数。
     * @return Retention days setting. / 当前保留天数配置。
     */
    static int retentionDays();

    /**
     * @brief Enables or disables archive compression. / 启用或关闭归档压缩。
     * @param enabled True to compress rotated files. / true 表示压缩滚动后的文件。
     */
    static void setCompressionEnabled(bool enabled);

    /**
     * @brief Checks archive compression switch. / 查询归档压缩开关状态。
     * @return True when compression is enabled. / 返回是否启用压缩。
     */
    static bool compressionEnabled();

    /**
     * @brief Flushes queued logs synchronously. / 同步刷出当前队列中的日志。
     */
    static void flush();

    /**
     * @brief Default destructor. / 默认析构函数。
     */
    ~QtLog() override = default;

    /**
     * @brief Writes one log record asynchronously. / 异步写入一条日志。
     * @param message Business log message body. / 业务日志正文。
     * @param level Log level used for filtering and formatting. / 用于过滤与格式化的日志等级。
     * @param option Output target option. / 输出目标选项。
     */
    void writeLog(const QString& message,
                  QwtLogger::LogLevel level = QwtLogger::Info,
                  QwtLogger::OutputOption option = QwtLogger::FileOnly);

private:
    /**
     * @brief Constructs logger bound to one path. / 构造绑定到单一路径的日志对象。
     * @param targetFilePath Log file path bound to this instance. / 当前实例绑定的日志文件路径。
     */
    explicit QtLog(const QString& targetFilePath);

    /**
     * @brief Initializes shared worker thread once. / 初始化全局共享工作线程（仅一次）。
     */
    static void initWorkerOnce();

    /**
     * @brief Shuts down shared worker thread safely. / 安全关闭全局共享工作线程。
     */
    static void shutdownWorker();

    /**
     * @brief Captures current writer options atomically. / 原子快照当前写入配置。
     * @return Writer options snapshot for one log enqueue. / 单次入队使用的写入选项快照。
     */
    static QwtLogger::WriterOptions writerOptionsSnapshot();

    /**
     * @brief Converts log level to text tag. / 将日志等级转换为文本标签。
     * @param level Input log level. / 输入日志等级。
     * @return Level prefix string. / 日志等级前缀字符串。
     */
    QString logLevelToString(QwtLogger::LogLevel level) const;

    /**
     * @brief Returns current local timestamp text. / 返回当前本地时间戳文本。
     * @return Formatted timestamp. / 格式化后的时间戳。
     */
    QString getCurrentTimestamp() const;

private:
    QString logFilePath; ///< Bound file path for this logger instance. / 当前实例绑定的日志文件路径。

    static QMutex instanceMutex; ///< Guards instance map access. / 保护实例映射访问。
    static QHash<QString, QSharedPointer<QtLog>> instances; ///< file path -> logger instance. / 文件路径到日志实例映射。

    static std::once_flag workerOnce; ///< Initializes worker infrastructure once. / 确保工作线程基础设施仅初始化一次。
    static QPointer<QThread> workerThread; ///< Background worker thread object. / 后台日志工作线程对象。
    static QPointer<LogWorker> worker; ///< Background worker object. / 后台日志工作对象。

    static std::atomic<int> minLogLevel; ///< Runtime minimum log level threshold. / 运行时最小日志等级阈值。
    static std::atomic<qint64> maxFileSizeBytesValue; ///< Max file size for rotation. / 触发大小滚动的最大文件大小。
    static std::atomic<int> retentionDaysValue; ///< Retention days for archives. / 归档保留天数。
    static std::atomic<bool> dailyRotationEnabledValue; ///< Daily rotation switch. / 按天滚动开关。
    static std::atomic<bool> compressArchivesValue; ///< Archive compression switch. / 归档压缩开关。
};

#endif // QTLOG_H
