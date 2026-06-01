# =====================================================================
# File: QtLog.pro
# Author: kelo
# Created: 2026-03-28
# Description:
#   qmake project file for QtLog library.
#   QtLog 库的 qmake 工程文件。
# =====================================================================

QT -= gui

TEMPLATE = lib
CONFIG += c++17 dll
DEFINES += QTLOG_LIBRARY
TARGET = QtLog

HEADERS_DIR = include
BIN_DIR = bin

CONFIG(debug, debug|release) {
    TARGET = $$TARGET"d"
}

win32 {
    # dll 和 import lib 都输出到源码目录下的 bin
    DESTDIR = $$PWD/$$BIN_DIR
    DLLDESTDIR = $$PWD/$$BIN_DIR
}

PUBLIC_HEADERS += \
    QtLog_global.h \
    QtLog.h

SOURCES += \
    LogWorker.cpp \
    QtLog.cpp

HEADERS += \
    LogWorker.h \
    QtLog_global.h \
    QtLog.h

win32 {
    # 构建后确保 include 目录存在
    QMAKE_POST_LINK += if not exist \"$$PWD\\$$HEADERS_DIR\" mkdir \"$$PWD\\$$HEADERS_DIR\" $$escape_expand(\\n\\t)

    # 拷贝公开头文件到 include 目录
    for(header, PUBLIC_HEADERS) {
        QMAKE_POST_LINK += copy /Y \"$$PWD\\$$header\" \"$$PWD\\$$HEADERS_DIR\\\" $$escape_expand(\\n\\t)
    }
}

unix {
    target.path = /usr/lib
}

!isEmpty(target.path): INSTALLS += target
