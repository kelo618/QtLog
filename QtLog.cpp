/************************************************************
 * File: QtLog.cpp
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   实现 QtLog 对外门面、路径实例注册表、实例配置与原子统计。
 *   进程状态和实例字段全部隐藏在本文件，调用线程只执行过滤、格式化与非阻塞入队。
 *   共享 Worker 负责文件副作用，退出回调统一刷空队列并回收后台线程。
 ************************************************************/

#include <QtLog/QtLog.h>

#include "LogWorker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSharedPointer>
#include <QThread>

#include <atomic>
#include <mutex>
#include <utility>

/**
 * @brief 保存单个 Logger 的不可见状态，避免公共头传递同步与容器依赖。
 * @note 对象由对应 QtLog 独占，所有累计计数使用 relaxed 原子操作提供线程安全快照。
 */
struct QtLog::Private
{
    QString logFilePath; ///< 当前实例绑定的规范化绝对路径，构造后不再变化。
    mutable QMutex optionsMutex; ///< 保护 loggerOptions 的整体读取和替换。
    QtLogging::LoggerOptions loggerOptions; ///< 当前实例生效的等级、滚动、保留与压缩配置。
    std::atomic<quint64> submittedCount{0}; ///< write() 调用总量。
    std::atomic<quint64> filteredCount{0}; ///< 在调用线程被最低等级过滤的记录量。
    std::atomic<quint64> enqueuedCount{0}; ///< 成功放入共享队列的记录量。
    std::atomic<quint64> droppedCount{0}; ///< 因队列满或 Worker 退出而丢弃的记录量。
    std::atomic<quint64> fileWrittenCount{0}; ///< 已确认刷盘成功的文件记录量。
    std::atomic<quint64> consoleWrittenCount{0}; ///< 已发送到 Qt 控制台的记录量。
    std::atomic<quint64> errorCount{0}; ///< QueueOverflow 之外的基础设施错误量。
    std::atomic<quint64> rotationCount{0}; ///< 成功滚动活动文件的次数。
    std::atomic<qsizetype> pendingCount{0}; ///< 已入队但尚未完成处理的记录量。
};

/**
 * @brief 保存仅供本实现文件使用的进程状态和辅助函数。
 */
