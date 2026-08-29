/************************************************************
 * File: QtLog.h
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   声明 QtLog 异步日志门面，负责按规范化文件路径管理进程级 Logger 实例。
 *   调用线程在此完成等级过滤、文本格式化与非阻塞入队，文件 I/O 统一交给共享工作线程。
 *   实例状态通过私有实现隐藏，公共头不暴露队列、线程、容器或原子计数器等实现依赖。
 ************************************************************/

#ifndef QTLOG_H
#define QTLOG_H

#include "QtLog_global.h"

#include <QObject>
#include <QString>

class LogWorker;

/**
 * @brief 提供线程安全的异步文件与控制台日志能力。
 *
 * 每个规范化文件路径在进程内只对应一个实例，实例由内部注册表持有至进程退出。
 * 所有实例共享一个后台工作线程和有界队列，调用方不得手动析构 instance() 返回的指针。
 */
class QTLOG_EXPORT QtLog : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 返回指定日志文件对应的进程级 Logger，必要时创建实例与共享工作线程。
     * @param targetFilePath 调用方提供的日志文件路径；不能为空，相对路径按调用时工作目录转为绝对路径。
     * @return 路径有效且 QCoreApplication 已存在时返回进程生命周期指针，否则返回 nullptr。
     * @note Windows 使用大小写不敏感的实例键，Linux 保持路径大小写语义。
     */
    static QtLog* instance(const QString& targetFilePath);

    /**
     * @brief 设置进程级有界队列容量，后续提交立即采用新容量。
     * @param capacity 最大待处理记录数，必须大于或等于 QtLogging::MinimumQueueCapacity。
     * @return 容量合法并成功保存时返回 true；非法值不会改变原配置并返回 false。
     * @note 降低容量不会删除已入队记录，新提交会在队列降到新上限前被丢弃。
     */
    static bool setQueueCapacity(qsizetype capacity);

    /**
     * @brief 返回当前进程级有界队列容量。
     * @return 当前允许同时等待处理的最大记录数。
     */
    static qsizetype queueCapacity();

    /**
     * @brief 同步处理所有 Logger 已入队的记录。
     * @note 从普通线程调用时阻塞至工作线程完成；从工作线程调用时直接执行以避免死锁。
     */
    static void flushAll();

    /**
     * @brief 析构 Logger 门面对象。
     * @note 实例由内部注册表管理，正常业务代码不应主动析构。
     */
    ~QtLog() override;

    /**
     * @brief 原子替换当前 Logger 的全部运行配置。
     * @param newOptions 新配置；文件大小和保留天数不得为负数。
     * @return 全部字段有效并完成替换时返回 true，否则保留旧配置并返回 false。
     */
    bool setOptions(const QtLogging::LoggerOptions& newOptions);

    /**
     * @brief 获取当前 Logger 配置的一致性快照。
     * @return 调用时刻生效的等级、滚动、保留与压缩配置。
     */
    QtLogging::LoggerOptions options() const;

    /**
     * @brief 获取当前 Logger 自创建以来的累计统计快照。
     * @return 由原子计数器组合而成的线程安全统计值。
     */
    QtLogging::Statistics statistics() const;

    /**
     * @brief 返回当前 Logger 绑定的规范化绝对文件路径。
     * @return 创建实例时固定保存的文件路径。
     */
    QString filePath() const;

    /**
     * @brief 过滤、格式化并非阻塞提交一条日志记录。
     * @param message 业务日志正文；换行符按原内容保留。
     * @param level 用于过滤、文本标签和控制台函数映射的严重程度。
     * @param output 指定写文件、写控制台或同时写入两者。
     * @note 队列满时丢弃当前最新记录并更新 dropped，不等待后台磁盘 I/O。
     */
    void write(const QString& message,
               QtLogging::Level level = QtLogging::Level::Info,
               QtLogging::Output output = QtLogging::Output::File);

signals:
    /**
     * @brief 后台基础设施错误需要业务侧记录、告警或展示时发出。
     * @param code 稳定的错误分类，便于调用方选择处理策略。
     * @param filePath 发生错误的规范化日志文件路径。
     * @param details 面向诊断的自然语言细节，不作为机器解析协议。
     * @note 信号投递到 Logger 所在线程；该线程需要运行 Qt 事件循环才能及时接收。
     */
    void errorOccurred(QtLogging::ErrorCode code,
                       const QString& filePath,
                       const QString& details);

private:
    /** @brief 允许内部 Worker 直接回报批次结果和错误，避免额外回调包装层。 */
    friend class LogWorker;

    /** @brief 保存仅在实现文件可见的实例状态，隔离公共头与内部依赖。 */
    struct Private;

    /**
     * @brief 构造绑定到唯一规范化路径的 Logger。
     * @param normalizedFilePath 已完成绝对化与清理的文件路径。
     */
    explicit QtLog(const QString& normalizedFilePath);

    /**
     * @brief 接收 Worker 批次结果并更新对应 Logger 的原子统计。
     * @param normalizedFilePath 批次记录所属的规范化路径。
     * @param completed 已完成处理并可从 pending 扣除的记录数。
     * @param fileWritten 已确认文件刷盘成功的记录数。
     * @param consoleWritten 已发送到控制台的记录数。
     * @param rotations 本批次成功完成的滚动次数。
     */
    static void recordBatchResult(const QString& normalizedFilePath,
                                  quint64 completed,
                                  quint64 fileWritten,
                                  quint64 consoleWritten,
                                  quint64 rotations);

    /**
     * @brief 接收 Worker 错误并投递给对应 Logger 的信号接收方。
     * @param normalizedFilePath 错误关联的规范化路径。
     * @param code 错误分类。
     * @param details 诊断文本。
     */
    static void recordWorkerError(const QString& normalizedFilePath,
                                  QtLogging::ErrorCode code,
                                  const QString& details);

    Private* privateData = nullptr; ///< 由当前 Logger 独占并在析构时释放的实例状态。
};

#endif // QTLOG_H
