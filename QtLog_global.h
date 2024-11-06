#ifndef QTLOG_GLOBAL_H
#define QTLOG_GLOBAL_H

#include <QtCore/qglobal.h>

#if defined(QTLOG_LIBRARY)
#define QTLOG_EXPORT Q_DECL_EXPORT
#else
#define QTLOG_EXPORT Q_DECL_IMPORT
#endif

#endif // QTLOG_GLOBAL_H
