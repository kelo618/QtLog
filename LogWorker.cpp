/************************************************************
 * File: LogWorker.cpp
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   实现 QtLog 共享 Worker 的有界队列、批量刷盘、文件滚动、`.qz` 压缩与归档清理。
 *   除 enqueueLog() 的短互斥区外，所有 Writer 与文件系统副作用均限定在后台线程执行。
 *   每个批次把完成数量和错误回报给公开门面，使调用方可判断丢弃、落盘和恢复状态。
 ************************************************************/

#include "LogWorker.h"

#include <QtLog/QtLog.h>

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTimer>

#include <utility>

/**
 * @brief 创建 Worker 并配置由其拥有的周期刷盘定时器。
 * @note Worker 无父对象，移动到专用线程后由线程结束信号释放。
 */
LogWorker::LogWorker()
    : QObject(nullptr)
    , flushTimer(new QTimer(this))
{
    // 定时器由 Worker 作为父对象拥有，移动 Worker 时会随对象一起切换线程亲和性。
    flushTimer->setInterval(defaultFlushIntervalMs);
    flushTimer->setSingleShot(false);

    // timeout 在 Worker 所在线程直接调用 flush，不产生额外跨线程文件访问。
    QObject::connect(flushTimer, &QTimer::timeout, this, &LogWorker::flush);
}

/**
 * @brief 析构 Worker 前处理仍在队列中的记录。
 * @note 正常退出已经由门面注册的退出回调主动刷盘，本调用作为生命周期兜底。
 */
LogWorker::~LogWorker()
{
    // 析构发生在所属线程，flush 会直接执行而不会排队到即将停止的事件循环。
    flush();

    // writers 中的普通指针均由 Worker 独占，批次结束后统一释放并清空映射。
    for (Writer* writer : writers) {
        delete writer;
    }
    writers.clear();
}

/**
 * @brief 启动周期刷盘定时器。
 */
void LogWorker::start()
{
    // QThread::started 只触发一次，定时器从此在 Worker 线程按固定间隔消费低流量记录。
    flushTimer->start();
}

/**
 * @brief 非阻塞提交一条日志到有界队列。
 * @param filePath 目标规范化路径。
 * @param message 最终文本。
 * @param level 日志等级。
 * @param output 输出目标。
 * @param writerOptions 文件策略快照。
 * @return 成功加入队列返回 true，容量已满返回 false。
 */
bool LogWorker::enqueueLog(const QString& filePath,
                           const QString& message,
                           QtLogging::Level level,
                           QtLogging::Output output,
                           const QtLogging::LoggerOptions& writerOptions)
{
    const qsizetype queueCapacity = QtLog::queueCapacity(); ///< 本次提交采用的进程队列容量快照。
    bool shouldScheduleFlush = false; ///< 离开互斥区后是否投递一次 queued flush。
    bool shouldReportOverflow = false; ///< 当前满载区间是否需要首次报告错误。
    bool isAccepted = false; ///< 当前记录是否实际进入 pendingLogs。

    {
        // 互斥区只执行容量判断和内存追加，不包含格式化、回调或文件操作。
        QMutexLocker locker(&queueMutex);

        // 达到或超过运行时容量时丢弃最新记录，已有记录和顺序保持不变。
        if (pendingLogs.size() >= queueCapacity) {
            if (!isOverflowActive) {
                isOverflowActive = true;
                shouldReportOverflow = true;
            }
        } else {
            // PendingLog 保存调用时完整策略，后续配置变化不回溯影响已排队记录。
            PendingLog log;
            log.filePath = filePath;
            log.message = message;
            log.level = level;
            log.output = output;
            log.writerOptions = writerOptions;
            pendingLogs.push_back(std::move(log));
            isAccepted = true;

            // 队列达到批次阈值时只安排一个异步 flush，定时器仍负责低流量记录。
            if (pendingLogs.size() >= defaultBatchSize && !isFlushScheduled) {
                isFlushScheduled = true;
                shouldScheduleFlush = true;
            }
        }
    }

    // 每个持续满载区间只报告一次；精确丢弃数量由 Logger 的 dropped 统计提供。
    if (shouldReportOverflow) {
        reportError(filePath,
                    QtLogging::ErrorCode::QueueOverflow,
                    QStringLiteral("QtLog queue reached capacity %1; newest records are being dropped.")
                        .arg(queueCapacity));
    }

    // queued 调用仅唤醒 Worker 事件循环，生产线程不会等待刷盘完成。
    if (shouldScheduleFlush) {
        QMetaObject::invokeMethod(this, &LogWorker::flush, Qt::QueuedConnection);
    }

    // 返回值只表示是否进入队列，不表示文件或控制台输出已完成。
    return isAccepted;
}

