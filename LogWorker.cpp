/*
 * =====================================================================
 * File: LogWorker.cpp
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   Queue-driven worker implementation for batch logging and rotation.
 *   基于队列的批量日志写入与滚动实现。
 * =====================================================================
 */

#include "LogWorker.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSet>
#include <QThread>

// Initializes periodic flush timer on worker object creation.
// 在工作对象创建时初始化周期刷盘定时器。
LogWorker::LogWorker(QObject* parent)
    : QObject(parent)
    , flushTimer(new QTimer(this))
{
    flushTimer->setInterval(defaultFlushIntervalMs);
    flushTimer->setSingleShot(false);
    QObject::connect(flushTimer, &QTimer::timeout, this, &LogWorker::flush);
}

// Ensures pending logs are flushed before worker destruction.
// 在工作对象销毁前确保待写日志全部刷盘。
LogWorker::~LogWorker()
{
    flush();
}

// Starts periodic flush timer if it is not already active.
// 若定时器未启动则启动周期 flush。
void LogWorker::start()
{
    if (!flushTimer->isActive()) {
        flushTimer->start();
    }
}

// Pushes a log record into queue and schedules flush when threshold is reached.
// 将日志记录压入队列，达到阈值时调度一次 flush。
void LogWorker::enqueueLog(const QString& filePath,
                           const QString& message,
                           QwtLogger::LogLevel level,
                           QwtLogger::OutputOption option,
                           const QwtLogger::WriterOptions& writerOptions)
{
    bool shouldScheduleFlush = false;
    {
        QMutexLocker locker(&queueMutex);
        PendingLog log;
        log.filePath = filePath;
        log.message = message;
        log.level = level;
        log.option = option;
        log.writerOptions = writerOptions;
        pendingLogs.push_back(log);

        if (pendingLogs.size() >= batchSize && !flushScheduled) {
            flushScheduled = true;
            shouldScheduleFlush = true;
        }
    }

    if (shouldScheduleFlush) {
        QMetaObject::invokeMethod(this, &LogWorker::flush, Qt::QueuedConnection);
    }
}

// Creates parent directory for target log file when needed.
// 必要时创建目标日志文件的父目录。
bool LogWorker::ensureDirectoryExists(const QString& filePath) const
{
    const QFileInfo fileInfo(filePath);
    QDir dir = fileInfo.dir();

    if (dir.exists()) {
        return true;
    }

    if (!dir.mkpath(".")) {
        qWarning() << "Failed to create directory for log file:" << filePath;
        return false;
    }

    return true;
}

// Opens writer file in append mode and resets stream/date metadata.
// 以追加模式打开写入文件并更新流与日期元数据。
bool LogWorker::openWriterFile(const QSharedPointer<Writer>& writer)
{
    if (!writer) {
        return false;
    }

    if (!ensureDirectoryExists(writer->baseFilePath)) {
        return false;
    }

    writer->file.setFileName(writer->baseFilePath);
    if (!writer->file.open(QIODevice::Append | QIODevice::Text)) {
        qWarning() << "Failed to open log file:" << writer->baseFilePath;
        return false;
    }

    writer->stream.setDevice(&writer->file);
    writer->openedDate = QDate::currentDate();
    return true;
}

// Returns cached writer or creates a new writer for target file.
// 返回缓存写入器，若不存在则为目标文件创建写入器。
QSharedPointer<LogWorker::Writer> LogWorker::ensureWriter(const QString& filePath)
{
    auto writer = writers.value(filePath);
    if (!writer) {
        writer = QSharedPointer<Writer>::create();
        writer->baseFilePath = filePath;
        writers.insert(filePath, writer);
    }

    if (!writer->file.isOpen() && !openWriterFile(writer)) {
        return {};
    }

    return writer;
}

// Decides whether file should rotate by day boundary or file size limit.
// 按跨天或文件大小阈值判断是否需要滚动。
bool LogWorker::shouldRotate(const QSharedPointer<Writer>& writer, const PendingLog& log) const
{
    if (!writer || !writer->file.isOpen()) {
        return false;
    }

    const bool rotateByDay = log.writerOptions.dailyRotationEnabled
                             && writer->openedDate.isValid()
                             && writer->openedDate != QDate::currentDate();

    const bool rotateBySize = log.writerOptions.maxFileSizeBytes > 0
                              && (writer->file.size() + log.message.toUtf8().size() + 1
                                  > log.writerOptions.maxFileSizeBytes);

    return rotateByDay || rotateBySize;
}

// Builds rotated filename with millisecond timestamp suffix.
// 构造带毫秒时间戳后缀的滚动文件名。
QString LogWorker::buildRolledFilePath(const QString& filePath) const
{
    const QFileInfo fileInfo(filePath);
    const QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
    const QString baseName = fileInfo.completeBaseName();
    const QString suffix = fileInfo.suffix();
    const QString rolledName = suffix.isEmpty()
                                   ? QString("%1_%2").arg(baseName, timestamp)
                                   : QString("%1_%2.%3").arg(baseName, timestamp, suffix);
    return fileInfo.dir().absoluteFilePath(rolledName);
}

