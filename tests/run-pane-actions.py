#!/usr/bin/env python3
"""Build a temporary instrumented window and exercise real Qt actions offscreen.

Requires an existing Debug build (./scripts/build.sh). Only test copies expose
private members. File jobs are intercepted immediately before KIO dispatch;
selection, focus, shortcuts, menus and rename/Trash dialogs remain real.
"""
import json
import os
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tabs', action='store_true', help='run tab drag/drop tests instead of pane actions')
options = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'src/thispcview.cpp').read_text()

def intercept(signature, marker, statement):
    global source
    start = source.index(signature)
    point = source.index(marker, start)
    source = source[:point] + statement + '\n        return;\n' + source[point:]

intercept('    void trashSelected()', '        KIO::CopyJob *job =',
          '        dispatch = {"trash", urls, {}};')
intercept('    void renameSelected()', '        KIO::CopyJob *job =',
          '        dispatch = {"rename", {source}, destination};')
intercept('    void pasteClipboardInto(', '        KIO::CopyJob *job =',
          '        dispatch = {cut ? "move" : "copy", urls, destination};')
intercept('    void createNewFolder()', '        KIO::MkdirJob *job =',
          '        dispatch = {"mkdir", {}, destination};')
intercept('    void createNewFile(', '        KIO::StoredTransferJob *job =',
          '        dispatch = {"create", {}, destination};')
intercept('    void showPropertiesDialog(', '        KFileItem fileItem(',
          '        dispatch = {"properties", {url}, {}};')
intercept('    void handleDroppedUrls(', '        KIO::CopyJob *job =',
          '        dispatch = {action == Qt::MoveAction ? "move" : "copy", urls, destination};')
source = source.replace('private:', 'public:').replace('protected:', 'public:')
source = source.replace('    Q_OBJECT', '    Q_OBJECT\npublic:')
source = source.replace('int main(int argc, char **argv)', 'int applicationMain(int argc, char **argv)')
prelude = '''#include <QtTest>
struct Dispatch { QString kind; QList<QUrl> sources; QUrl destination; };
static Dispatch dispatch;
'''
with tempfile.TemporaryDirectory(prefix='thispc-pane-tests-') as tmp:
    tmp = Path(tmp)
    cpp = tmp / 'pane-test.cpp'
    cpp.write_text(prelude + source + '\n' + (root / 'tests' / ('tab-drag-drop.cpp' if options.tabs else 'pane-actions.cpp')).read_text())
    commands = json.loads((root / 'build/compile_commands.json').read_text())
    entry = next(c for c in commands if c['file'].endswith('/src/thispcview.cpp'))
    args = shlex.split(entry['command'])
    output = args.index('-o')
    del args[output:output + 2]
    args.remove('-c')
    args.remove(entry['file'])
    # Test-only compilation avoids debug info for the large translation unit.
    args += ['-g0', '-O0', '-Wno-unused-function', '-Wno-unused-variable',
             '-UQT_NO_CAST_FROM_ASCII', '-UQT_NO_CAST_TO_ASCII']
    args += shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'Qt6Test'], text=True))
    links = shlex.split((root / 'build/CMakeFiles/thispc-view.dir/link.txt').read_text())
    libraries = [arg for arg in links if arg.startswith('/usr/') and '.so' in arg]
    args += [str(cpp), '-o', str(tmp / 'pane-test'), *libraries, '-lQt6Test']
    subprocess.run(args, cwd=root / 'build', check=True)
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_FORCE_STDERR_LOGGING='1', XDG_CONFIG_HOME=str(tmp / 'config'),
               XDG_CACHE_HOME=str(tmp / 'cache'), XDG_DATA_HOME=str(tmp / 'data'), LANG='C.UTF-8', LC_ALL='C.UTF-8')
    subprocess.run([str(tmp / 'pane-test')], env=env, check=True, timeout=60)
