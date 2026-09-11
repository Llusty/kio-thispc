cmake_minimum_required(VERSION 3.16)

project(kio-thispc VERSION 0.19.0.3 LANGUAGES CXX)

set(QT_MIN_VERSION "6.5.0")
set(KF_MIN_VERSION "6.17.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Core Gui Widgets PrintSupport)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS KIO Solid)

# ---------------------------------------------------------------------------
# KIO backend: thispc:/
# ---------------------------------------------------------------------------
add_library(thispc MODULE)

target_sources(thispc PRIVATE
    src/thispc.cpp
    src/thispc.json
)

set_target_properties(thispc PROPERTIES
    OUTPUT_NAME "thispc"
)

target_link_libraries(thispc PRIVATE
    Qt6::Core
    KF6::KIOCore
    KF6::Solid
)

install(TARGETS thispc DESTINATION ${KDE_INSTALL_PLUGINDIR}/kf6/kio)

file(GLOB THISPC_ICONS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/icons/*.svg"
)

install(
    FILES ${THISPC_ICONS}
    DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/places
)

# ---------------------------------------------------------------------------
# Native "This PC" view. It reads drives from thispc:/ through KIO.
# ---------------------------------------------------------------------------
add_executable(thispc-view
    src/thispcview.cpp
)

target_link_libraries(thispc-view PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
    KF6::KIOCore
    KF6::KIOWidgets
)

install(TARGETS thispc-view DESTINATION ${KDE_INSTALL_BINDIR})

install(
    FILES data/org.kde.thispcview.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)
