#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Harasim
# SPDX-License-Identifier: MIT

"""Build a temporary instrumented window and exercise real Qt actions offscreen.

Builds a temporary CMake project with the extracted headers and fresh Qt MOC
output. Only test copies expose private members. Pane/drag tests intercept file
jobs before KIO dispatch; PropertiesDialog tests use real disposable files.
"""
import os
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--keep-build', action='store_true',
                    help='preserve the generated pane-test tree for post-mortem debugging')
group = parser.add_mutually_exclusive_group()
group.add_argument('--tabs', action='store_true', help='run tab drag/drop tests instead of pane actions')
group.add_argument('--sidebar-dnd', action='store_true', help='run sidebar drag/drop tests')
group.add_argument('--sidebar-layout', action='store_true', help='run sidebar scroll/resize tests')
group.add_argument('--properties', action='store_true', help='run real PropertiesDialog tests')
group.add_argument('--search', action='store_true', help='run real KIO search and cancellation tests')
group.add_argument('--actions', action='store_true', help='run real FileActions and Undo tests')
group.add_argument('--action-state', action='store_true', help='run pure ActionStateController tests')
group.add_argument('--templates', action='store_true', help='run native XDG template menu tests')
group.add_argument('--trash', action='store_true', help='run Empty Trash menu and simulated operation tests')
group.add_argument('--operations', action='store_true', help='run OperationManager state tests')
group.add_argument('--local-transfer', action='store_true', help='run native local copy pause/resume tests')
group.add_argument('--transfer-plan', action='store_true', help='run native directory transfer planning tests')
group.add_argument('--local-move', action='store_true', help='run safe native move and history tests')
group.add_argument('--all', action='store_true', help='run every regression suite')
group.add_argument('--suites', nargs='+', choices=[
    'panes', 'tabs', 'properties', 'search', 'actions', 'action_state', 'operations',
    'local_transfer', 'transfer_plan', 'local_move', 'local_tree', 'tree_history',
    'sidebar_dnd', 'sidebar_layout', 'split_layout', 'templates', 'trash', 'archive', 'archive_jobs', 'archive_menu', 'archive_creation', 'preview', 'quick_look', 'batch_rename', 'view_settings', 'listing_core', 'drive_home', 'solid_monitor', 'device_mount', 'device_removal', 'split_compare', 'selection_menu', 'location_presentation', 'navigation_history', 'keyboard_navigation', 'remote_url', 'saved_remote', 'recent_reconnect', 'session_history', 'properties_integration', 'properties_hidden', 'properties_posix_mode', 'properties_xattr_edit', 'properties_xattrs', 'properties_capabilities', 'properties_data', 'drive_properties', 'acl', 'acl_editor', 'checksums', 'metadata', 'properties_lifecycle', 'storage_scan', 'storage_analysis', 'hash_utilities', 'duplicate_finder', 'duplicate_actions', 'storage_treemap', 'launch_url_resolver', 'preview_controller', 'image_video_previews', 'executable_previews', 'folder_previews', 'preview_settings'],
    help='build once and run only the selected regression suites')
options = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'src/thispcview.cpp').read_text()

file_actions = (root / 'src/fileactions.h').read_text()

def require_once(text, needle, purpose):
    count = text.count(needle)
    if count != 1:
        raise RuntimeError(
            f'test harness seam for {purpose} must occur exactly once; found {count}: {needle!r}')
    return needle

def replace_once(text, old, new, purpose):
    require_once(text, old, purpose)
    return text.replace(old, new, 1)

def intercept(text, signature, marker, statement, flag='interceptFileJobs'):
    require_once(text, signature, f'{signature.strip()} method')
    start = text.index(signature)
    end = text.index('\n    }', start)
    method = text[start:end]
    require_once(method, marker, f'{signature.strip()} injection point')
    point = text.index(marker, start, end)
    return text[:point] + '        if (' + flag + ') { ' + statement + ' return; }\n' + text[point:]

# Never execute the real global EmptyTrash job in this runner: an isolated
# XDG_DATA_HOME alone does not isolate trash directories on other mounts.
file_actions = replace_once(file_actions, 'KIO::emptyTrash()', 'makeTestEmptyTrashJob()',
                            'isolated Empty Trash job')