/**
 * @brief 确保日志文件父目录可用。
 * @param filePath 目标文件路径。
 * @return 目录存在或成功创建返回 true，否则返回 false。
 */
bool LogWorker::ensureDirectoryExists(const QString& filePath) const
{
    // QFileInfo 统一解析相对关系；公开门面已保证传入路径为绝对路径。
    const QFileInfo fileInfo(filePath);
    QDir directory = fileInfo.dir();

    // 已存在目录无需产生文件系统副作用。
    if (directory.exists()) {
        return true;
    }

    // mkpath(".") 在当前 QDir 表示创建该目录及缺失父级。
    if (!directory.mkpath(QStringLiteral("."))) {
        reportError(filePath,
                    QtLogging::ErrorCode::DirectoryCreateFailed,
                    QStringLiteral("Failed to create the parent directory for the log file."));
        return false;
    }

    // 父目录已经可供后续 QFile 打开。
    return true;
}

/**
 * @brief 打开 Writer 的活动文件并初始化 UTF-8 文本流。
 * @param writer 待打开 Writer。
 * @return 文件和流均可用时返回 true。
 */
bool LogWorker::openWriterFile(Writer& writer)
{
    // 目录创建失败时不尝试 QFile::open，保留路径供后续批次重试。
    if (!ensureDirectoryExists(writer.baseFilePath)) {
        return false;
    }

    // 活动日志始终以文本追加模式打开，已有内容不会被截断。
    writer.file.setFileName(writer.baseFilePath);
    if (!writer.file.open(QIODevice::Append | QIODevice::Text)) {
        reportError(writer.baseFilePath,
                    QtLogging::ErrorCode::FileOpenFailed,
                    writer.file.errorString());
        return false;
    }

    // 每次重新打开都重绑流、清除先前错误并固定 UTF-8 编码。
    writer.stream.setDevice(&writer.file);
    writer.stream.setEncoding(QStringConverter::Utf8);
    writer.stream.resetStatus();
    writer.openedDate = QDate::currentDate();
    writer.currentSizeBytes = writer.file.size();

    // Writer 已准备好接受当前及后续批次写入。
    return true;
}

/**
 * @brief 获取目标路径的缓存 Writer，必要时创建并打开。
 * @param filePath 规范化日志路径。
 * @return 可写 Writer 或空指针。
 */
LogWorker::Writer* LogWorker::ensureWriter(const QString& filePath)
{
    // writers 只在 Worker 线程访问，无需与生产队列共用互斥量。
    Writer* writer = writers.value(filePath, nullptr);
    if (writer == nullptr) {
        writer = new Writer;
        writer->baseFilePath = filePath;
        writers.insert(filePath, writer);
    }

    // 错误关闭后的 Writer 会在下一条记录到来时重新尝试打开。
    if (!writer->file.isOpen() && !openWriterFile(*writer)) {
        return nullptr;
    }

    // 返回 Worker 线程缓存的活动 Writer。
    return writer;
}

/**
 * @brief 关闭错误 Writer 以允许后续批次恢复。
 * @param writer 发生错误的 Writer。
 */
void LogWorker::closeWriterForRetry(Writer& writer) const
{
    // 先解除 QTextStream 设备，避免 QFile 关闭后流继续引用失效设备状态。
    writer.stream.setDevice(nullptr);
    if (writer.file.isOpen()) {
        writer.file.close();
    }
    writer.openedDate = {};
    writer.currentSizeBytes = 0;
}

/**
 * @brief 判断当前记录是否触发日期或大小滚动。
 * @param writer 活动 Writer。
 * @param log 待写记录。
 * @return 任一启用条件满足时返回 true。
 */
