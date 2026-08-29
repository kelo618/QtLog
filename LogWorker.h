/************************************************************
 * File: LogWorker.h
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   声明 QtLog 内部共享工作对象，管理有界队列、按文件复用的 Writer 与批量刷盘定时器。
 *   调用线程只通过受互斥量保护的 enqueueLog() 提交记录，所有文件操作限定在 Worker 线程。
 *   Worker 直接向 QtLog 门面汇报完成数量与错误，内部资源均采用明确的单一所有权。
 ************************************************************/

#ifndef LOGWORKER_H
#define LOGWORKER_H

#include <QtLog/QtLog_global.h>

#include <QDate>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTextStream>
#include <QVector>

class QTimer;

/**
 * @brief 在专用线程中消费所有 Logger 的队列记录并执行实际输出。
 *
 * enqueueLog() 可由任意生产线程调用；除队列外的 Writer、定时器与文件状态只能在对象所属线程访问。
 * 本对象由 QtLog 创建并移动到共享 QThread，调用方不得直接持有或删除。
 */
class LogWorker : public QObject
{
public:
    /**
     * @brief 创建由 QtLog 工作线程独占的内部消费对象。
     * @note 对象无父对象，由工作线程结束信号安排释放。
     */
    LogWorker();

    /**
     * @brief 析构 Worker，并在所属线程中处理仍留在队列中的记录。
     */
    ~LogWorker() override;

    /**
     * @brief 尝试把格式化日志加入进程级有界队列。
     * @param filePath Logger 绑定的规范化绝对路径。
     * @param message 已包含等级与时间戳的最终日志文本。
     * @param level 控制台输出函数映射所需的日志等级。
     * @param output 当前记录的输出目标。
     * @param writerOptions 当前 Logger 的文件策略快照。
     * @return 成功入队返回 true；队列已达容量时丢弃最新记录并返回 false。
     * @note 本函数可从任意线程调用，不执行文件 I/O，也不会等待队列腾出空间。
     */
    bool enqueueLog(const QString& filePath,
                    const QString& message,
                    QtLogging::Level level,
                    QtLogging::Output output,
                    const QtLogging::LoggerOptions& writerOptions);

    /**
     * @brief 在线程启动后开启周期刷盘定时器。
     */
    void start();

    /**
     * @brief 将当前队列交换为本地批次并完成文件和控制台输出。
     * @note 调用方必须已将执行切换到 Worker 所在线程。
     */
    void flush();

private:
    /**
     * @brief 保存单个日志文件在 Worker 线程中的活动句柄与日期状态。
     */
    struct Writer
    {
        QString baseFilePath; ///< 当前 Writer 对应的规范化活动日志路径。
        QFile file; ///< 由 Worker 线程独占打开和关闭的活动文件对象。
        QTextStream stream; ///< 绑定到 file 的 UTF-8 文本流，仅在 Worker 线程访问。
        QDate openedDate; ///< 最近一次成功打开活动文件时的本地日期。
        qint64 currentSizeBytes = 0; ///< 活动文件已落盘与当前流缓冲区的合计 UTF-8 字节数。
    };

    /**
     * @brief 保存一条已入队记录及其不可变输出策略快照。
     */
    struct PendingLog
    {
        QString filePath; ///< 目标 Logger 的规范化绝对路径。
        QString message; ///< 已完成格式化、等待输出的文本。
        QtLogging::Level level = QtLogging::Level::Info; ///< 控制台函数映射使用的严重程度。
        QtLogging::Output output = QtLogging::Output::File; ///< 当前记录需要写入的目标组合。
        QtLogging::LoggerOptions writerOptions; ///< 入队时获取的滚动、保留与压缩策略快照。
    };

    /**
     * @brief 累计同一路径在一个工作批次中的完成与成功数量。
     */
    struct BatchResult
    {
        quint64 completed = 0; ///< 已从队列消费的记录数量。
        quint64 pendingFileWrites = 0; ///< 已写入流、等待批次刷盘确认的记录数量。
        quint64 fileWritten = 0; ///< 批次刷盘成功后确认的文件记录数量。
        quint64 consoleWritten = 0; ///< 已调用控制台输出函数的记录数量。
        quint64 rotations = 0; ///< 批次内成功完成的文件滚动数量。
    };

    /**
     * @brief 确保目标文件的父目录存在。
     * @param filePath 目标日志文件路径。
     * @return 目录已存在或成功创建时返回 true，否则报告错误并返回 false。
     */
    bool ensureDirectoryExists(const QString& filePath) const;

    /**
     * @brief 以追加文本模式打开 Writer 的活动文件并重置流状态。
     * @param writer Worker 独占的 Writer。
     * @return 文件与文本流可用时返回 true，否则报告错误并返回 false。
     */
    bool openWriterFile(Writer& writer);

