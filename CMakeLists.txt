cmake_minimum_required(VERSION 3.16)

project(kio-thispc VERSION 0.31.0 LANGUAGES CXX)

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

find_package(LibArchive REQUIRED)
find_package(ZLIB REQUIRED)
find_package(TagLib REQUIRED)
find_package(exiv2 REQUIRED CONFIG)

find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Core Concurrent Gui Widgets PrintSupport Pdf)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS KIO ItemViews Solid)

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
    src/applicationstyle.cpp
    src/applicationstyle.h
    src/appwidgets.cpp
    src/appwidgets.h
    src/thispcview.cpp
    src/archive-creation.h
    src/archive-detection.h
    src/archive-extraction.h
    src/browsercommon.h
    src/batchrename.h
    src/batchrenamejournal.h
    src/pathwidgets.h
    src/directoryview.h
    src/directoryviewsettings.h
    src/fileactions.h
    src/templatemenu.h
    src/localfilecopyjob.h
    src/localfilemovejob.h
    src/localfileoverwrite.h
    src/localtransferplan.h
    src/localtransferjob.h
    src/localtreehistory.h
    src/operationmanager.h
    src/propertiesdialog.h
    src/previewpane.h
    src/quicklook.h
    src/sidebar.h
    src/searchcontroller.h
    src/sessionmanager.h
    src/undocontroller.h
    src/splitbrowserpane.h
    src/splitcomparemodel.h
    src/splitsyncmodel.h
    src/splitcomparedialog.h
    src/splitsyncpreviewdialog.h
    src/splitsyncexecutor.h
    src/splitsyncexecutiondialog.h
)

# Black-box startup/recovery integration binary. It is built from the exact
# application sources but is neither installed nor enabled in production.
add_executable(thispc-view-stage3c1-test EXCLUDE_FROM_ALL
    src/applicationstyle.cpp
    src/applicationstyle.h
    src/appwidgets.cpp
    src/appwidgets.h
    src/thispcview.cpp
    src/archive-creation.h
    src/archive-detection.h
    src/archive-extraction.h
    src/browsercommon.h
    src/batchrename.h
    src/batchrenamejournal.h
    src/pathwidgets.h
    src/directoryview.h
    src/directoryviewsettings.h
    src/fileactions.h
    src/templatemenu.h
    src/localfilecopyjob.h
    src/localfilemovejob.h
    src/localfileoverwrite.h
    src/localtransferplan.h
    src/localtransferjob.h
    src/localtreehistory.h
    src/operationmanager.h
    src/propertiesdialog.h
    src/previewpane.h
    src/quicklook.h
    src/sidebar.h
    src/searchcontroller.h
    src/sessionmanager.h
    src/undocontroller.h
    src/splitbrowserpane.h
    src/splitcomparemodel.h
    src/splitsyncmodel.h
    src/splitcomparedialog.h
    src/splitsyncpreviewdialog.h
    src/splitsyncexecutor.h
    src/splitsyncexecutiondialog.h
)
target_compile_definitions(thispc-view-stage3c1-test PRIVATE THISPC_BATCH_RENAME_TEST_HOOKS=1)

# Exiv2 reports malformed metadata through C++ exceptions. Keep exception
# support scoped to the GUI target that reads EXIF in the worker task.
kde_target_enable_exceptions(thispc-view PRIVATE)

target_include_directories(thispc-view PRIVATE ${LibArchive_INCLUDE_DIRS})

target_link_libraries(thispc-view PRIVATE
    ${LibArchive_LIBRARIES}
    ZLIB::ZLIB
    TagLib::TagLib
    Exiv2::exiv2lib
    Qt6::Core
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::Pdf
    KF6::KIOCore
    KF6::KIOWidgets
    KF6::ItemViews
)

kde_target_enable_exceptions(thispc-view-stage3c1-test PRIVATE)
target_include_directories(thispc-view-stage3c1-test PRIVATE ${LibArchive_INCLUDE_DIRS})
target_link_libraries(thispc-view-stage3c1-test PRIVATE
    ${LibArchive_LIBRARIES} ZLIB::ZLIB TagLib::TagLib Exiv2::exiv2lib
    Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::PrintSupport Qt6::Pdf
    KF6::KIOCore KF6::KIOWidgets KF6::ItemViews)

install(TARGETS thispc-view DESTINATION ${KDE_INSTALL_BINDIR})

install(
    FILES data/org.kde.thispcview.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)
