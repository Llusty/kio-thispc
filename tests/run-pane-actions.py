#!/usr/bin/env python3
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
group = parser.add_mutually_exclusive_group()
group.add_argument('--tabs', action='store_true', help='run tab drag/drop tests instead of pane actions')
group.add_argument('--sidebar-dnd', action='store_true', help='run sidebar drag/drop tests')
group.add_argument('--sidebar-layout', action='store_true', help='run sidebar scroll/resize tests')
group.add_argument('--properties', action='store_true', help='run real PropertiesDialog tests')
group.add_argument('--search', action='store_true', help='run real KIO search and cancellation tests')
group.add_argument('--actions', action='store_true', help='run real FileActions and Undo tests')
group.add_argument('--operations', action='store_true', help='run OperationManager state tests')
group.add_argument('--local-transfer', action='store_true', help='run native local copy pause/resume tests')
group.add_argument('--transfer-plan', action='store_true', help='run native directory transfer planning tests')
group.add_argument('--local-move', action='store_true', help='run safe native move and history tests')
group.add_argument('--all', action='store_true', help='run every regression suite')
group.add_argument('--suites', nargs='+', choices=[
    'panes', 'tabs', 'properties', 'search', 'actions', 'operations',
    'local_transfer', 'transfer_plan', 'local_move', 'local_tree', 'tree_history',
    'sidebar_dnd', 'sidebar_layout'],
    help='build once and run only the selected regression suites')
options = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'src/thispcview.cpp').read_text()

file_actions = (root / 'src/fileactions.h').read_text()

def intercept(text, signature, marker, statement):
    start = text.index(signature)
    end = text.index('\n    }', start)
    point = text.index(marker, start, end)
    return text[:point] + '        if (interceptFileJobs) { ' + statement + ' return; }\n' + text[point:]

for signature, marker, statement in [
    ('    void trashSelected(', '        KIO::CopyJob *job =', 'dispatch = {"trash", urls, {}};'),
    ('    void renameSelected(', '        KIO::CopyJob *job =', 'dispatch = {"rename", {source}, destination};'),
    ('    void pasteClipboardInto(', '        if (startNativeSingleFileTransfer(', 'dispatch = {cut ? "move" : "copy", urls, destination};'),
    ('    void createNewFolder(', '        KIO::MkdirJob *job =', 'dispatch = {"mkdir", {}, destination};'),
    ('    void createNewFile(', '        KIO::StoredTransferJob *job =', 'dispatch = {"create", {}, destination};'),
    ('    void transfer(', '        if (startNativeSingleFileTransfer(', 'dispatch = {action == Qt::MoveAction ? "move" : "copy", urls, destination};'),
]:
    file_actions = intercept(file_actions, signature, marker, statement)
source = intercept(source, '    void showPropertiesDialog(', '        PropertiesDialog::show(',
                   'dispatch = {"properties", {url}, {}};')

def expose(text):
    return text.replace('private:', 'public:').replace('protected:', 'public:').replace('    Q_OBJECT', '    Q_OBJECT\npublic:')

source = expose(source)
source = source.replace('int main(int argc, char **argv)', 'int applicationMain(int argc, char **argv)')
prelude = '''#include <QtTest>
#include <KIO/RenameDialog>
#include "localfilecopyjob.h"
#include "localfilemovejob.h"
#include "localtransferplan.h"
#include "localtransferjob.h"
#include "localtreehistory.h"
struct Dispatch { QString kind; QList<QUrl> sources; QUrl destination; };
static Dispatch dispatch;
static bool interceptFileJobs = true;
'''
(root / 'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='thispc-pane-tests-') as tmp, tempfile.TemporaryDirectory(
        prefix='test-data-', dir=root / 'build') as disk_tmp:
    tmp = Path(tmp)
    (tmp / 'src').mkdir()
    for header in (root / 'src').glob('*.h'):
        (tmp / 'src' / header.name).write_text(expose(file_actions if header.name == 'fileactions.h' else header.read_text()))
    suites = {'panes': 'pane-actions.cpp', 'tabs': 'tab-drag-drop.cpp', 'sidebar_dnd': 'sidebar-drag-drop.cpp', 'sidebar_layout': 'sidebar-layout.cpp', 'properties': 'properties-dialog.cpp', 'search': 'search-controller.cpp', 'actions': 'file-actions.cpp', 'operations': 'operation-manager.cpp', 'local_transfer': 'local-file-copy-job.cpp', 'transfer_plan': 'local-transfer-plan.cpp', 'local_move': 'local-file-move-job.cpp', 'local_tree': 'local-transfer-job.cpp', 'tree_history': 'local-tree-history.cpp'}
    selected = options.suites or (list(suites) if options.all else ['sidebar_layout' if options.sidebar_layout else 'sidebar_dnd' if options.sidebar_dnd else 'local_move' if options.local_move else 'transfer_plan' if options.transfer_plan else 'local_transfer' if options.local_transfer else 'operations' if options.operations else 'actions' if options.actions else 'tabs' if options.tabs else 'properties' if options.properties else 'search' if options.search else 'panes'])
    combined = prelude + source
    for suite in selected:
        disk_data = Path(disk_tmp) / suite
        disk_data.mkdir()
        test = (root / 'tests' / suites[suite]).read_text().replace('int main(', 'int run(')
        # Unlike main(), an ordinary int function must return explicitly.
        end = test.rfind('}')
        test = test[:end] + '    return 0;\n' + test[end:]
        combined += '\nnamespace ' + suite + ' {\n' + test + '\n}\n'
    combined += '\nint main(int argc, char **argv) {\n'
    for suite in selected:
        combined += f'    if (argc > 1 && QByteArray(argv[1]) == "{suite}") return {suite}::run(argc, argv);\n'
    combined += '    return 2;\n}\n'
    (tmp / 'src/thispcview.cpp').write_text(combined)
    cmake = '''cmake_minimum_required(VERSION 3.16)
project(thispc-regressions LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport Test)
find_package(KF6KIO REQUIRED)
file(GLOB TEST_HEADERS CONFIGURE_DEPENDS src/*.h)
add_executable(pane-test src/thispcview.cpp ${TEST_HEADERS})
target_compile_options(pane-test PRIVATE -g0 -O0 -Wno-unused-function -Wno-unused-variable)
target_link_libraries(pane-test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::PrintSupport Qt6::Test KF6::KIOCore KF6::KIOWidgets)
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
        runtime = tmp / suite / 'runtime'
        runtime.mkdir(parents=True, mode=0o700)
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_FORCE_STDERR_LOGGING='1',
                   XDG_RUNTIME_DIR=str(runtime),
                   DISPLAY='', WAYLAND_DISPLAY='',
                   XDG_CONFIG_HOME=str(tmp / suite / 'config'), XDG_CACHE_HOME=str(tmp / suite / 'cache'),
                   XDG_DATA_HOME=str(disk_data / 'data'), THISPC_TEST_FILES=str(disk_data),
                   LANG='C.UTF-8', LC_ALL='C.UTF-8')
        command = [str(tmp / 'build/pane-test'), suite]
        if suite not in {'operations', 'local_transfer', 'transfer_plan', 'local_move', 'local_tree', 'tree_history',
                         'sidebar_layout'}:
            command = ['dbus-run-session', '--config-file=' + str(bus_config), '--'] + command
        subprocess.run(command, env=env, check=True, timeout=60)
