"""A browser can navigate host directories when no native dialog is available."""
import json
import os
import socket
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

root = Path(__file__).resolve().parents[1]
binary = root / os.environ.get('COA_BUILD_DIR', 'build') / ('cognitive-os-agent.exe' if os.name == 'nt' else 'cognitive-os-agent')
with tempfile.TemporaryDirectory(prefix='coa-directory-browser-') as tmp:
    state = Path(tmp)
    (state / 'child').mkdir()
    (state / 'ordinary.txt').write_text('file')
    (state / 'cognitive-os-agent.json').write_text('{"llm.provider":"mock"}')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    with (root / 'build-directory-browser.log').open('w') as log:
        proc = subprocess.Popen([str(binary), '--state', tmp, '--workspace', tmp, 'serve', str(port)], cwd=root,
                                stdout=log, stderr=log, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            base = f'http://127.0.0.1:{port}'
            for _ in range(100):
                try:
                    with urllib.request.urlopen(base + '/v1/scheduler', timeout=2):
                        break
                except OSError:
                    assert proc.poll() is None
                    time.sleep(.05)
            url = base + '/v1/workspaces/directories?path=' + urllib.parse.quote(tmp, safe='')
            with urllib.request.urlopen(url, timeout=5) as response:
                result = json.load(response)
            assert result['path'].replace('\\', '/').casefold() == str(state.resolve()).replace('\\', '/').casefold(), result
            assert 'child' in result['directories'] and 'ordinary.txt' not in result['directories'], result
            assert result['parent'] != result['path'], result
            print('PASS: server directory navigation returns directories only')
        finally:
            proc.terminate()
            proc.wait(timeout=5)
