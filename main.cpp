/************************************************************
 * File: main.cpp
 * Author: kelo
 * Created: 2026-08-30
 * Description:
 *   提供 QtLog 的最小可运行入口，创建控制台应用并向当前目录提交一条文件日志。
 *   程序在退出前同步刷空共享队列，可用于快速确认源码集成、线程启动与文件写入行为。
 ************************************************************/

#include <QtLog/QtLog.h>

#include <QCoreApplication>
#include <QDir>

/**
 * @brief 启动最小日志示例并等待记录完成文件写入。
 * @param argc 命令行参数数量，由运行环境提供。
 * @param argv 命令行参数数组，生命周期由运行环境管理。
 * @return 日志对象创建成功返回零，应用环境不完整时返回非零值。
 */
int main(int argc, char* argv[])
{
    // QCoreApplication 为后台线程、排队调用和退出清理提供必要的 Qt 运行环境。
    QCoreApplication application(argc, argv);

    // 示例日志固定写入启动目录，便于运行后直接检查生成结果。
    const QString logPath = QDir::current().absoluteFilePath(QStringLiteral("QtLog.log"));
    QtLog* const logger = QtLog::instance(logPath); ///< 进程注册表持有的日志门面，不由 main 释放。
    if (logger == nullptr) {
        // 应用环境或路径无效时无法提交日志，向调用环境返回失败状态。
        return 1;
    }

    // 提交示例记录后建立同步边界，确保 main 返回前数据已经完成处理。
    logger->write(QStringLiteral("QtLog is ready."));
    QtLog::flushAll();

    // 日志已经完成持久化，正常结束控制台程序。
    return 0;
}
