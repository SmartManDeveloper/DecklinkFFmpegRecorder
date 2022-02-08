QT -= gui
CONFIG += c++11 console
CONFIG -= app_bundle

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

INCLUDEPATH += \
	$${PWD}/deps/ffmpeg/include \
	$${PWD}/deps/x264/include 

LIBS += \
	-L$${PWD}/deps/ffmpeg/lib \
	-L$${PWD}/deps/x264/lib \
	-lavcodec \
	-lavdevice \
	-lavfilter \
	-lavformat \
	-lavutil \
	-lswresample \
	-lswscale \
	-lx264 \
	-ldl

HEADERS += \
	decklink/DeckLinkAPI.h \
	ffmpegutils.h \
	decklinkmanager.h \
	recorder.h

SOURCES += \
	decklink/DeckLinkAPIDispatch.cpp \
	decklinkmanager.cpp \
	main.cpp \
	recorder.cpp

# Default rules for deployment.
#qnx: target.path = /tmp/$${TARGET}/bin
#else: unix:!android: target.path = /opt/$${TARGET}/bin
#!isEmpty(target.path): INSTALLS += target

# suppres gcc 9 Qt annoying warning QVariant deprecated copy
QMAKE_CXXFLAGS += -Wno-deprecated-copy
