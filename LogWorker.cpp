#include "LogWorker.h"
///
/// \brief 默认构造函数
/// \param 输出文件绝对路径
/// \param QObject父类
///
LogWorker::LogWorker(const QString &filePath, QObject *parent)
    : QObject(parent), logFile(filePath) {
    logFile.open(QIODevice::Append | QIODevice::Text);
    logStream.setDevice(&logFile);
}
///
/// \brief 析构函数
///
LogWorker::~LogWorker() {
    // 关闭日志文件
    if (logFile.isOpen()) logFile.close();
}
///
/// \brief LogWorker::processLog
/// \param message
/// \param level
/// \param outputOption
///
void LogWorker::processLog(const QString &message, QwtLogger::LogLevel level, QwtLogger::OutputOption outputOption) {
    Q_UNUSED(level);
    if (outputOption == QwtLogger::FileOnly
        || outputOption == QwtLogger::FileAndConsole) {
        if (logFile.isOpen()) {
            logStream << message << "\n";
            logStream.flush();
        }
    }

    if (outputOption == QwtLogger::ConsoleOnly
        || outputOption == QwtLogger::FileAndConsole)
        qDebug() << message;
}
