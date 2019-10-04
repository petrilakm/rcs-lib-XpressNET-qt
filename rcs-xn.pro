TARGET = rcs-xn
TEMPLATE = lib
DEFINES += QT_DEPRECATED_WARNINGS
DEFINES += RCS_XN_SHARED_LIBRARY

SOURCES += \
	src/rcs-xn.cpp \
	src/settings.cpp \
	src/signals.cpp \
	src/form-signal-edit.cpp
HEADERS += \
	src/rcs-xn.h \
	src/errors.h \
	src/events.h \
	src/settings.h \
	src/util.h \
	src/signals.h \
	src/form-signal-edit.h

FORMS += \
	form/main-window.ui \
	form/signal-edit.ui

SOURCES += \
	lib/xn-lib-cpp-qt/xn.cpp
HEADERS += \
	lib/xn-lib-cpp-qt/q-str-exception.h \
	lib/xn-lib-cpp-qt/xn.h \
	lib/xn-lib-cpp-qt/xn-commands.h \
	lib/xn-lib-cpp-qt/xn-loco-addr.h \
	lib/q-str-exception.h

CONFIG += c++14 dll
QMAKE_CXXFLAGS += -Wall -Wextra -pedantic

win32 {
	QMAKE_LFLAGS += -Wl,--kill-at
	QMAKE_CXXFLAGS += -enable-stdcall-fixup
}
win64 {
	QMAKE_LFLAGS += -Wl,--kill-at
	QMAKE_CXXFLAGS += -enable-stdcall-fixup
}

QT += core gui serialport
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

VERSION_MAJOR = 0
VERSION_MINOR = 5

DEFINES += "VERSION_MAJOR=$$VERSION_MAJOR" \
	"VERSION_MINOR=$$VERSION_MINOR"

#Target version
VERSION = "$${VERSION_MAJOR}.$${VERSION_MINOR}"
DEFINES += "VERSION=\\\"$${VERSION}\\\""