bool LogWorker::shouldRotate(const Writer& writer,
                             const PendingLog& log) const
{
    // 按天滚动在跨本地自然日后的首条记录写入前触发。
    const bool shouldRotateByDay = log.writerOptions.dailyRotationEnabled
                                   && writer.openedDate.isValid()
                                   && writer.openedDate != QDate::currentDate();

    // 按大小滚动把 UTF-8 字节数和行尾换行纳入阈值预测。
    const qint64 nextRecordBytes = serializedLineSize(log.message);
    const bool shouldRotateBySize = log.writerOptions.maxFileSizeBytes > 0
                                    && writer.currentSizeBytes > 0
                                    && writer.currentSizeBytes + nextRecordBytes
                                           > log.writerOptions.maxFileSizeBytes;

    // 任一策略触发即可在当前记录写入前滚动。
    return shouldRotateByDay || shouldRotateBySize;
}

/**
 * @brief 计算 QTextStream 通过文本模式持久化一条记录所占的实际字节数。
 * @param message 已格式化日志正文。
 * @return 包含消息内部换行和最终行尾的平台字节数。
 */
qint64 LogWorker::serializedLineSize(const QString& message)
{
    // UTF-8 字节数组给出正文基础大小，追加的一字节 LF 对应非 Windows 平台行尾。
    const QByteArray utf8Message = message.toUtf8();
    qint64 serializedBytes = utf8Message.size() + 1;

#ifdef Q_OS_WIN
    // QFile 文本模式把每个 LF 转换为 CRLF，消息内部换行与最终行尾都各增加一字节。
    serializedBytes += message.count(QLatin1Char('\n')) + 1;
#endif

    // 返回值与实际文件大小使用相同字节单位，可避免 Windows 下滚动阈值持续低估。
    return serializedBytes;
}

/**
 * @brief 构建唯一滚动归档路径。
 * @param filePath 活动日志路径。
 * @return 不与普通文件或 `.qz` 文件冲突的绝对路径。
 */
QString LogWorker::buildRolledFilePath(const QString& filePath) const
{
    // 文件信息提供目录、基础名和最后一个扩展名，保持多点文件名的主体部分。
    const QFileInfo fileInfo(filePath);
    const QString timestamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString baseName = fileInfo.completeBaseName();
    const QString suffix = fileInfo.suffix();

    // 同一毫秒多次滚动时从零开始尝试，后续冲突追加递增序号。
    int collisionIndex = 0;
    while (true) {
        const QString collisionSuffix = collisionIndex == 0
                                            ? QString()
                                            : QStringLiteral("_%1").arg(collisionIndex);
        const QString rolledName = suffix.isEmpty()
                                       ? QStringLiteral("%1_%2%3")
                                             .arg(baseName, timestamp, collisionSuffix)
                                       : QStringLiteral("%1_%2%3.%4")
                                             .arg(baseName, timestamp, collisionSuffix, suffix);
        const QString candidatePath = fileInfo.dir().absoluteFilePath(rolledName);

        // 同时检查未压缩和已压缩名称，禁止覆盖任何已有归档。
        if (!QFileInfo::exists(candidatePath)
            && !QFileInfo::exists(candidatePath + QStringLiteral(".qz"))) {
            return candidatePath;
        }

        // 下一轮只改变碰撞序号，不改变同一次滚动使用的时间戳。
        ++collisionIndex;
    }
}

/**
 * @brief 使用 qCompress 创建 `.qz` 归档并删除源文件。
 * @param filePath 已滚动普通文件路径。
 * @param loggerFilePath 公开 Logger 绑定的基础路径。
 * @return 完整压缩与源清理成功返回 true。
 */
