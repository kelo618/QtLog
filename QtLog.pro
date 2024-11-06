QT -= gui

TEMPLATE = lib
DEFINES += QTLOG_LIBRARY
CONFIG += c++17
TARGET = QtLog
# 设置头文件目录
HEADERS_DIR = include
# 指定 DLL 和静态库输出目录
DLL_DIR = bin
LIB_DIR = lib

# 设置生成的目标路径
CONFIG(debug, debug|release) {
    TARGET = $$TARGET"d"  # 为 Debug 版本添加后缀 'd'
}

# 将动态库和静态库分别输出到 bin 和 lib 文件夹
CONFIG(release, debug|release) {
    DESTDIR = $$DLL_DIR
} else:CONFIG(debug, debug|release) {
    DESTDIR = $$DLL_DIR
}

# 若是静态库，重定向到 lib 文件夹
CONFIG(staticlib) {
    DESTDIR = $$LIB_DIR
}

# 确保生成时创建 `include`、`bin` 和 `lib` 文件夹
QMAKE_EXTRA_TARGETS += create_directories
create_directories.target = dummy
create_directories.commands = \
    mkdir $$PWD/$$HEADERS_DIR & \
    mkdir $$PWD/$$DLL_DIR & \
    mkdir $$PWD/$$LIB_DIR

# 将头文件拷贝到 include 文件夹
for(header, HEADERS) {
    QMAKE_POST_LINK += $$quote(copy /Y "$$header" "$$PWD/$$HEADERS_DIR\\") $$escape_expand(\\n\\t)
}
# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    LogWorker.cpp \
    QtLog.cpp

HEADERS += \
    LogWorker.h \
    QtLog_global.h \
    QtLog.h

# Default rules for deployment.
unix {
    target.path = /usr/lib
}

!isEmpty(target.path): INSTALLS += target

DISTFILES += \
    README.md