for signature, marker, statement in [
    ('    void trashSelected(', '        KIO::CopyJob *job =', 'dispatch = {"trash", urls, {}};'),
    ('    void renameTo(', '        KIO::CopyJob *job =', 'dispatch = {"rename", {source}, destination};'),
    ('    void pasteClipboardInto(', '        if (startNativeSingleFileTransfer(', 'dispatch = {cut ? "move" : "copy", urls, destination};'),
    ('    void createNewFolderWithName(', '        KIO::MkdirJob *job =', 'dispatch = {"mkdir", {}, destination};'),
    ('    void createNewFileWithName(', '        KIO::StoredTransferJob *job =', 'dispatch = {"create", {}, destination};'),
    ('    void createFromTemplateWithName(', '        KIO::CopyJob *job =', 'dispatch = {"template", {source}, destination};'),
    ('    void transfer(', '        if (startNativeSingleFileTransfer(', 'dispatch = {action == Qt::MoveAction ? "move" : "copy", urls, destination};'),
]:
    file_actions = intercept(file_actions, signature, marker, statement)
source = intercept(source, '    void showPropertiesDialog(', '        PropertiesDialog::show(',
                   'dispatch = {"properties", {url}, {}};')
source = intercept(source, '    void extractArchiveWithArk(', '        auto *job = new ArchiveExtractionJob',
                   'dispatch = {showDialog ? "extract-to" : "extract-here", {archiveUrl}, destinationUrl}; m_runningArchivePaths.remove(identity);',
                   'interceptArchiveJobs')
source = intercept(source, '    void refreshPane(', '        if (m_paneAdapter)',
                   'refreshedPanes.push_back(pane == PaneId::Split ? 1 : 0);',
                   'interceptPaneRefreshes')
source = intercept(source, '    void launchResolvedUrl(', '        auto *job =',
                   'dispatch = {"launch", {url}, {}};')

def expose_legacy_header(text):
    return text.replace('private:', 'public:').replace('protected:', 'public:').replace('    Q_OBJECT', '    Q_OBJECT\npublic:')