bool LogWorker::compressAndRemoveFile(const QString& filePath,
                                      const QString& loggerFilePath) const
{
    // qCompress 需要把完整输入与输出同时保存在内存，超过安全上限时保留普通归档。
    const QFileInfo sourceInfo(filePath);
    if (sourceInfo.size() > maximumCompressionInputBytes) {
        reportError(loggerFilePath,
                    QtLogging::ErrorCode::CompressionSkippedTooLarge,
                    QStringLiteral("Rolled log exceeds the 64 MiB qCompress safety limit; the file remains uncompressed."));
        return false;
    }

    // 源文件只读打开失败时不创建目标归档，原始数据保持不变。
    QFile source(filePath);
    if (!source.open(QIODevice::ReadOnly)) {
        reportError(loggerFilePath,
                    QtLogging::ErrorCode::CompressionFailed,
                    source.errorString());
        return false;
    }

    // 输入受 64 MiB 上限保护，压缩级别六在体积与 Worker 占用时间之间保持平衡。
    const QByteArray sourceBytes = source.readAll();
    if (source.error() != QFileDevice::NoError
        || qint64(sourceBytes.size()) != sourceInfo.size()) {
        const QString errorDetails = source.errorString();
        source.close();
        reportError(loggerFilePath,
                    QtLogging::ErrorCode::CompressionFailed,
                    QStringLiteral("Failed to read the complete rolled log before compression: %1")
                        .arg(errorDetails));
        return false;
    }
    const QByteArray compressedBytes = qCompress(sourceBytes, 6);
    source.close();

    // 唯一路径生成器已经检查 `.qz` 冲突，Truncate 仅作用于本次新建归档。
    const QString archivePath = filePath + QStringLiteral(".qz");
    QFile archive(archivePath);
    if (!archive.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        reportError(loggerFilePath,
                    QtLogging::ErrorCode::CompressionFailed,
                    archive.errorString());
        return false;
    }

    // 必须写入完整 QByteArray；部分写入不构成可恢复的 qUncompress 输入。
    const qint64 writtenBytes = archive.write(compressedBytes);
    if (writtenBytes != compressedBytes.size() || !archive.flush()) {
        const QString errorDetails = archive.errorString();
        archive.close();
        QFile::remove(archivePath);
        reportError(loggerFilePath,
                    QtLogging::ErrorCode::CompressionFailed,
                    errorDetails);
        return false;
    }
    archive.close();

    // 只有压缩归档完整落盘后才删除普通源文件，失败时保留两份可恢复数据。
    if (!QFile::remove(filePath)) {
        reportError(loggerFilePath,
                    QtLogging::ErrorCode::CompressionFailed,
                    QStringLiteral("Compressed archive was written, but the uncompressed source could not be removed."));
        return false;
    }

    // `.qz` 已成为该次滚动的唯一归档文件。
    return true;
}

/**
 * @brief 删除严格匹配命名规则且超过保留天数的归档。
 * @param filePath 活动日志路径。
 * @param retentionDays 保留天数。
 */
void LogWorker::cleanupOldArchives(const QString& filePath, int retentionDays) const
{
    // 零表示调用方明确关闭自动删除，不扫描目录也不产生副作用。
    if (retentionDays <= 0) {
        return;
    }

    // 正则由已转义基础名和扩展名构建，只允许本库时间戳、碰撞序号与可选 `.qz`。
    const QFileInfo fileInfo(filePath);
    const QString escapedBaseName = QRegularExpression::escape(fileInfo.completeBaseName());
    const QString escapedSuffix = QRegularExpression::escape(fileInfo.suffix());
    const QString extensionPattern = escapedSuffix.isEmpty()
                                         ? QString()
                                         : QStringLiteral("\\.%1").arg(escapedSuffix);
    const QRegularExpression archivePattern(
        QStringLiteral("^%1_\\d{8}_\\d{6}_\\d{3}(?:_\\d+)?%2(?:\\.qz)?$")
            .arg(escapedBaseName, extensionPattern));
    const QDateTime expirationDeadline = QDateTime::currentDateTime().addDays(-retentionDays);

    // 目录枚举排除符号链接，避免清理越过日志目录触及外部文件。
    const QFileInfoList candidates = fileInfo.dir().entryInfoList(
        QDir::Files | QDir::NoSymLinks);

    // 每个候选必须同时满足精确命名和修改时间条件，名称相似文件不会被删除。
    for (const QFileInfo& candidate : candidates) {
        if (!archivePattern.match(candidate.fileName()).hasMatch()
            || candidate.lastModified() >= expirationDeadline) {
            continue;
        }

        // 删除失败不会中断其他归档检查，但必须向业务侧报告可诊断错误。
        if (!QFile::remove(candidate.absoluteFilePath())) {
            reportError(filePath,
                        QtLogging::ErrorCode::ArchiveCleanupFailed,
                        QStringLiteral("Failed to remove expired archive: %1")
                            .arg(candidate.absoluteFilePath()));
        }
    }
}

/**
 * @brief 滚动当前活动文件并重新打开基础路径。
 * @param writer 当前 Writer。
 * @param log 触发滚动的记录。
 * @param didRotate 输出滚动成功状态。
 * @param existingWritesFlushed 输出已有流内容是否确认刷盘。
 * @return 基础路径重新可写时返回 true。
 */
