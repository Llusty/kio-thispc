cmake_minimum_required(VERSION 3.16)

project(kio-thispc VERSION 0.45.2 LANGUAGES CXX)

# 0.46.0 Stage 1B: Build-system separation.
# When ON, builds only the portable Qt6/Linux core library (`thispc-core`) and
# headless core smoke/gate tests without requiring ECM, KDE Frameworks (KF6),
# KIO, Solid, GIO, QtGui, QtWidgets, QtPdf, or QtWebEngine.
option(THISPC_BUILD_PORTABLE_CORE_ONLY
    "Build only the portable Qt6/Linux core library and headless core tests without KDE Frameworks or ECM" OFF)

set(QT_PORTABLE_CORE_MIN_VERSION "6.4.2")
set(QT_MIN_VERSION "6.5.0")
set(KF_MIN_VERSION "6.17.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(CTest)

if(THISPC_BUILD_PORTABLE_CORE_ONLY)
    find_package(Qt6 ${QT_PORTABLE_CORE_MIN_VERSION} CONFIG REQUIRED COMPONENTS Core Concurrent)
else()
    find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
    set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

    include(KDEInstallDirs)
    include(KDECMakeSettings)
    include(KDECompilerSettings NO_POLICY_SCOPE)

    find_package(LibArchive REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(TagLib REQUIRED)
    find_package(exiv2 REQUIRED CONFIG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBACL REQUIRED IMPORTED_TARGET libacl)

    find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS Core Concurrent Gui Widgets PrintSupport Pdf DBus)
    find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS KIO ItemViews Solid FileMetaData Config CoreAddons Notifications)
endif()

# ---------------------------------------------------------------------------
# Portable Qt6/Linux core library (0.46.0 Stage 1B)
# Strictly free from ECM, KF6, KIO, Solid, GIO, QtGui, QtWidgets, QtPdf,
# QtWebEngine, QtPrintSupport, QtDBus, and optional native media/ACL libs.
# Shared between the core-only build and the KDE compatibility build.
# ---------------------------------------------------------------------------
set(THISPC_PORTABLE_CORE_SOURCES
    src/applicationstyle.cpp
    src/applicationstyle.h
    src/portablecommon.h
    src/portablepaths.cpp
    src/portablepaths.h
    src/portabletransferjob.cpp
    src/portabletransferjob.h
    src/remoteurlhelper.cpp
    src/remoteurlhelper.h
    src/savedremotelocation.cpp
    src/savedremotelocation.h
    src/acldata.h
    src/aggregatepermissionsmodel.h
    src/aggregatepermissionswriter.h
    src/aggregatepropertiesformat.h
    src/aggregatepropertiesmodel.h
    src/aggregatepropertiesscan.h
    src/applicationlifecycle.h
    src/checksumdata.h
    src/checksumjob.h
    src/duplicateactiondata.h
    src/duplicatefinderdata.h
    src/duplicatefinderjob.h
    src/duplicategroupmodel.h
    src/localfileoverwrite.h
    src/localmergetransaction.h
    src/storageanalysisaccumulator.h
    src/storageanalysisdata.h
    src/storagescandata.h
    src/storagescanjob.h
    src/storagescanworker.h
    src/storagetreemapdata.h
    src/storagetreemaplayout.h
    src/transferbandwidthlimiter.h
)

add_library(thispc-core STATIC ${THISPC_PORTABLE_CORE_SOURCES})