    /**
     * @brief 获取或创建目标路径的 Worker 线程 Writer。
     * @param filePath 规范化目标路径。
     * @return 文件可写时返回 Writer；打开失败时返回空指针。
     */
    Writer* ensureWriter(const QString& filePath);

    /**
     * @brief 在写入或刷盘错误后关闭 Writer，使后续批次能够重新打开。
     * @param writer 发生错误的 Writer。
     */
    void closeWriterForRetry(Writer& writer) const;

    /**
     * @brief 判断当前记录写入前是否需要执行日期或大小滚动。
     * @param writer 当前活动 Writer。
     * @param log 即将写入的记录与策略快照。
     * @return 任一启用条件满足时返回 true。
     */
    bool shouldRotate(const Writer& writer, const PendingLog& log) const;

    /**
     * @brief 滚动活动文件、应用压缩与保留策略，并重新打开活动文件。
     * @param writer 需要滚动的 Writer。
     * @param log 触发滚动的记录及策略。
     * @param didRotate 输出参数；重命名成功时设置为 true。
     * @param existingWritesFlushed 输出参数；滚动前已有流内容确认刷盘时设置为 true。
     * @return 活动文件重新可写时返回 true，否则返回 false。
     */
    bool rotateWriter(Writer& writer,
                      const PendingLog& log,
                      bool& didRotate,
                      bool& existingWritesFlushed);

    /**
     * @brief 计算一条记录通过文本模式写入后的平台实际字节数。
     * @param message 已格式化但尚未附加最终换行符的日志文本。
     * @return UTF-8 正文字节数、消息内换行转换和最终平台行尾的总和。
     */
    static qint64 serializedLineSize(const QString& message);

    /**
     * @brief 构建不会覆盖现有普通归档或 `.qz` 归档的滚动路径。
     * @param filePath 活动日志路径。
     * @return 带毫秒时间戳和必要递增序号的绝对归档路径。
     */
    QString buildRolledFilePath(const QString& filePath) const;

    /**
     * @brief 使用 qCompress 创建 `.qz` 文件并在成功后删除未压缩源文件。
     * @param filePath 已滚动的普通文件路径。
     * @param loggerFilePath 公开 Logger 绑定的基础路径，用于错误路由。
     * @return 压缩归档完整落盘且源文件删除成功时返回 true，否则保留可恢复文件并返回 false。
     */
    bool compressAndRemoveFile(const QString& filePath,
                               const QString& loggerFilePath) const;

    /**
     * @brief 删除超过保留天数且严格匹配本库命名格式的归档。
     * @param filePath 活动日志基础路径，用于限定目录、名称前缀与扩展名。
     * @param retentionDays 保留自然天数，大于零时生效。
     */
    void cleanupOldArchives(const QString& filePath, int retentionDays) const;

    /**
     * @brief 将单条记录映射到对应的 Qt 控制台输出函数。
     * @param log 待输出记录。
     */
    void writeToConsole(const PendingLog& log) const;

    /**
     * @brief 处理一整个队列快照，并按路径合并刷盘与统计回报。
     * @param batch 本次从共享队列交换出的记录集合。
     */
    void writeBatch(const QVector<PendingLog>& batch);

    /**
     * @brief 通过 qWarning 保留诊断并直接通知 QtLog 门面。
     * @param filePath 错误关联路径。
     * @param code 错误分类。
     * @param details 诊断文本。
     */
    void reportError(const QString& filePath,
                     QtLogging::ErrorCode code,
                     const QString& details) const;

private:
    static constexpr qsizetype defaultBatchSize = 128; ///< 达到该记录数时立即安排一次异步刷盘。
    static constexpr int defaultFlushIntervalMs = 200; ///< 队列未达批量阈值时的最大定时等待毫秒数。
    static constexpr qint64 maximumCompressionInputBytes = 64LL * 1024LL * 1024LL; ///< qCompress 允许读取的最大源文件字节数。

    QHash<QString, Writer*> writers; ///< 路径到独占 Writer 的映射，元素在 Worker 析构时统一释放。
    QMutex queueMutex; ///< 保护 pendingLogs 与 isOverflowActive，可由所有生产线程竞争。
    QVector<PendingLog> pendingLogs; ///< 保持互斥量获取顺序的进程级待处理记录队列。
    bool isFlushScheduled = false; ///< 防止容量阈值连续投递重复的 queued flush 调用。
    bool isOverflowActive = false; ///< 标记当前持续满载区间已报告过 QueueOverflow。
    QTimer* flushTimer = nullptr; ///< 由本对象拥有、只在 Worker 线程启动和触发的周期定时器。
};

#endif // LOGWORKER_H