bool LogWorker::rotateWriter(Writer& writer,
                             const PendingLog& log,
                             bool& didRotate,
                             bool& existingWritesFlushed)
{
    // 调用方以 false 初始化两个输出，只有对应文件操作明确成功才改变状态。
    didRotate = false;
    existingWritesFlushed = false;

    // 滚动前先确认现有缓冲区已刷入文件；失败时关闭并尝试恢复当前活动文件。
    writer.stream.flush();
    const bool isStreamHealthy = writer.stream.status() == QTextStream::Ok;
    const bool isFileFlushed = writer.file.flush();
    if (!isStreamHealthy || !isFileFlushed) {
        reportError(writer.baseFilePath,
                    QtLogging::ErrorCode::FileFlushFailed,
                    writer.file.errorString());
        closeWriterForRetry(writer);
        return openWriterFile(writer);
    }

    // 只有流状态和 QFile::flush 都成功，当前批次此前的写入才可计入 fileWritten。
    existingWritesFlushed = true;

    // 关闭句柄后 Windows 才允许可靠重命名活动文件。
    writer.stream.setDevice(nullptr);
    writer.file.close();

    const QFileInfo activeFileInfo(writer.baseFilePath);
    if (activeFileInfo.exists()) {
        const QString rolledPath = buildRolledFilePath(writer.baseFilePath);
        if (!QFile::rename(writer.baseFilePath, rolledPath)) {
            reportError(writer.baseFilePath,
                        QtLogging::ErrorCode::RotationFailed,
                        QStringLiteral("Failed to rename the active log to %1.")
                            .arg(rolledPath));
        } else {
            // rename 成功即计为一次滚动；压缩失败不会撤销已经安全完成的重命名。
            didRotate = true;
            if (log.writerOptions.compressionEnabled) {
                compressAndRemoveFile(rolledPath, writer.baseFilePath);
            }

            // 保留清理在滚动之后执行，并只处理严格匹配的过期归档。
            cleanupOldArchives(writer.baseFilePath,
                               log.writerOptions.retentionDays);
        }
    }

    // 无论滚动、压缩或清理结果如何，都尝试恢复基础路径供当前记录写入。
    return openWriterFile(writer);
}

/**
 * @brief 将记录映射到 Qt 控制台输出。
 * @param log 待输出记录。
 */
void LogWorker::writeToConsole(const PendingLog& log) const
{
    // 每个公开等级使用相应 Qt 输出函数，Critical 不会像 qFatal 一样终止进程。
    switch (log.level) {
    case QtLogging::Level::Debug:
        qDebug().noquote() << log.message;
        break;
    case QtLogging::Level::Info:
        qInfo().noquote() << log.message;
        break;
    case QtLogging::Level::Warning:
        qWarning().noquote() << log.message;
        break;
    case QtLogging::Level::Error:
    case QtLogging::Level::Critical:
        qCritical().noquote() << log.message;
        break;
    }
}

/**
 * @brief 写入一批队列记录并按路径统一刷盘、回报统计。
 * @param batch 从共享队列交换出的不可变记录集合。
 */