# These headers still have direct white-box coverage. Keep the debt explicit:
# do not silently expose every header copied into the synthetic translation unit.
legacy_exposed_headers = {
    'appwidgets.h', 'archive-creation.h', 'archive-extraction.h',
    'batchrename.h', 'batchrenamerecovery.h', 'directorypreviewadapter.h', 'directoryview.h',
    'fileactions.h', 'launchurlresolver.h', 'localfilecopyjob.h', 'localfilemovejob.h',
    'localtransferjob.h', 'localtransferplan.h', 'localtreehistory.h',
    'operationmanager.h', 'pathwidgets.h', 'previewcontroller.h', 'previewpane.h', 'quicklook.h',
    'searchcontroller.h', 'sidebar.h', 'splitbrowserpane.h',
    'splitcomparedialog.h', 'splitsyncexecutiondialog.h',
    'splitsyncpreviewdialog.h', 'templatemenu.h', 'undocontroller.h',
}
prelude = '''#include <QtTest>
#include <QPdfWriter>
#include <KIO/RenameDialog>
#include <KIO/OpenUrlJob>
#include "localfilecopyjob.h"
#include "localfilemovejob.h"
#include "localtransferplan.h"
#include "localtransferjob.h"
#include "localtreehistory.h"
#include "previewcontroller.h"
#include "directorypreviewadapter.h"
struct Dispatch { QString kind; QList<QUrl> sources; QUrl destination; };
static Dispatch dispatch;
static bool interceptFileJobs = true;
static bool interceptArchiveJobs = false;
static bool interceptPaneRefreshes = false;
static QList<int> refreshedPanes;
class TestEmptyTrashJob final : public KJob
{
public:
    TestEmptyTrashJob() { setCapabilities(KJob::Killable); }
    void start() override {}
    void complete(int error = 0) {
        setError(error);
        if (error) setErrorText(QStringLiteral("Simulated Trash error"));
        emitResult();
    }
protected:
    bool doKill() override { return true; }
};
static QPointer<TestEmptyTrashJob> testEmptyTrashJob;
static int emptyTrashDispatches = 0;
static TestEmptyTrashJob *makeTestEmptyTrashJob() {
    ++emptyTrashDispatches;
    testEmptyTrashJob = new TestEmptyTrashJob;
    return testEmptyTrashJob;
}
'''
(root / 'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='thispc-pane-tests-', delete=not options.keep_build) as tmp, tempfile.TemporaryDirectory(
        prefix='test-data-', dir=root / 'build', delete=not options.keep_build) as disk_tmp:
    tmp = Path(tmp)
    if options.keep_build:
        print(f'Persistent pane-test build: {tmp}', flush=True)
    (tmp / 'src').mkdir()
    for header in (root / 'src').glob('*.h'):
        contents = file_actions if header.name == 'fileactions.h' else header.read_text()
        if header.name in legacy_exposed_headers:
            contents = expose_legacy_header(contents)
        (tmp / 'src' / header.name).write_text(contents)
    for implementation in ('actionstatecontroller.cpp', 'appwidgets.cpp', 'applicationstyle.cpp', 'directorylistingcore.cpp', 'drivehomecoordinator.cpp', 'soliddevicemonitor.cpp', 'devicemountcontroller.cpp', 'deviceremovalcontroller.cpp', 'keyboardnavigation.cpp', 'locationpresentation.cpp', 'navigationhistory.cpp', 'paneadapter.cpp', 'panemenucontroller.cpp', 'primarybrowserpane.cpp', 'previewcoordinator.cpp', 'previewcontroller.cpp', 'directorypreviewadapter.cpp', 'tabcontroller.cpp', 'searchuicontroller.cpp', 'selectionmenucontroller.cpp', 'remoteurlhelper.cpp', 'savedremotelocation.cpp', 'savedremotelocationdialog.cpp'):
        (tmp / 'src' / implementation).write_text((root / 'src' / implementation).read_text())
    suites = {'trash': 'empty-trash.cpp', 'panes': 'pane-actions.cpp', 'templates': 'template-menu.cpp', 'tabs': 'tab-drag-drop.cpp', 'sidebar_dnd': 'sidebar-drag-drop.cpp', 'sidebar_layout': 'sidebar-layout.cpp', 'properties': 'properties-dialog.cpp', 'search': 'search-controller.cpp', 'actions': 'file-actions.cpp', 'action_state': 'action-state-controller.cpp', 'operations': 'operation-manager.cpp', 'local_transfer': 'local-file-copy-job.cpp', 'transfer_plan': 'local-transfer-plan.cpp', 'local_move': 'local-file-move-job.cpp', 'local_tree': 'local-transfer-job.cpp', 'tree_history': 'local-tree-history.cpp', 'archive': 'archive-detection.cpp', 'archive_jobs': 'archive-extraction.cpp', 'archive_menu': 'archive-menu.cpp', 'archive_creation': 'archive-creation.cpp'}
    suites['split_layout'] = 'split-layout.cpp'
    suites['preview'] = 'preview-pane.cpp'
    suites['quick_look'] = 'quick-look.cpp'
    suites['batch_rename'] = 'batch-rename.cpp'
    suites['view_settings'] = 'directory-view-settings.cpp'
    suites['listing_core'] = 'directory-listing-core.cpp'
    suites['drive_home'] = 'drive-home-coordinator.cpp'
    suites['solid_monitor'] = 'solid-device-monitor.cpp'
    suites['device_mount'] = 'device-mount-controller.cpp'
    suites['device_removal'] = 'device-removal-controller.cpp'
    suites['split_compare'] = 'split-compare.cpp'
    suites['selection_menu'] = 'selection-menu-controller.cpp'
    suites['location_presentation'] = 'location-presentation.cpp'
    suites['navigation_history'] = 'navigation-history.cpp'
    suites['keyboard_navigation'] = 'keyboard-navigation.cpp'
    suites['remote_url'] = 'remote-url-helper.cpp'
    suites['saved_remote'] = 'saved-remote-locations.cpp'
    suites['recent_reconnect'] = 'recent-reconnect.cpp'
    suites['session_history'] = 'session-history.cpp'
    suites['properties_data'] = 'properties-data-provider.cpp'
    suites['properties_integration'] = 'properties-integration.cpp'
    suites['properties_hidden'] = 'properties-hidden.cpp'
    suites['properties_posix_mode'] = 'properties-posix-mode.cpp'
    suites['properties_xattr_edit'] = 'properties-xattr-edit.cpp'
    suites['properties_xattrs'] = 'properties-xattrs.cpp'
    suites['properties_capabilities'] = 'properties-capabilities.cpp'
    suites['drive_properties'] = 'drive-properties.cpp'
    suites['acl'] = 'acl-controller.cpp'
    suites['acl_editor'] = 'acl-editor-widget.cpp'
    suites['checksums'] = 'checksum-job.cpp'
    suites['metadata'] = 'metadata-provider.cpp'
    suites['properties_lifecycle'] = 'properties-lifecycle.cpp'
    suites['storage_scan'] = 'storage-scan.cpp'
    suites['storage_analysis'] = 'storage-analysis.cpp'
    suites['hash_utilities'] = 'hash-utilities.cpp'
    suites['duplicate_finder'] = 'duplicate-finder.cpp'
    suites['duplicate_actions'] = 'duplicate-review.cpp'
    suites['storage_treemap'] = 'storage-treemap.cpp'
    suites['launch_url_resolver'] = 'launch-url-resolver.cpp'
    suites['preview_controller'] = 'preview-controller.cpp'
    suites['image_video_previews'] = 'image-video-previews.cpp'
    suites['executable_previews'] = 'executable-previews.cpp'
    suites['folder_previews'] = 'folder-previews.cpp'
    suites['preview_settings'] = 'preview-settings.cpp'
    selected = options.suites or (list(suites) if options.all else ['trash' if options.trash else 'templates' if options.templates else 'sidebar_layout' if options.sidebar_layout else 'sidebar_dnd' if options.sidebar_dnd else 'local_move' if options.local_move else 'transfer_plan' if options.transfer_plan else 'local_transfer' if options.local_transfer else 'operations' if options.operations else 'action_state' if options.action_state else 'actions' if options.actions else 'tabs' if options.tabs else 'properties' if options.properties else 'search' if options.search else 'panes'])
    if 'batch_rename' in selected:
        subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build'),
                        '-DCMAKE_BUILD_TYPE=Debug'], check=True)
        subprocess.run(['cmake', '--build', str(root / 'build'), '--target',
                        'thispc-view', 'thispc-view-stage3c1-test', '-j2'], check=True)
    combined = prelude + source
    for suite in selected:
        disk_data = Path(disk_tmp) / suite
        disk_data.mkdir()
        test = replace_once((root / 'tests' / suites[suite]).read_text(),
                            'int main(', 'int run(', f'{suite} suite entry point')
        # Unlike main(), an ordinary int function must return explicitly.
        end = test.rfind('}')
        test = test[:end] + '    return 0;\n' + test[end:]
        combined += '\nnamespace regression_' + suite + ' {\n' + test + '\n}\n'
    combined += '\nint main(int argc, char **argv) {\n'
    combined += '    if (qgetenv("THISPC_RECOVERY_STARTUP_SYNC") == "1") { auto &gate = BatchRenameRecoveryGate::instance(); if (gate.beginStartupScan()) gate.completeStartupScan(); }\n'
    for suite in selected:
        combined += f'    if (argc > 1 && QByteArray(argv[1]) == "{suite}") return regression_{suite}::run(argc, argv);\n'
    combined += '    return 2;\n}\n'
    (tmp / 'src/thispcview.cpp').write_text(combined)
    cmake = '''cmake_minimum_required(VERSION 3.16)
project(thispc-regressions LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Concurrent Gui Widgets PrintSupport Pdf DBus Test)
find_package(KF6KIO REQUIRED)
find_package(KF6ItemViews REQUIRED)
find_package(KF6Solid REQUIRED)
find_package(KF6FileMetaData REQUIRED)
find_package(KF6Config REQUIRED)
find_package(LibArchive REQUIRED)
find_package(ZLIB REQUIRED)
find_package(TagLib REQUIRED)
find_package(exiv2 REQUIRED CONFIG)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBACL REQUIRED IMPORTED_TARGET libacl)
file(GLOB TEST_HEADERS CONFIGURE_DEPENDS src/*.h)
add_executable(pane-test src/thispcview.cpp src/actionstatecontroller.cpp src/appwidgets.cpp src/applicationstyle.cpp src/directorylistingcore.cpp src/drivehomecoordinator.cpp src/soliddevicemonitor.cpp src/devicemountcontroller.cpp src/deviceremovalcontroller.cpp src/keyboardnavigation.cpp src/locationpresentation.cpp src/navigationhistory.cpp src/paneadapter.cpp src/panemenucontroller.cpp src/primarybrowserpane.cpp src/previewcoordinator.cpp src/previewcontroller.cpp src/directorypreviewadapter.cpp src/tabcontroller.cpp src/searchuicontroller.cpp src/selectionmenucontroller.cpp src/remoteurlhelper.cpp src/savedremotelocation.cpp src/savedremotelocationdialog.cpp ${TEST_HEADERS})
target_compile_options(pane-test PRIVATE -g3 -O0 -fno-omit-frame-pointer -Wno-unused-function -Wno-unused-variable)
target_include_directories(pane-test PRIVATE ${LibArchive_INCLUDE_DIRS})
target_link_libraries(pane-test PRIVATE Qt6::Core Qt6::Concurrent Qt6::Gui Qt6::Widgets Qt6::PrintSupport Qt6::Pdf Qt6::DBus Qt6::Test KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::ItemViews KF6::Solid KF6::FileMetaData KF6::ConfigCore ${LibArchive_LIBRARIES} ZLIB::ZLIB TagLib::TagLib Exiv2::exiv2lib PkgConfig::LIBACL)
target_compile_definitions(pane-test PRIVATE THISPC_BATCH_RENAME_TEST_HOOKS=1 THISPC_TEST_HARNESS=1)
'''
    (tmp / 'CMakeLists.txt').write_text(cmake)
    subprocess.run(['cmake', '-S', str(tmp), '-B', str(tmp / 'build')], check=True)
    subprocess.run(['cmake', '--build', str(tmp / 'build'), '-j2'], check=True)
    # A private bus without desktop service activation keeps tests isolated
    # from Plasma daemons and the user's live display.
    bus_config = tmp / 'test-bus.conf'
    bus_config.write_text('''<busconfig>
<type>session</type>
<listen>unix:tmpdir=/tmp</listen>
<auth>EXTERNAL</auth>
<policy context="default">
<allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/>
</policy>
</busconfig>
''')
    for suite in selected:
        disk_data = Path(disk_tmp) / suite
        runtime = tmp / suite / 'runtime'
        runtime.mkdir(parents=True, mode=0o700)
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_FORCE_STDERR_LOGGING='1',
                   XDG_RUNTIME_DIR=str(runtime),
                   DISPLAY='', WAYLAND_DISPLAY='',
                   XDG_CONFIG_HOME=str(tmp / suite / 'config'), XDG_CACHE_HOME=str(tmp / suite / 'cache'),
                   XDG_DATA_HOME=str(disk_data / 'data'), THISPC_TEST_FILES=str(disk_data),
                   LANG='C.UTF-8', LC_ALL='C.UTF-8')
        if suite == 'batch_rename':
            env['THISPC_BLACKBOX_VIEW'] = str(root / 'build/bin/thispc-view-stage3c1-test')
            env['THISPC_PRODUCTION_VIEW'] = str(root / 'build/bin/thispc-view')
        if suite != 'batch_rename':
            env['THISPC_RECOVERY_STARTUP_SYNC'] = '1'
        command = [str(tmp / 'build/pane-test'), suite]
        if suite not in {'operations', 'local_transfer', 'transfer_plan', 'local_move', 'local_tree', 'tree_history',
                         'sidebar_layout', 'archive', 'archive_creation', 'solid_monitor'}:
            command = ['dbus-run-session', '--config-file=' + str(bus_config), '--'] + command
        print(f'RUN SUITE: {suite}', flush=True)
        subprocess.run(command, env=env, check=True, timeout=60)