namespace
{
/**
 * @brief 集中保存全部 Logger 共享的实例表、工作线程和队列容量。
 * @note 状态按首次调用初始化并持续至进程静态对象销毁阶段。
 */
struct ProcessState
{
    QMutex instanceMutex; ///< 保护 instances 的查找、插入与回调路由。
    QHash<QString, QSharedPointer<QtLog>> instances; ///< 平台路径键到进程生命周期 Logger 的映射。
    std::once_flag workerOnce; ///< 保证共享线程基础设施仅初始化一次。
    QPointer<QThread> workerThread; ///< 主动拥有并在退出阶段删除的后台线程对象。
    QPointer<LogWorker> worker; ///< 在线程结束时通过 deleteLater 回收的后台 Worker。
    std::atomic<qsizetype> queueCapacity{QtLogging::DefaultQueueCapacity}; ///< 运行时队列容量。
};

/**
 * @brief 返回延迟初始化的进程共享状态。
 * @return 进程内唯一且持续到静态销毁阶段的状态引用。
 */
ProcessState& processState()
{
    static ProcessState state; ///< 首次使用时构造，避免公共类暴露进程级静态成员。
    return state;
}

/**
 * @brief 将调用方路径规范化为实例注册表使用的绝对路径。
 * @param targetFilePath 原始路径；全空白内容视为无效输入。
 * @return 清理后的绝对路径，输入无效时返回空字符串。
 */
QString normalizeFilePath(const QString& targetFilePath)
{
    // 空白路径无法稳定映射到日志文件，直接拒绝以免意外写入当前目录。
    if (targetFilePath.trimmed().isEmpty()) {
        return {};
    }

    // 统一分隔符后解析绝对位置，再折叠可安全消除的 `.` 与 `..` 片段。
    const QString independentPath = QDir::fromNativeSeparators(targetFilePath);
    const QString absolutePath = QFileInfo(independentPath).absoluteFilePath();
    return QDir::cleanPath(absolutePath);
}

/**
 * @brief 构建平台一致的 Logger 实例键。
 * @param normalizedFilePath 已规范化的绝对路径。
 * @return Windows 返回大小写折叠键，其他平台返回原路径。
 */
QString instanceKey(const QString& normalizedFilePath)
{
#ifdef Q_OS_WIN
    // Windows 常规文件系统不区分路径大小写，折叠后可避免重复 Logger。
    return normalizedFilePath.toCaseFolded();
#else
    // Linux 文件系统通常区分大小写，注册表必须保留原有路径语义。
    return normalizedFilePath;
#endif
}

/**
 * @brief 将日志等级转换为固定文本标签。
 * @param level 日志严重程度。
 * @return 对应方括号标签，非法枚举返回 UNKNOWN。
 */
QString levelText(QtLogging::Level level)
{
    // switch 明确覆盖全部公开枚举值，避免数值顺序变化影响文本协议。
    switch (level) {
    case QtLogging::Level::Debug:
        return QStringLiteral("[DEBUG]");
    case QtLogging::Level::Info:
        return QStringLiteral("[INFO]");
    case QtLogging::Level::Warning:
        return QStringLiteral("[WARNING]");
    case QtLogging::Level::Error:
        return QStringLiteral("[ERROR]");
    case QtLogging::Level::Critical:
        return QStringLiteral("[CRITICAL]");
    }

    // 强制转换产生的未定义值保留可诊断标签，不中断业务线程。
    return QStringLiteral("[UNKNOWN]");
}

/**
 * @brief 生成固定格式的本地秒级时间戳。
 * @return `yyyy-MM-dd HH:mm:ss` 格式文本。
 */
QString currentTimestamp()
{
    // 本地时间便于直接阅读，格式固定以保证每条记录结构一致。
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

/**
 * @brief 在进程退出阶段同步刷空队列并停止后台线程。
 * @note 本函数允许重复进入；空线程指针表示基础设施未创建或已经回收。
 */
void shutdownWorker()
{
    ProcessState& state = processState(); ///< 当前进程共享状态，后续统一清空 Worker 指针。
    QThread* const threadSnapshot = state.workerThread.data(); ///< 等待期间保持稳定的线程对象地址。
    LogWorker* const workerSnapshot = state.worker.data(); ///< 停止前需要完成最终刷盘的 Worker。

    // 未使用过 QtLog 时没有后台资源，退出回调可以直接结束。
    if (threadSnapshot == nullptr) {
        return;
    }

    // 正常退出来自应用线程；同线程分支避免阻塞队列连接自锁。
    if (workerSnapshot != nullptr) {
        if (QThread::currentThread() == workerSnapshot->thread()) {
            workerSnapshot->flush();
        } else {
            QMetaObject::invokeMethod(workerSnapshot,
                                      &LogWorker::flush,
                                      Qt::BlockingQueuedConnection);
        }
    }

    // 请求事件循环退出并等待 Worker 的 deleteLater 完成，再释放线程对象本身。
    threadSnapshot->quit();
    if (QThread::currentThread() != threadSnapshot) {
        threadSnapshot->wait();
        delete threadSnapshot;
    }

    // 清空非拥有观察指针，退出阶段的后续 write() 会按 dropped 统计。
    state.workerThread = nullptr;
    state.worker = nullptr;
}

/**
 * @brief 在进程生命周期内创建唯一共享 Worker 与后台线程。
 * @note 调用前已经确认 QCoreApplication 存在，call_once 提供并发初始化屏障。
 */
void initWorkerOnce()
{
    ProcessState& state = processState(); ///< 初始化闭包持有的进程状态具有静态生命周期。

    // 闭包在首个调用线程执行，只创建对象和连接，不执行日志文件 I/O。
    std::call_once(state.workerOnce, [&state]() {
        // 后台线程由退出回调显式停止和删除，不依赖外部 QObject 所有权。
        state.workerThread = new QThread();
        state.workerThread->setObjectName(QStringLiteral("QtLogWorkerThread"));

        // Worker 无父对象，移动线程后由 QThread::finished 安排安全回收。
        state.worker = new LogWorker();
        state.worker->moveToThread(state.workerThread);
        QObject::connect(state.workerThread, &QThread::started, state.worker, &LogWorker::start);
        QObject::connect(state.workerThread, &QThread::finished, state.worker, &QObject::deleteLater);

        // 线程启动后注册退出回调，正常进程关闭会先刷盘再释放后台对象。
        state.workerThread->start();
        qAddPostRoutine(&shutdownWorker);
    });
}
} // namespace

/**
 * @brief 返回规范化路径对应的进程级 Logger。
 * @param targetFilePath 原始日志文件路径。
 * @return 路径和应用上下文有效时返回稳定指针，否则返回 nullptr。
 */
QtLog* QtLog::instance(const QString& targetFilePath)
{
    const QString normalizedPath = normalizeFilePath(targetFilePath); ///< 实例键与实际文件共用的绝对路径。
    if (normalizedPath.isEmpty()) {
        qWarning().noquote() << QStringLiteral("QtLog requires a non-empty log file path.");
        return nullptr;
    }

    // Qt 线程与退出回调依赖 QCoreApplication，缺少应用对象时无法安全启动 Worker。
    if (QCoreApplication::instance() == nullptr) {
        qWarning().noquote() << QStringLiteral("QtLog must be used after QCoreApplication is created.");
        return nullptr;
    }

    // 工作线程初始化独立于实例注册表锁，避免启动路径长期占用全局互斥量。
    initWorkerOnce();

    ProcessState& state = processState(); ///< 提供实例表及其互斥量的进程状态。
    const QString key = instanceKey(normalizedPath); ///< 合并平台上等价路径的注册表键。
    QMutexLocker locker(&state.instanceMutex); ///< 覆盖查找与插入，防止并发创建重复实例。

    // 注册表共享所有权保证返回裸指针在进程退出前始终有效。
    const auto existing = state.instances.constFind(key);
    if (existing != state.instances.cend()) {
        return existing.value().data();
    }

    // 新实例无 QObject 父对象，其唯一长期所有权由进程注册表承担。
    QSharedPointer<QtLog> logger(new QtLog(normalizedPath));
    QtLog* const loggerPointer = logger.data(); ///< 返回调用方但不转移所有权的稳定地址。
    state.instances.insert(key, std::move(logger));
    return loggerPointer;
}

/**
 * @brief 更新进程级有界队列容量。
 * @param capacity 新容量，不能小于公开最小值。
 * @return 参数合法并保存时返回 true，否则返回 false。
 */
bool QtLog::setQueueCapacity(qsizetype capacity)
{
    // 小于单批阈值的容量会造成持续抖动，作为公开边界直接拒绝。
    if (capacity < QtLogging::MinimumQueueCapacity) {
        return false;
    }

    // 原子配置只影响后续提交，不会主动删除已经进入队列的记录。
    processState().queueCapacity.store(capacity, std::memory_order_relaxed);
    return true;
}

/**
 * @brief 返回当前进程级队列容量。
 * @return 后续提交采用的最大待处理记录数。
 */
qsizetype QtLog::queueCapacity()
{
    // relaxed 读取只提供运行配置，不与具体日志内容建立跨线程顺序关系。
    return processState().queueCapacity.load(std::memory_order_relaxed);
}

/**
 * @brief 同步处理所有已入队记录。
 * @note 同线程调用直接进入 Worker，跨线程调用阻塞至当前队列完成。
 */
void QtLog::flushAll()
{
    LogWorker* const workerSnapshot = processState().worker.data(); ///< 本次同步边界使用的 Worker 地址。
    if (workerSnapshot == nullptr) {
        return;
    }

    // 工作线程内直接调用可避免 BlockingQueuedConnection 自锁。
    if (QThread::currentThread() == workerSnapshot->thread()) {
        workerSnapshot->flush();
        return;
    }

    // 普通线程等待 Worker 完成当前队列，返回后统计已经同步更新。
    QMetaObject::invokeMethod(workerSnapshot,
                              &LogWorker::flush,
                              Qt::BlockingQueuedConnection);
}

/**
 * @brief 构造绑定到规范化路径的 Logger 及其私有状态。
 * @param normalizedFilePath 已验证的规范化绝对路径。
 */
QtLog::QtLog(const QString& normalizedFilePath)
    : QObject(nullptr)
    , privateData(new Private)
{
    // 文件路径构造后不再改变，所有公开写入均通过该路径路由。
    privateData->logFilePath = normalizedFilePath;
}

/**
 * @brief 释放当前 Logger 独占的私有状态。
 */
QtLog::~QtLog()
{
    // 注册表保证析构发生在进程尾部，此时私有原子状态已不再被 Worker 更新。
    delete privateData;
    privateData = nullptr;
}

/**
 * @brief 校验并整体替换实例配置。
 * @param newOptions 调用方提供的新配置。
 * @return 所有字段有效时返回 true，否则返回 false。
 */
bool QtLog::setOptions(const QtLogging::LoggerOptions& newOptions)
{
    // 负数没有稳定业务语义，拒绝整个快照以避免部分配置已经生效。
    if (newOptions.maxFileSizeBytes < 0 || newOptions.retentionDays < 0) {
        return false;
    }

    const int minimumLevelValue = static_cast<int>(newOptions.minimumLevel); ///< 校验强制转换产生的等级值。
    if (minimumLevelValue < static_cast<int>(QtLogging::Level::Debug)
        || minimumLevelValue > static_cast<int>(QtLogging::Level::Critical)) {
        return false;
    }

    // 互斥量保证 write() 不会观察到多个字段拼接出的中间配置。
    QMutexLocker locker(&privateData->optionsMutex);
    privateData->loggerOptions = newOptions;
    return true;
}

/**
 * @brief 获取实例配置快照。
 * @return 锁保护下复制出的完整配置。
 */
QtLogging::LoggerOptions QtLog::options() const
{
    // 配置整体复制保证单条日志使用同一时刻的滚动与过滤策略。
    QMutexLocker locker(&privateData->optionsMutex);
    return privateData->loggerOptions;
}

/**
 * @brief 组合实例累计统计。
 * @return 调用时刻各原子计数器的线程安全近似快照。
 */
QtLogging::Statistics QtLog::statistics() const
{
    QtLogging::Statistics snapshot; ///< 返回调用方的独立统计值对象。
    snapshot.submitted = privateData->submittedCount.load(std::memory_order_relaxed);
    snapshot.filtered = privateData->filteredCount.load(std::memory_order_relaxed);
    snapshot.enqueued = privateData->enqueuedCount.load(std::memory_order_relaxed);
    snapshot.dropped = privateData->droppedCount.load(std::memory_order_relaxed);
    snapshot.fileWritten = privateData->fileWrittenCount.load(std::memory_order_relaxed);
    snapshot.consoleWritten = privateData->consoleWrittenCount.load(std::memory_order_relaxed);
    snapshot.errors = privateData->errorCount.load(std::memory_order_relaxed);
    snapshot.rotations = privateData->rotationCount.load(std::memory_order_relaxed);
    snapshot.pending = privateData->pendingCount.load(std::memory_order_relaxed);
    return snapshot;
}

/**
 * @brief 返回实例绑定路径。
 * @return 构造时保存且不可变的绝对路径。
 */
QString QtLog::filePath() const
{
    // 路径在构造后只读，无需额外同步即可安全复制。
    return privateData->logFilePath;
}

/**
 * @brief 过滤、格式化并非阻塞提交日志。
 * @param message 业务正文。
 * @param level 日志等级。
 * @param output 输出目标。
 */
void QtLog::write(const QString& message,
                  QtLogging::Level level,
                  QtLogging::Output output)
{
    // submitted 覆盖全部调用，使过滤、入队与丢弃数量可以解释完整输入。
    privateData->submittedCount.fetch_add(1, std::memory_order_relaxed);

    const QtLogging::LoggerOptions optionSnapshot = options(); ///< 当前记录不可变的过滤与文件策略。
    if (static_cast<int>(level) < static_cast<int>(optionSnapshot.minimumLevel)) {
        privateData->filteredCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    LogWorker* const workerSnapshot = processState().worker.data(); ///< 当前可接收入队请求的共享 Worker。
    if (workerSnapshot == nullptr) {
        privateData->droppedCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // 固定格式只包含等级、时间和正文，不加入隐式线程、源文件或结构化字段。
    const QString formattedMessage = QStringLiteral("%1 %2 : %3")
                                         .arg(levelText(level), currentTimestamp(), message);

    // 先登记潜在入队记录，避免 Worker 极快完成时 pending 出现先减后加。
    privateData->enqueuedCount.fetch_add(1, std::memory_order_relaxed);
    privateData->pendingCount.fetch_add(1, std::memory_order_relaxed);

    const bool isEnqueued = workerSnapshot->enqueueLog(privateData->logFilePath,
                                                        formattedMessage,
                                                        level,
                                                        output,
                                                        optionSnapshot); ///< 只表示是否进入内存队列。
    if (!isEnqueued) {
        // 队列拒绝后撤销预登记，并把当前最新记录归入 dropped。
        privateData->enqueuedCount.fetch_sub(1, std::memory_order_relaxed);
        privateData->pendingCount.fetch_sub(1, std::memory_order_relaxed);
        privateData->droppedCount.fetch_add(1, std::memory_order_relaxed);
    }
}

/**
 * @brief 更新对应 Logger 的批次完成统计。
 * @param normalizedFilePath 批次路径。
 * @param completed 完成记录数。
 * @param fileWritten 文件成功数。
 * @param consoleWritten 控制台成功数。
 * @param rotations 滚动成功数。
 */
void QtLog::recordBatchResult(const QString& normalizedFilePath,
                              quint64 completed,
                              quint64 fileWritten,
                              quint64 consoleWritten,
                              quint64 rotations)
{
    ProcessState& state = processState(); ///< 提供路径到 Logger 的线程安全注册表。
    QSharedPointer<QtLog> logger; ///< 离开注册表锁后继续保证目标 Logger 存活。
    {
        QMutexLocker locker(&state.instanceMutex); ///< 保护本次注册表查询。
        logger = state.instances.value(instanceKey(normalizedFilePath));
    }

    // 理论上所有记录都来自注册实例，找不到时不再更新公开状态。
    if (logger.isNull()) {
        return;
    }

    // enqueue 已预登记 pending，Worker 完成后可安全扣减并累计确认输出。
    logger->privateData->pendingCount.fetch_sub(static_cast<qsizetype>(completed),
                                                std::memory_order_relaxed);
    logger->privateData->fileWrittenCount.fetch_add(fileWritten, std::memory_order_relaxed);
    logger->privateData->consoleWrittenCount.fetch_add(consoleWritten, std::memory_order_relaxed);
    logger->privateData->rotationCount.fetch_add(rotations, std::memory_order_relaxed);
}

/**
 * @brief 累计 Worker 错误并向 Logger 所在线程投递公开信号。
 * @param normalizedFilePath 错误路径。
 * @param code 错误分类。
 * @param details 诊断文本。
 */
void QtLog::recordWorkerError(const QString& normalizedFilePath,
                              QtLogging::ErrorCode code,
                              const QString& details)
{
    ProcessState& state = processState(); ///< 提供错误路径对应的 Logger 注册表。
    QSharedPointer<QtLog> logger; ///< 排队信号执行前维持接收对象生命周期。
    {
        QMutexLocker locker(&state.instanceMutex); ///< 保护本次注册表查询。
        logger = state.instances.value(instanceKey(normalizedFilePath));
    }

    // 无对应 Logger 时内部 qWarning 已保留诊断，不向不存在的对象投递事件。
    if (logger.isNull()) {
        return;
    }

    // QueueOverflow 已由 dropped 精确计数，errors 只累计文件和归档失败。
    if (code != QtLogging::ErrorCode::QueueOverflow) {
        logger->privateData->errorCount.fetch_add(1, std::memory_order_relaxed);
    }

    // 捕获共享指针维持对象寿命，context 确保用户槽只在 Logger 线程执行。
    QMetaObject::invokeMethod(logger.data(),
                              [logger, code, normalizedFilePath, details]() {
                                  emit logger->errorOccurred(code,
                                                             normalizedFilePath,
                                                             details);
                              },
                              Qt::QueuedConnection);
}
