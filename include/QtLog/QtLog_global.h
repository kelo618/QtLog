/************************************************************
 * File: QtLog_global.h
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   定义 QtLog 的公共导出宏、日志枚举、运行配置与统计快照。
 *   本文件只包含跨动态库边界稳定传递的值类型，不保存运行期状态，也不承担文件 I/O。
 *   所有类型均可直接用于日志配置、错误信号和线程安全统计读取。
 ************************************************************/

#ifndef QTLOG_GLOBAL_H
#define QTLOG_GLOBAL_H

#include <QObject>
#include <QString>
#include <QtCore/qglobal.h>

#if defined(QTLOG_STATIC)
#define QTLOG_EXPORT
#elif defined(QTLOG_LIBRARY)
#define QTLOG_EXPORT Q_DECL_EXPORT
#else
#define QTLOG_EXPORT Q_DECL_IMPORT
#endif

/**
 * @brief 保存 QtLog 对外公开的值类型。
 * @note 命名空间中的枚举已注册到 Qt 元对象系统，可安全用于跨线程信号参数。
 */
namespace QtLogging
{
Q_NAMESPACE_EXPORT(QTLOG_EXPORT)

inline constexpr qsizetype DefaultQueueCapacity = 16384; ///< 进程级待写队列的默认记录容量。
inline constexpr qsizetype MinimumQueueCapacity = 128; ///< 运行时允许设置的最小队列容量。

/**
 * @brief 表示日志严重程度，枚举顺序同时用于最低等级过滤。
 */
enum class Level : int {
    Debug = 0,    ///< 调试诊断信息，通常仅在开发或问题定位时启用。
    Info = 1,     ///< 正常运行过程中的业务与状态信息。
    Warning = 2,  ///< 可恢复异常或需要关注但不阻断运行的情况。
    Error = 3,    ///< 当前操作失败，但进程仍可继续运行的错误。
    Critical = 4  ///< 严重故障，调用方应立即告警或执行降级处理。
};
Q_ENUM_NS(Level)

/**
 * @brief 指定单条日志需要写入的目标集合。
 */
enum class Output : int {
    File = 0,          ///< 仅写入当前 Logger 绑定的文件。
    Console = 1,       ///< 仅通过 Qt 调试输出写入控制台。
    FileAndConsole = 2 ///< 同时写入文件与控制台。
};
Q_ENUM_NS(Output)

/**
 * @brief 标识调用方可观察的日志基础设施错误类型。
 */
enum class ErrorCode : int {
    DirectoryCreateFailed = 0,      ///< 无法创建日志文件所在目录。
    FileOpenFailed = 1,             ///< 无法以追加文本模式打开活动日志文件。
    FileWriteFailed = 2,            ///< 文本写入阶段检测到流错误。
    FileFlushFailed = 3,            ///< 批次刷盘失败，持久化状态无法确认。
    RotationFailed = 4,             ///< 活动文件无法重命名为滚动归档。
    CompressionFailed = 5,          ///< `.qz` 归档创建或源文件清理失败。
    CompressionSkippedTooLarge = 6, ///< 归档超过 64 MiB 安全上限，保留为未压缩文件。
    ArchiveCleanupFailed = 7,       ///< 符合过期规则的归档无法删除。
    QueueOverflow = 8               ///< 进程队列已满，最新提交记录被非阻塞丢弃。
};
Q_ENUM_NS(ErrorCode)

/**
 * @brief 保存单个 Logger 的文件策略与最低日志等级。
 * @note 配置可在运行期替换；每条日志在入队前获取一次不可变快照。
 */
struct QTLOG_EXPORT LoggerOptions
{
    Level minimumLevel = Level::Info; ///< 低于该等级的记录在调用线程直接过滤。
    qint64 maxFileSizeBytes = 0; ///< 大于零时启用按大小滚动，单位为字节；零表示关闭。
    int retentionDays = 0; ///< 大于零时删除超过该天数的本库归档；零表示关闭清理。
    bool dailyRotationEnabled = false; ///< 为 true 时，跨本地自然日后的首条记录触发滚动。
    bool compressionEnabled = false; ///< 为 true 时使用 qCompress 将滚动文件保存为 `.qz`。
};

/**
 * @brief 提供单个 Logger 自创建以来的线程安全累计统计。
 * @note 计数器不会自动清零；pending 表示已入队但尚未完成目标输出的记录数。
 */
struct QTLOG_EXPORT Statistics
{
    quint64 submitted = 0; ///< 调用 write() 的总次数，包含过滤和丢弃记录。
    quint64 filtered = 0; ///< 因低于 minimumLevel 而未进入队列的记录数。
    quint64 enqueued = 0; ///< 成功进入进程级工作队列的记录数。
    quint64 dropped = 0; ///< 因队列容量不足而丢弃的最新记录数。
    quint64 fileWritten = 0; ///< 已确认批次刷盘成功的文件记录数。
    quint64 consoleWritten = 0; ///< 已发送到 Qt 控制台输出函数的记录数。
    quint64 errors = 0; ///< 除 QueueOverflow 外已报告的基础设施错误次数。
    quint64 rotations = 0; ///< 成功完成活动文件重命名的滚动次数。
    qsizetype pending = 0; ///< 当前仍在队列或工作批次中的记录数。
};
} // namespace QtLogging

Q_DECLARE_METATYPE(QtLogging::ErrorCode)

#endif // QTLOG_GLOBAL_H