# AUD-1B-06: Ensure thispc-core is PIC-ready for future optional shared plugins
# (e.g. KDE/GIO backend modules) and enforces baseline warnings + Qt discipline
# in portable-core-only mode without mutating unrelated global flags.
set_target_properties(thispc-core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_compile_features(thispc-core PUBLIC cxx_std_20)
target_include_directories(thispc-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(thispc-core PUBLIC
    Qt6::Core
    Qt6::Concurrent
)

if(THISPC_BUILD_PORTABLE_CORE_ONLY)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(thispc-core PRIVATE
            -Wall
            -Wextra
            -Wformat-security
            -Werror=return-type
            -Wnon-virtual-dtor
            -Woverloaded-virtual
        )
    endif()
    target_compile_definitions(thispc-core PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_FROM_BYTEARRAY
        QT_NO_CAST_TO_ASCII
        QT_NO_FOREACH
        QT_NO_KEYWORDS
        QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
        QT_NO_URL_CAST_FROM_STRING
    )
endif()

# AUD-1B-07: Generate a target-property manifest from real CMake target state
# for the dependency gate (not a regex over CMakeLists.txt).
set(THISPC_CORE_MANIFEST_FILE "${CMAKE_CURRENT_BINARY_DIR}/thispc-core-manifest.txt")
file(GENERATE
    OUTPUT "${THISPC_CORE_MANIFEST_FILE}"
    CONTENT
"TARGET=thispc-core
CORE_ONLY=$<BOOL:${THISPC_BUILD_PORTABLE_CORE_ONLY}>
POSITION_INDEPENDENT_CODE=$<TARGET_PROPERTY:thispc-core,POSITION_INDEPENDENT_CODE>
SOURCES=$<TARGET_PROPERTY:thispc-core,SOURCES>
LINK_LIBRARIES=$<TARGET_PROPERTY:thispc-core,LINK_LIBRARIES>
INTERFACE_LINK_LIBRARIES=$<TARGET_PROPERTY:thispc-core,INTERFACE_LINK_LIBRARIES>
"
)

if(BUILD_TESTING)
    add_executable(thispc-core-smoke
        tests/portable-core-smoke.cpp
        tests/portable-core-headers-compile.cpp
    )
    target_link_libraries(thispc-core-smoke PRIVATE thispc-core)
    add_test(NAME thispc-core-smoke COMMAND thispc-core-smoke)

    add_executable(thispc-portable-transfer-job-test
        tests/portable-transfer-job-test.cpp
    )
    target_link_libraries(thispc-portable-transfer-job-test PRIVATE thispc-core)
    add_test(NAME thispc-portable-transfer-job-test COMMAND thispc-portable-transfer-job-test)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        set(_thispc_gate_args
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --manifest "${THISPC_CORE_MANIFEST_FILE}"
            --build-dir "${CMAKE_BINARY_DIR}"
            --smoke-binary "$<TARGET_FILE:thispc-core-smoke>"
            --core-archive "$<TARGET_FILE:thispc-core>"
        )
        if(NOT THISPC_BUILD_PORTABLE_CORE_ONLY)
            list(APPEND _thispc_gate_args --allow-kde-cache)
        endif()
        add_test(
            NAME thispc-core-deps-gate
            COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/check-portable-core-deps.py"
                ${_thispc_gate_args}
        )
    endif()
endif()

if(THISPC_BUILD_PORTABLE_CORE_ONLY)
    message(STATUS "ThisPC 0.46.0: THISPC_BUILD_PORTABLE_CORE_ONLY=ON — skipping ECM/KF6/KIO targets.")
    return()
endif()

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
# Native "This PC" view (KDE compatibility build). Links `thispc-core`.
# ---------------------------------------------------------------------------
set(THISPC_VIEW_KDE_SOURCES
    src/actionstatecontroller.cpp
    src/actionstatecontroller.h
    src/appwidgets.cpp
    src/appwidgets.h
    src/directorylistingcore.cpp
    src/directorylistingcore.h
    src/directorywatchcontroller.cpp
    src/directorywatchcontroller.h
    src/drivehomecoordinator.cpp
    src/drivehomecoordinator.h
    src/soliddevicemonitor.cpp
    src/soliddevicemonitor.h
    src/devicemountcontroller.cpp
    src/devicemountcontroller.h
    src/deviceremovalcontroller.cpp
    src/deviceremovalcontroller.h
    src/locationpresentation.cpp
    src/locationpresentation.h
    src/launchurlresolver.h
    src/keyboardnavigation.cpp
    src/keyboardnavigation.h
    src/navigationhistory.cpp
    src/navigationhistory.h
    src/paneadapter.cpp
    src/paneadapter.h
    src/panemenucontroller.cpp
    src/panemenucontroller.h
    src/primarybrowserpane.cpp
    src/primarybrowserpane.h
    src/remotereconnectbar.h
    src/savedremotelocationdialog.cpp
    src/savedremotelocationdialog.h
    src/previewcoordinator.cpp
    src/previewcoordinator.h
    src/previewcontroller.cpp
    src/previewcontroller.h
    src/directorypreviewadapter.cpp
    src/directorypreviewadapter.h
    src/panecontext.h
    src/selectionmenucontroller.cpp
    src/selectionmenucontroller.h
    src/tabcontroller.cpp
    src/tabcontroller.h
    src/thispcview.cpp
    src/adminretrycontract.h
    src/adminretrycontroller.h
    src/adminretryexecutor.h
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
    src/localdirectorymovejob.h
    src/localbtrfsmovecommon.h
    src/localbatchmovejob.h
    src/localtransferplan.h
    src/localmergeplan.h
    src/localmergeexecutionjob.h
    src/localtransferjob.h
    src/kiostagedtransferjob.h
    src/kjobtransferadapter.cpp
    src/kjobtransferadapter.h
    src/localtreehistory.h
    src/transfertaskdescriptor.h
    src/transferreservationregistry.h
    src/transferqueuecontroller.h
    src/transferqueuesettings.h
    src/transferqueueui.h
    src/transfernotificationmanager.h
    src/transferhistorymanager.h
    src/operationmanager.h
    src/propertieshidden.h
    src/propertiesposixmode.h
    src/propertiesdata.h
    src/propertiesxattrwriter.h
    src/propertiesxattrs.h
    src/propertiesxattrwidget.h
    src/propertiescapabilities.h
    src/propertiesdataprovider.h
    src/drivepropertiesdata.h
    src/drivepropertiesprovider.h
    src/aggregatepropertiesdialog.h
    src/aggregatepermissionswidget.h
    src/propertiesshell.h
    src/propertiesdialog.h
    src/aclprovider.h
    src/aclcontroller.h
    src/acleditorwidget.h
    src/checksumwidget.h
    src/hashutilitiesdialog.h
    src/storagescandialog.h
    src/storageanalysismodel.h
    src/duplicateactioncontroller.h
    src/storagetreemapwidget.h
    src/metadatadata.h
    src/metadataprovider.h
    src/metadatawidget.h
    src/previewpane.h
    src/quicklook.h
    src/sidebar.h
    src/searchquery.h
    src/searchmatcher.h
    src/searchplan.h
    src/searchcontroller.h
    src/searchuicontroller.cpp
    src/searchuicontroller.h
    src/searchpopover.cpp
    src/searchpopover.h
    src/searchprogressbar.h
    src/advancedsearchfiltersdialog.cpp
    src/advancedsearchfiltersdialog.h
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

add_executable(thispc-view ${THISPC_VIEW_KDE_SOURCES})

target_compile_definitions(thispc-view PRIVATE
    THISPC_VERSION="${PROJECT_VERSION}"
)

# 0.44 Stage 3: the transfer queue UI is not yet connected to production
# dispatch. It is compiled into an isolated demo build only, and even then it
# additionally requires THISPC_TRANSFER_QUEUE_DEMO=1 at runtime.
option(THISPC_ENABLE_TRANSFER_QUEUE_DEMO
    "Build the isolated, non-production transfer queue demo UI" OFF)
if(THISPC_ENABLE_TRANSFER_QUEUE_DEMO)
    target_compile_definitions(thispc-view PRIVATE
        THISPC_ENABLE_TRANSFER_QUEUE_DEMO=1
    )
endif()

# Black-box startup/recovery integration binary. It is built from the exact
# application sources but is neither installed nor enabled in production.
add_executable(thispc-view-stage3c1-test EXCLUDE_FROM_ALL ${THISPC_VIEW_KDE_SOURCES})
target_compile_definitions(thispc-view-stage3c1-test PRIVATE
    THISPC_BATCH_RENAME_TEST_HOOKS=1
    THISPC_VERSION="${PROJECT_VERSION}"
)

# Exiv2 reports malformed metadata through C++ exceptions. Keep exception
# support scoped to the GUI target that reads EXIF in the worker task.
kde_target_enable_exceptions(thispc-view PRIVATE)

target_include_directories(thispc-view PRIVATE ${LibArchive_INCLUDE_DIRS})

target_link_libraries(thispc-view PRIVATE
    thispc-core
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
    Qt6::DBus
    KF6::KIOCore
    KF6::KIOGui
    KF6::KIOWidgets
    KF6::ItemViews
    KF6::Solid
    KF6::FileMetaData
    KF6::ConfigCore
    KF6::CoreAddons
    KF6::Notifications
    PkgConfig::LIBACL
)

kde_target_enable_exceptions(thispc-view-stage3c1-test PRIVATE)
target_include_directories(thispc-view-stage3c1-test PRIVATE ${LibArchive_INCLUDE_DIRS})
target_link_libraries(thispc-view-stage3c1-test PRIVATE
    thispc-core
    ${LibArchive_LIBRARIES} ZLIB::ZLIB TagLib::TagLib Exiv2::exiv2lib
    Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::PrintSupport Qt6::Pdf Qt6::DBus
    KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::ItemViews KF6::Solid KF6::FileMetaData KF6::ConfigCore KF6::CoreAddons KF6::Notifications PkgConfig::LIBACL)

# Stage 3A isolated interactive GUI preview binary (never installed)
add_executable(thispc-merge-preview
    tests/local-merge-dialogs-preview.cpp
    src/localmergedialogs.h
    src/localmergeplan.h
    src/browsercommon.h
)
target_include_directories(thispc-merge-preview PRIVATE src)
target_link_libraries(thispc-merge-preview PRIVATE thispc-core Qt6::Core Qt6::Gui Qt6::Widgets KF6::KIOCore)

# Stage 2A.2-1 isolated KDE compatibility adapter tests (KDE-compatible builds only)
if(BUILD_TESTING)
    add_executable(thispc-kjob-transfer-adapter-test
        tests/kjob-transfer-adapter-test.cpp
        src/kjobtransferadapter.cpp
        src/kjobtransferadapter.h
        src/localfilecopyjob.h
    )
    target_include_directories(thispc-kjob-transfer-adapter-test PRIVATE src)
    target_link_libraries(thispc-kjob-transfer-adapter-test PRIVATE
        thispc-core
        Qt6::Core
        KF6::CoreAddons
        KF6::KIOCore
    )
    add_test(NAME thispc-kjob-transfer-adapter-test COMMAND thispc-kjob-transfer-adapter-test)
endif()

# Stage 3B.3B-2 isolated developer GUI binary for manual Folder Merge acceptance (never installed)
add_executable(thispc-view-merge-dev EXCLUDE_FROM_ALL ${THISPC_VIEW_KDE_SOURCES})
target_compile_definitions(thispc-view-merge-dev PRIVATE
    THISPC_ENABLE_FOLDER_MERGE_DEV_BUILD=1
    THISPC_VERSION="${PROJECT_VERSION}-merge-dev"
)
kde_target_enable_exceptions(thispc-view-merge-dev PRIVATE)
target_include_directories(thispc-view-merge-dev PRIVATE ${LibArchive_INCLUDE_DIRS} src)
target_link_libraries(thispc-view-merge-dev PRIVATE
    thispc-core
    ${LibArchive_LIBRARIES} ZLIB::ZLIB TagLib::TagLib Exiv2::exiv2lib
    Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::PrintSupport Qt6::Pdf Qt6::DBus
    KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::ItemViews KF6::Solid KF6::FileMetaData KF6::ConfigCore KF6::CoreAddons KF6::Notifications PkgConfig::LIBACL)

install(TARGETS thispc-view DESTINATION ${KDE_INSTALL_BINDIR})

install(
    FILES data/org.kde.thispcview.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)

install(
    FILES data/org.kde.thispcview.notifyrc
    DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR}
)
