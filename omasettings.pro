QT       += core gui widgets
CONFIG   += c++17 release
TARGET    = omasettings
TEMPLATE  = app
SOURCES  += main.cpp
CONFIG   += link_pkgconfig
PKGCONFIG += fontconfig
