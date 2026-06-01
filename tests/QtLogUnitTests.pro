# =====================================================================
# File: QtLogUnitTests.pro
# Author: kelo
# Created: 2026-03-28
# Description:
#   qmake project file for QtLog unit tests.
#   QtLog 单元测试 qmake 工程文件。
# =====================================================================

QT += core testlib
CONFIG += console testcase c++17
TEMPLATE = app
TARGET = QtLogUnitTests

INCLUDEPATH += ..
DEFINES += QTLOG_LIBRARY

SOURCES += \
    ../QtLog.cpp \
    ../LogWorker.cpp \
    QtLogUnitTests_main.cpp

HEADERS += \
    ../QtLog.h \
    ../LogWorker.h \
    ../QtLog_global.h