// Compresses rolled file into .qz and removes original rolled file.
// 将滚动文件压缩为 .qz 并删除原滚动文件。
bool LogWorker::compressAndRemoveFile(const QString& filePath) const
{
    QFile source(filePath);
    if (!source.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open rolled log file for compression:" << filePath;
        return false;
    }

    const QByteArray compressed = qCompress(source.readAll(), 6);
    source.close();

    const QString archivePath = filePath + ".qz";
    QFile archive(archivePath);
    if (!archive.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Failed to create compressed archive:" << archivePath;
        return false;
    }

    if (archive.write(compressed) != compressed.size()) {
        qWarning() << "Failed to write compressed archive:" << archivePath;
        archive.close();
        return false;
    }
    archive.close();

    if (!QFile::remove(filePath)) {
        qWarning() << "Failed to remove rolled file after compression:" << filePath;
        return false;
    }

    return true;
}

// Removes expired rolled archives that exceed retention policy.
// 删除超过保留策略的过期滚动归档文件。
void LogWorker::cleanupOldArchives(const QString& filePath, int retentionDays) const
{
    if (retentionDays <= 0) {
        return;
    }

    const QFileInfo fileInfo(filePath);
    const QString prefix = fileInfo.completeBaseName() + "_";
    const QString suffix = fileInfo.suffix();
    const QDateTime deadline = QDateTime::currentDateTime().addDays(-retentionDays);

    const QFileInfoList files = fileInfo.dir().entryInfoList(QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo& candidate : files) {
        const QString name = candidate.fileName();
        if (!name.startsWith(prefix)) {
            continue;
        }

        const bool suffixMatches = suffix.isEmpty()
                                       ? true
                                       : (name.endsWith("." + suffix)
                                          || name.endsWith("." + suffix + ".qz"));
        if (!suffixMatches) {
            continue;
        }

        if (candidate.lastModified() < deadline) {
            QFile::remove(candidate.absoluteFilePath());
        }
    }
}

// Rotates current writer file, then reopens active log file for continued writes.
// 执行当前写入文件滚动，并重新打开活动日志文件继续写入。
bool LogWorker::rotateWriter(const QSharedPointer<Writer>& writer, const PendingLog& log)
{
    if (!writer || !writer->file.isOpen()) {
        return true;
    }

    writer->stream.flush();
    writer->file.flush();
    writer->file.close();

    const QFileInfo fileInfo(writer->baseFilePath);
    if (fileInfo.exists()) {
        const QString rolledPath = buildRolledFilePath(writer->baseFilePath);
        if (!QFile::rename(writer->baseFilePath, rolledPath)) {
            qWarning() << "Failed to rotate log file:" << writer->baseFilePath;
        } else {
            if (log.writerOptions.compressArchives) {
                compressAndRemoveFile(rolledPath);
            }
            cleanupOldArchives(writer->baseFilePath, log.writerOptions.retentionDays);
        }
    }

    return openWriterFile(writer);
}

// Maps log level to qDebug/qWarning/qCritical console output.
// 将日志等级映射到 qDebug/qWarning/qCritical 控制台输出。
void LogWorker::writeToConsole(const PendingLog& log) const
{
    switch (log.level) {
    case QwtLogger::Info:
        qDebug().noquote() << log.message;
        break;
    case QwtLogger::Warning:
        qWarning().noquote() << log.message;
        break;
    case QwtLogger::Error:
        qCritical().noquote() << log.message;
        break;
    default:
        qDebug().noquote() << log.message;
        break;
    }
}

// Writes a whole batch and flushes touched files once per batch.
// 写入整批日志，并按文件在批次末统一 flush。
void LogWorker::writeBatch(const QVector<PendingLog>& batch)
{
    QSet<QString> touchedFiles;

    for (const PendingLog& log : batch) {
        if (log.option == QwtLogger::FileOnly || log.option == QwtLogger::FileAndConsole) {
            const auto writer = ensureWriter(log.filePath);
            if (writer) {
                const bool readyForWrite = !shouldRotate(writer, log) || rotateWriter(writer, log);
                if (readyForWrite) {
                    writer->stream << log.message << "\n";
                    touchedFiles.insert(log.filePath);
                }
            }
        }

        if (log.option == QwtLogger::ConsoleOnly || log.option == QwtLogger::FileAndConsole) {
            writeToConsole(log);
        }
    }

    for (const QString& filePath : touchedFiles) {
        const auto writer = writers.value(filePath);
        if (writer && writer->file.isOpen()) {
            writer->stream.flush();
            writer->file.flush();
        }
    }
}

// Flushes queued logs; marshals call to worker thread when invoked externally.
// 刷出队列日志；若从外部线程调用则切换到工作线程执行。
void LogWorker::flush()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, &LogWorker::flush, Qt::BlockingQueuedConnection);
        return;
    }

    QVector<PendingLog> batch;
    {
        QMutexLocker locker(&queueMutex);
        flushScheduled = false;
        if (!pendingLogs.isEmpty()) {
            batch.swap(pendingLogs);
        }
    }

    if (!batch.isEmpty()) {
        writeBatch(batch);
    }
}
