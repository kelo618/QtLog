#ifndef QTLOG_H
#define QTLOG_H

#include "QtLog_global.h"
#include <QObject>
#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QHash>
#include <QSharedPointer>
#include <QDateTime>
#include <QThread>

/*-----枚举类型的命名空间-----*/
namespace QwtLogger {
// 日志等级
enum LogLevel {
    Info,           // 信息
    Warning,        // 警告
    Error           // 错误
};
// 输出选项
enum OutputOption {
    FileOnly,       // 仅文件
    ConsoleOnly,    // 仅控制台
    FileAndConsole  // 同时
};
}

// 前置声明
class LogWorker;

/*-----类定义-----*/
class QTLOG_EXPORT QtLog : public QObject
{
    Q_OBJECT

public:
    virtual ~QtLog();

    static QtLog* Instance(const QString &filePath);            // 获取特定文件路径的 Logger 实例

    void writeLog(const QString &message,
                  QwtLogger::LogLevel level = QwtLogger::Info,
                  QwtLogger::OutputOption outputOption = QwtLogger::FileOnly);                      // 写入日志内容
private:
    explicit QtLog(const QString &filePath);

private:
    QString logLevelToString(QwtLogger::LogLevel level);        // 将日志等级转换为字符串

    QString getCurrentTimestamp();  // 获取当前时间戳

Q_SIGNALS:
signals:
    void logMessage(const QString &message, QwtLogger::LogLevel level, QwtLogger::OutputOption outputOption);

private:
    QFile logFile;                                              // 日志文件对象
    QTextStream logStream;                                      // 写入文本流
    static QMutex s_instanceMutex;                              // 管理实例创建的全局互斥锁
    static QHash<QString, QSharedPointer<QtLog>> s_instances;   // 文件路径到 Logger 实例的映射
    LogWorker *worker;
    QThread *workerThread;
};

#endif // QTLOG_H
