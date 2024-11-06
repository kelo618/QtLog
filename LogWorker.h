#ifndef LOGWORKER_H
#define LOGWORKER_H

#include <QtLog.h>

class LogWorker : public QObject
{
    Q_OBJECT
public:
    explicit LogWorker(const QString &filePath, QObject *parent = nullptr);

    ~LogWorker();
public slots:
    void processLog(const QString &message, QwtLogger::LogLevel level, QwtLogger::OutputOption outputOption);

private:
    QFile logFile;
    QTextStream logStream;
};

#endif // LOGWORKER_H
