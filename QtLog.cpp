#include "QtLog.h"
#include "LogWorker.h"

QMutex QtLog::s_instanceMutex; // 用于实例管理的互斥锁
QHash<QString, QSharedPointer<QtLog>> QtLog::s_instances;

///
/// \brief 线程安全的单例模式 内部有一张哈希表维护日志文件
///         如果有新的日志文件，就创建一个专属于它的新的
///         智能指针QtLog对象。
/// \param 文件绝对路径
/// \return QtLog共享指针
///
QtLog *QtLog::Instance(const QString &filePath)
{
     // 保证实例创建的线程安全
    QMutexLocker locker(&s_instanceMutex);
     // 如果文件未出现过就创建新的日志实例
    if (!s_instances.contains(filePath))
        s_instances[filePath] = QSharedPointer<QtLog>(new QtLog(filePath));
    // 返回对应的日志实例
    return s_instances[filePath].data();
}
///
/// \brief 根据选择的日志等级和要输出的目的地来写入
/// \param 文本信息
/// \param 日志等级(信息/警告/错误)
/// \param 输出目的地(文件/控制台/both)
///
void QtLog::writeLog(const QString &message, QwtLogger::LogLevel level, QwtLogger::OutputOption outputOption)
{
    QString logMsg = QString("%1 {%2} : %3")
                             .arg(logLevelToString(level)
                                  ,getCurrentTimestamp()
                                  ,message);
    emit logMessage(logMsg, level, outputOption);
}
///
/// \brief 被隐藏的默认构造函数
/// \param 日志文件的绝对路径
///
QtLog::QtLog(const QString &filePath) : logFile(filePath)
{
    // 打开文件 在末尾追加
    logFile.open(QIODevice::Append | QIODevice::Text);
    // 文本流重定位到该输出文件
    logStream.setDevice(&logFile);
    // 创建新线程 并把日志输出移到子线程实现
    worker = new LogWorker(filePath);
    workerThread = new QThread(this);
    worker->moveToThread(workerThread);
    // 连接信号
    connect(this, &QtLog::logMessage, worker, &LogWorker::processLog);
    // 线程启动
    workerThread->start();
}
///
/// \brief 析构函数 释放文件对象
///
QtLog::~QtLog()
{
    // 关闭线程
    if (workerThread->isRunning()) {
        workerThread->quit();
        workerThread->wait();
    }
    // 关闭文件
    if (logFile.isOpen()) logFile.close();
}
///
/// \brief 根据所选的等级返回对应的开头字符串
/// \param 日志等级
/// \return 等级字符串
///
QString QtLog::logLevelToString(QwtLogger::LogLevel level)
{
    switch (level) {
    case QwtLogger::Info: return "[INFO]";
    case QwtLogger::Warning: return "[WARNING]";
    case QwtLogger::Error: return "[ERROR]";
    default: return "[UNKNOWN]";
    }
}
///
/// \brief 获取当前时间戳 精确到秒
/// \return 时间字符串
///
QString QtLog::getCurrentTimestamp()
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}
