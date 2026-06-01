/*
 * =====================================================================
 * File: QtLog_global.h
 * Author: kelo
 * Created: 2026-03-28
 * Description:
 *   QtLog public global definitions.
 *   QtLog 对外全局定义。
 * =====================================================================
 */

#ifndef QTLOG_GLOBAL_H
#define QTLOG_GLOBAL_H

#include <QtCore/qglobal.h>

#if defined(QTLOG_LIBRARY)
#define QTLOG_EXPORT Q_DECL_EXPORT
#else
#define QTLOG_EXPORT Q_DECL_IMPORT
#endif

namespace QwtLogger
{
enum LogLevel : int {
    Info = 0,    ///< Informational log level. / 信息级日志。
    Warning = 1, ///< Warning log level. / 警告级日志。
    Error = 2    ///< Error log level. / 错误级日志。
};

enum OutputOption
{
    FileOnly,      ///< Write to file only. / 仅写文件。
    ConsoleOnly,   ///< Write to console only. / 仅写控制台。
    FileAndConsole ///< Write to both file and console. / 同时写文件和控制台。
};

struct WriterOptions
{
    qint64 maxFileSizeBytes = 0;   ///< Rotate by size when > 0. / 大于 0 时按文件大小滚动。
    int retentionDays = 0;         ///< Keep rolled logs for N days when > 0. / 大于 0 时按天保留。
    bool dailyRotationEnabled = false; ///< Rotate when date changes. / 日期变化时滚动。
    bool compressArchives = false; ///< Compress rolled archives when true. / true 时压缩归档。
};
} // namespace QwtLogger

#endif // QTLOG_GLOBAL_H
