"""Exercise the real attachment API in an isolated mock-provider runtime."""
import base64
import json
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

backend = Path(__file__).resolve().parents[1]
binary = backend / 'build' / 'cognitive-os-agent.exe'
png = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aK1sAAAAASUVORK5CYII=')
with socket.socket() as sock:
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
with tempfile.TemporaryDirectory(prefix='image-preview-test-', dir=backend / 'build') as directory:
    state = Path(directory)
    (state / 'cognitive-os-agent.json').write_text('{"llm.provider":"mock"}', encoding='utf-8')
    process = subprocess.Popen([str(binary), '--state', str(state), '--workspace', str(backend), 'serve', str(port)], cwd=backend, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def request(path, method='GET', data=None):
        req = urllib.request.Request(f'http://127.0.0.1:{port}' + path, method=method, data=data)
        try:
            with urllib.request.urlopen(req, timeout=10) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)
    try:
        for _ in range(100):
            try:
                request('/v1/uploads')
                break
            except OSError:
                time.sleep(.1)
        else:
            raise AssertionError('isolated runtime did not start')
        status, upload = request('/v1/upload?name=preview.png&session=preview-A', 'POST', png)
        assert status == 200 and upload['status'] == 'ready', upload
        image_id = upload['id']
        status, preview = request(f'/v1/uploads/preview?id={image_id}&session=preview-A')
        assert status == 200, preview
        assert preview['data_url'] == 'data:image/png;base64,' + base64.b64encode(png).decode()
        assert set(preview) == {'data_url', 'name'}, 'preview must not expose filesystem paths'
        assert request(f'/v1/uploads/preview?id={image_id}&session=preview-B')[0] == 404
        assert request('/v1/uploads/preview?id=..%2F..%2Fconfig&session=preview-A')[0] == 404
        _, text = request('/v1/upload?name=note.txt&session=preview-A', 'POST', b'hello')
        assert request(f'/v1/uploads/preview?id={text["id"]}&session=preview-A')[0] == 404
        assert request('/v1/uploads/' + image_id, 'DELETE')[0] == 200
        assert request(f'/v1/uploads/preview?id={image_id}&session=preview-A')[0] == 404
        print('PASS: real image upload/preview, exact bytes, session isolation, invalid ID, non-image and deleted-image rejection; mock provider, no model API calls.')
    finally:
        process.terminate()
        process.wait(timeout=10)