void LogWorker::writeBatch(const QVector<PendingLog>& batch)
{
    // results 为每个 Logger 聚合完成、输出和滚动数量，并标识需要刷盘的记录数。
    QHash<QString, BatchResult> results;

    // 按队列顺序逐条处理，互斥量获取顺序即跨生产线程的持久化顺序。
    for (const PendingLog& log : batch) {
        BatchResult& result = results[log.filePath];
        ++result.completed;

        // 文件目标需要保证 Writer 可用，并在当前记录写入前应用滚动策略。
        if (log.output == QtLogging::Output::File
            || log.output == QtLogging::Output::FileAndConsole) {
            Writer* const writer = ensureWriter(log.filePath);
            if (writer != nullptr) {
                bool didRotate = false; ///< 当前记录前是否成功完成活动文件重命名。
                bool existingWritesFlushed = false; ///< 滚动前批次内容是否已确认刷盘。
                const bool rotationRequired = shouldRotate(*writer, log); ///< 当前记录是否跨越滚动边界。
                const bool isReadyForWrite = !rotationRequired
                                             || rotateWriter(*writer,
                                                             log,
                                                             didRotate,
                                                             existingWritesFlushed);

                // 滚动会结束一个刷盘确认区间；失败区间不应被后续成功刷盘误计。
                if (rotationRequired) {
                    if (existingWritesFlushed) {
                        result.fileWritten += result.pendingFileWrites;
                    }
                    result.pendingFileWrites = 0;
                }

                if (didRotate) {
                    ++result.rotations;
                }

                if (isReadyForWrite) {
                    // 写入流后立即检查编码/设备状态，失败 Writer 会关闭并在后续批次重试。
                    writer->stream << log.message << '\n';
                    if (writer->stream.status() != QTextStream::Ok) {
                        reportError(log.filePath,
                                    QtLogging::ErrorCode::FileWriteFailed,
                                    writer->file.errorString());
                        // 当前流错误使本确认区间内所有未刷盘记录都处于不确定状态。
                        result.pendingFileWrites = 0;
                        closeWriterForRetry(*writer);
                    } else {
                        ++result.pendingFileWrites;
                        writer->currentSizeBytes += serializedLineSize(log.message);
                    }
                }
            }
        }

        // 控制台输出与文件成功状态相互独立，文件失败时仍执行请求的控制台目标。
        if (log.output == QtLogging::Output::Console
            || log.output == QtLogging::Output::FileAndConsole) {
            writeToConsole(log);
            ++result.consoleWritten;
        }
    }

    // 只处理仍有待确认文件记录的结果项，无需维护第二个路径集合。
    for (auto iterator = results.begin(); iterator != results.end(); ++iterator) {
        const QString& filePath = iterator.key(); ///< 当前聚合结果对应的规范化日志路径。
        BatchResult& result = iterator.value(); ///< 当前路径可原地更新的批次统计。
        if (result.pendingFileWrites == 0) {
            continue;
        }

        Writer* const writer = writers.value(filePath, nullptr); ///< 当前路径由 Worker 独占的 Writer。

        // 写入过程中已经关闭的 Writer 不再刷盘，其待确认记录保持失败状态。
        if (writer == nullptr || !writer->file.isOpen()) {
            continue;
        }

        writer->stream.flush();
        const bool isStreamHealthy = writer->stream.status() == QTextStream::Ok;
        const bool isFileFlushed = writer->file.flush();
        if (isStreamHealthy && isFileFlushed) {
            result.fileWritten += result.pendingFileWrites;
        } else {
            reportError(filePath,
                        QtLogging::ErrorCode::FileFlushFailed,
                        writer->file.errorString());
            closeWriterForRetry(*writer);
        }
    }

    // 直接更新门面原子统计，flush() 返回前 pending 和输出数量已经反映当前批次。
    for (auto iterator = results.cbegin(); iterator != results.cend(); ++iterator) {
        const BatchResult& result = iterator.value();
        QtLog::recordBatchResult(iterator.key(),
                                 result.completed,
                                 result.fileWritten,
                                 result.consoleWritten,
                                 result.rotations);
    }
}

/**
 * @brief 刷出当前共享队列。
 * @note QtLog 门面负责线程切换，本函数始终在 Worker 线程执行。
 */
void LogWorker::flush()
{
    QVector<PendingLog> batch; ///< 本次独占处理的队列快照，离开互斥区后执行文件 I/O。
    {
        QMutexLocker locker(&queueMutex);
        isFlushScheduled = false;

        // swap 以常数时间清空共享队列；生产线程可在批次写盘期间继续提交新记录。
        if (!pendingLogs.isEmpty()) {
            batch.swap(pendingLogs);
        }

    }

    // 空批次不触发文件或统计回调，定时器等待下一轮即可。
    if (!batch.isEmpty()) {
        writeBatch(batch);
    }

    // 只有批次结束后共享队列仍为空才确认满载区间结束；持续生产期间不重复报告。
    {
        QMutexLocker locker(&queueMutex);
        if (pendingLogs.isEmpty()) {
            isOverflowActive = false;
        }
    }
}

/**
 * @brief 记录并转发基础设施错误。
 * @param filePath 错误路径。
 * @param code 错误分类。
 * @param details 诊断文本。
 */
void LogWorker::reportError(const QString& filePath,
                            QtLogging::ErrorCode code,
                            const QString& details) const
{
    // qWarning 保证即使业务侧未连接 errorOccurred，也能在开发环境看到失败原因。
    qWarning().noquote() << QStringLiteral("QtLog error [%1] %2: %3")
                                .arg(static_cast<int>(code))
                                .arg(filePath, details);

    // 门面只更新线程安全统计并排队公开信号，不会在当前线程执行用户槽。
    QtLog::recordWorkerError(filePath, code, details);
}
