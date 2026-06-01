/*
 * =====================================================================
 * File: LogWorker.h
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   Internal worker for queue based batch logging.
 *   基于队列批处理写日志的内部工作类。
 * =====================================================================
 */

#ifndef LOGWORKER_H
#define LOGWORKER_H

#include "QtLog_global.h"
#include <QDate>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QTextStream>
#include <QTimer>
#include <QVector>

/**
 * @brief Internal worker that batches logs and flushes by timer/threshold.
 *        内部工作类，按阈值与定时批量刷盘日志。
 */
class LogWorker : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Creates worker on owning thread. / 在所属线程创建日志工作对象。
     * @param parent QObject parent. / QObject 父对象。
     */
    explicit LogWorker(QObject* parent = nullptr);

    /**
     * @brief Destroys worker and performs final flush. / 析构时执行最终刷盘。
     */
    ~LogWorker() override;

    /**
     * @brief Enqueues one formatted log record. / 入队一条已格式化日志记录。
     * @param filePath Target log file path. / 目标日志文件路径。
     * @param message Final log text to persist. / 待持久化的最终日志文本。
     * @param level Log level for console mapping. / 用于控制台映射的日志等级。
     * @param option Output destination option. / 输出目标选项。
     * @param writerOptions Rotation and retention options snapshot. / 滚动与保留配置快照。
     */
    void enqueueLog(const QString& filePath,
                    const QString& message,
                    QwtLogger::LogLevel level,
                    QwtLogger::OutputOption option,
                    const QwtLogger::WriterOptions& writerOptions);

public slots:
    /**
     * @brief Starts periodic flush timer. / 启动周期性刷盘定时器。
     */
    void start();

    /**
     * @brief Flushes queued logs on worker thread. / 在工作线程刷出队列中的日志。
     */
    void flush();

private:
    struct Writer {
        QString baseFilePath; ///< Base log file path. / 主日志文件路径。
        QFile file;           ///< Active output file. / 当前输出文件对象。
        QTextStream stream;   ///< Text stream bound to file. / 绑定文件的文本流。
        QDate openedDate;     ///< Date when file was opened. / 文件打开时的日期。
    };

    struct PendingLog {
        QString filePath; ///< Target file path for this record. / 当前记录目标文件路径。
        QString message; ///< Final formatted message. / 最终格式化日志内容。
        QwtLogger::LogLevel level = QwtLogger::Info; ///< Message log level. / 日志等级。
        QwtLogger::OutputOption option = QwtLogger::FileOnly; ///< Output destination option. / 输出目标选项。
        QwtLogger::WriterOptions writerOptions; ///< Rotation and archive settings. / 滚动和归档设置。
    };

    /**
     * @brief Ensures parent directory of file path exists. / 确保日志文件父目录存在。
     * @param filePath Target log file path. / 目标日志文件路径。
     * @return True if directory exists or created successfully. / 目录可用则返回 true。
     */
    bool ensureDirectoryExists(const QString& filePath) const;

    /**
     * @brief Opens writer file in append text mode. / 以追加文本模式打开写入文件。
     * @param writer Writer descriptor. / 写入器描述对象。
     * @return True when file is ready. / 文件可写返回 true。
     */
    bool openWriterFile(const QSharedPointer<Writer>& writer);

    /**
     * @brief Gets or creates writer for target file. / 获取或创建目标文件写入器。
     * @param filePath Target log file path. / 目标日志文件路径。
     * @return Writer pointer when available, null on open failure. / 可用写入器指针，失败返回空。
     */
    QSharedPointer<Writer> ensureWriter(const QString& filePath);

    /**
     * @brief Checks whether current writer should rotate. / 判断当前写入器是否需要滚动。
     * @param writer Writer descriptor. / 写入器描述对象。
     * @param log Pending log with active options. / 携带当前配置的待写日志。
     * @return True when day/size threshold requires rotation. / 满足按天或按大小条件时返回 true。
     */
    bool shouldRotate(const QSharedPointer<Writer>& writer, const PendingLog& log) const;

    /**
     * @brief Rotates writer file and applies archive policy. / 执行文件滚动并应用归档策略。
     * @param writer Writer descriptor. / 写入器描述对象。
     * @param log Pending log with active options. / 携带当前配置的待写日志。
     * @return True when writer is reopened successfully. / 滚动后可重新写入返回 true。
     */
    bool rotateWriter(const QSharedPointer<Writer>& writer, const PendingLog& log);

    /**
     * @brief Builds rolled file path with timestamp suffix. / 构建带时间戳后缀的滚动文件路径。
     * @param filePath Base log file path. / 原始日志文件路径。
     * @return Absolute rolled file path. / 滚动文件绝对路径。
     */
    QString buildRolledFilePath(const QString& filePath) const;

    /**
     * @brief Compresses rolled file and removes source file. / 压缩滚动文件并删除源文件。
     * @param filePath Rolled file path. / 滚动后的文件路径。
     * @return True when compression and cleanup succeed. / 压缩与清理成功返回 true。
     */
    bool compressAndRemoveFile(const QString& filePath) const;

    /**
     * @brief Deletes expired rolled archives by retention days. / 按保留天数删除过期归档。
     * @param filePath Base log file path. / 原始日志文件路径。
     * @param retentionDays Retention day count. / 保留天数。
     */
    void cleanupOldArchives(const QString& filePath, int retentionDays) const;

    /**
     * @brief Writes one log record to console with level mapping.
     *        按日志等级映射输出到控制台。
     * @param log Pending log record. / 待写日志记录。
     */
    void writeToConsole(const PendingLog& log) const;

    /**
     * @brief Writes one batch to file and/or console. / 将一批日志写入文件和/或控制台。
     * @param batch Pending logs to consume. / 待消费日志批次。
     */
    void writeBatch(const QVector<PendingLog>& batch);

private:
    static constexpr int defaultBatchSize = 128; ///< Max queue batch size before immediate flush. / 达到该批量时立即触发刷盘。
    static constexpr int defaultFlushIntervalMs = 200; ///< Timer interval for periodic flush. / 定时批量刷盘间隔毫秒。

    int batchSize = defaultBatchSize; ///< Runtime batch threshold. / 运行时批量阈值。
    QHash<QString, QSharedPointer<Writer>> writers; ///< Per-file writer cache. / 按文件缓存写入器。

    QMutex queueMutex; ///< Protects pending queue from multi-thread producers. / 保护多线程生产者入队。
    QVector<PendingLog> pendingLogs; ///< In-memory log queue waiting for flush. / 等待刷盘的内存日志队列。
    bool flushScheduled = false; ///< Prevents duplicate queued flush tasks. / 防止重复投递 flush 任务。

    QTimer* flushTimer = nullptr; ///< Periodic flush timer on worker thread. / 工作线程内周期 flush 定时器。
};

#endif // LOGWORKER_H
