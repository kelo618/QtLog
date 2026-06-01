# =====================================================================
# File: QtLogStress.pro
# Author: kelo
# Created: 2026-03-28
# Description:
#   qmake project file for QtLog stress benchmark.
#   QtLog 压力基准测试 qmake 工程文件。
# =====================================================================

QT += core
CONFIG += console c++17
TEMPLATE = app
TARGET = QtLogStress

INCLUDEPATH += ..
DEFINES += QTLOG_LIBRARY

SOURCES += \
    ../QtLog.cpp \
    ../LogWorker.cpp \
    QtLogStress_main.cpp

HEADERS += \
    ../QtLog.h \
    ../LogWorker.h \
    ../QtLog_global.h
