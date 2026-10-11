"""Bounded attachment processor, embedded into the native binary at build time.

argv: original-file extension output-prefix. Output is one JSON object on stdout.
Optional converters: LibreOffice or Windows Word/PowerPoint (fresh COM instances).
No uploaded file is executed and document macros are disabled during conversion.
"""
import base64
import io
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import warnings
import xml.etree.ElementTree as ET
import zipfile

MAX_FRAMES = 6
MAX_TEXT = 1_000_000
MAX_ARCHIVE = 64 * 1024 * 1024
MAX_PIXELS = 40_000_000


def sample_indices(count):
    if count <= MAX_FRAMES: return list(range(count))
    return sorted({round(i * (count - 1) / (MAX_FRAMES - 1)) for i in range(MAX_FRAMES)})


def image_frame(image, prefix, frames, label, time_ms=None):
    from PIL import Image
    if len(frames) >= MAX_FRAMES: return
    if image.width * image.height > MAX_PIXELS: raise ValueError('image pixel limit exceeded')
    image.thumbnail((1280, 1280))
    if image.mode != 'RGB':
        converted = Image.new('RGB', image.size, 'white')
        if image.mode == 'RGBA': converted.paste(image, mask=image.getchannel('A'))
        else: converted.paste(image.convert('RGB'))
        image = converted
    path = f'{prefix}.frame{len(frames)}.jpg'
    image.save(path, 'JPEG', quality=78)
    frame = {'file': Path(path).name, 'label': label, 'mime': 'image/jpeg'}
    if time_ms is not None: frame['time_ms'] = time_ms
    frames.append(frame)


def render_pdf(path, prefix, result):
    try:
        import pymupdf as fitz
    except ImportError:
        from pypdf import PdfReader
        reader = PdfReader(path)
        if len(reader.pages) > 1000: raise ValueError('PDF page limit exceeded')
        parts = []
        for page in reader.pages:
            parts.append(page.extract_text() or '')
            if sum(map(len, parts)) > MAX_TEXT: raise ValueError('text limit exceeded')
        result['text'] = '\n'.join(parts)
        result['warnings'].append('Install PyMuPDF and Pillow for PDF page images and scanned PDF analysis')
        return
    from PIL import Image
    with fitz.open(path) as document:
        if document.needs_pass: raise ValueError('password-protected PDF requires a decrypted copy')
        if len(document) > 1000: raise ValueError('PDF page limit exceeded')
        parts, length = [], 0
        indices = set(sample_indices(len(document)))
        for i, page in enumerate(document):
            text = page.get_text(); length += len(text)
            if length > MAX_TEXT: raise ValueError('text limit exceeded')
            parts.append(f'Page {i+1}\n{text}')
            if i in indices:
                scale = min(2.0, 1280 / max(page.rect.width, page.rect.height, 1))
                pix = page.get_pixmap(matrix=fitz.Matrix(scale, scale), alpha=False)
                image = Image.open(io.BytesIO(pix.tobytes('png')))
                image_frame(image, prefix, result['frames'], f'Page {i+1}')
        result['text'] = '\n'.join(parts)
        result['page_count'] = len(document)
        if len(document) > MAX_FRAMES: result['warnings'].append('Page images are sampled; full extracted text is retained')


def soffice_path():
    configured = os.getenv('COA_LIBREOFFICE')
    if configured: return configured
    found = shutil.which('soffice') or shutil.which('libreoffice')
    if found: return found
    for env in ('ProgramFiles', 'ProgramFiles(x86)'):
        path = Path(os.getenv(env, '')) / 'LibreOffice/program/soffice.exe'
        if path.is_file(): return str(path)
    return None


def office_pdf(path, ext, folder):
    target = Path(folder) / 'converted.pdf'
    source = Path(folder) / ('source.' + ext)
    shutil.copyfile(path, source)
    converter = soffice_path()
    if converter:
        profile = (Path(folder) / 'lo-profile').resolve().as_uri()
        process = subprocess.run([converter, '-env:UserInstallation='+profile, '--headless',
                                  '--convert-to', 'pdf', '--outdir', folder, str(source)],
                                 capture_output=True, timeout=35)
        produced = Path(folder) / 'source.pdf'
        if process.returncode != 0 or not produced.is_file(): raise ValueError('LibreOffice conversion failed')
        return produced
    if os.name != 'nt': raise ValueError('LibreOffice is required for legacy Office documents and page rendering')
    import win32com.client
    app, doc = None, None
    settings, initially_empty, update_links = {}, False, None
    try:
        if ext in ('ppt', 'pptx'):
            app = win32com.client.DispatchEx('PowerPoint.Application')
            initially_empty = app.Presentations.Count == 0
            settings = {'AutomationSecurity': app.AutomationSecurity, 'DisplayAlerts': app.DisplayAlerts}
            app.AutomationSecurity = 3
            app.DisplayAlerts = 1
            doc = app.Presentations.Open(str(source.resolve()), ReadOnly=True, Untitled=False, WithWindow=False)
            doc.SaveAs(str(target.resolve()), 32)
        else:
            app = win32com.client.DispatchEx('Word.Application')
            initially_empty = app.Documents.Count == 0
            settings = {'AutomationSecurity': app.AutomationSecurity, 'DisplayAlerts': app.DisplayAlerts}
            if initially_empty: app.Visible = False
            app.DisplayAlerts = 0
            app.AutomationSecurity = 3
            update_links = app.Options.UpdateLinksAtOpen
            app.Options.UpdateLinksAtOpen = False
            doc = app.Documents.Open(str(source.resolve()), ConfirmConversions=False,
                                     ReadOnly=True, AddToRecentFiles=False, Visible=False)
            doc.ExportAsFixedFormat(str(target.resolve()), 17)
        if not target.is_file(): raise ValueError('Office did not produce a PDF')
        return target
    finally:
        if doc is not None:
            try:
                if ext in ('ppt', 'pptx'): doc.Close()
                else: doc.Close(False)
            except Exception: pass
        if app is not None:
            for key, value in settings.items():
                try: setattr(app, key, value)
                except Exception: pass
            if update_links is not None:
                try: app.Options.UpdateLinksAtOpen = update_links
                except Exception: pass
            try:
                remaining = app.Presentations.Count if ext in ('ppt','pptx') else app.Documents.Count
                if initially_empty and remaining == 0: app.Quit()
            except Exception: pass


def office_xml(path, ext, prefix, result):
    from PIL import Image
    parts = []
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if len(entries) > 10000 or sum(i.file_size for i in entries) > MAX_ARCHIVE:
            raise ValueError('Office archive expansion limit exceeded')
        names = {i.filename for i in entries}
        for info in entries:
            name = info.filename
            if name.endswith('.xml') and (name.startswith('word/') or name.startswith('xl/') or
                                          name.startswith('ppt/slides/') or name.startswith('ppt/notesSlides/')):
                tree = ET.fromstring(archive.read(info))
                text = ' '.join(t.text for t in tree.iter() if t.text and t.tag.split('}')[-1] in ('t', 'v'))
                if text: parts.append(name + '\n' + text)
                if sum(map(len, parts)) > MAX_TEXT: raise ValueError('text limit exceeded')
        result['text'] = '\n'.join(parts)
        # Prefer complete rendered pages, including charts, tables and diagrams.
        if ext in ('docx', 'pptx') and '[Content_Types].xml' in names:
            try:
                with tempfile.TemporaryDirectory(prefix='coa-office-') as folder:
                    converted = office_pdf(path, ext, folder)
                    rendered = {'frames': [], 'warnings': []}
                    render_pdf(converted, prefix, rendered)
                    result['frames'] = rendered['frames']
                    result['warnings'].extend(rendered['warnings'])
                    result['page_count'] = rendered.get('page_count', 0)
                    if not result['text'].strip(): result['text'] = rendered['text']
            except Exception as error:
                result['warnings'].append('Page rendering unavailable: '+str(error))
        # A portable fallback still makes embedded photographs accessible.
        if not result['frames']:
            for info in entries:
                if not ('/media/' in info.filename) or len(result['frames']) >= MAX_FRAMES: continue
                try:
                    with Image.open(io.BytesIO(archive.read(info))) as image:
                        image_frame(image.copy(), prefix, result['frames'], 'Embedded image '+info.filename)
                except Exception: pass


def video(path, prefix, result):
    import cv2
    from PIL import Image
    capture = cv2.VideoCapture(str(path))
    try:
        if not capture.isOpened(): raise ValueError('video decoder cannot open this container or codec')
        count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = float(capture.get(cv2.CAP_PROP_FPS))
        width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH)); height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
        if width * height > MAX_PIXELS: raise ValueError('video pixel limit exceeded')
        if count <= 0 or fps <= 0: raise ValueError('video duration metadata unavailable')
        for index in sample_indices(count):
            capture.set(cv2.CAP_PROP_POS_FRAMES, index)
            ok, frame = capture.read()
            if not ok: continue
            ms = round(index * 1000 / fps)
            image = Image.fromarray(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
            image_frame(image, prefix, result['frames'], f'Video frame at {ms/1000:.2f}s', ms)
        if not result['frames']: raise ValueError('no video frames could be decoded')
        result.update(duration_ms=round(count*1000/fps), source_frame_count=count)
        result['text'] = (f'Video duration: {result["duration_ms"]/1000:.2f}s. '
                          f'{count} source frames; {len(result["frames"])} sampled frames with timestamps. '
                          'These are samples, not an exhaustive view of every event. Audio is not transcribed.')
        result['warnings'].append('Sampled visual analysis; audio is not transcribed')
    finally: capture.release()


def video_fallback(path, prefix, result):
    """FFmpeg fallback for codecs not included in the installed OpenCV build."""
    from PIL import Image
    executable = os.getenv('COA_FFMPEG') or shutil.which('ffmpeg')
    if not executable:
        import imageio_ffmpeg
        executable = imageio_ffmpeg.get_ffmpeg_exe()
    probe = subprocess.run([executable,'-hide_banner','-i',str(path)],capture_output=True,timeout=10)
    match = re.search(rb'Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)',probe.stderr)
    if not match: raise ValueError('video duration unavailable to FFmpeg')
    duration = int(match[1])*3600+int(match[2])*60+float(match[3])
    if duration<=0: raise ValueError('empty video')
    fps_match=re.search(rb'([0-9]+(?:\.[0-9]+)?) fps',probe.stderr)
    fps=float(fps_match[1]) if fps_match else 0
    # Duration includes the final frame's display interval. Seeking within
    # that interval can produce EOF rather than an image on short clips.
    last=max(0,duration-(1/fps+.001 if fps>0 else .25))
    samples=min(MAX_FRAMES,max(1,round(duration*fps))) if fps>0 else MAX_FRAMES
    for i in range(samples):
        seconds = i*last/(samples-1) if samples>1 else 0
        output = f'{prefix}.decode.jpg'
        run = subprocess.run([executable,'-y','-loglevel','error','-ss',str(seconds),'-i',str(path),
                              '-frames:v','1','-vf','scale=1280:1280:force_original_aspect_ratio=decrease',
                              output],capture_output=True,timeout=6)
        if run.returncode or not Path(output).is_file(): continue
        try:
            with Image.open(output) as image:
                image_frame(image.copy(),prefix,result['frames'],f'Video frame at {seconds:.2f}s',round(seconds*1000))
        finally: Path(output).unlink(missing_ok=True)
    if not result['frames']: raise ValueError('FFmpeg could not decode video frames')
    result['duration_ms']=round(duration*1000)
    result['text']=f'Video duration: {duration:.2f}s. Sampled frames with timestamps. Audio is not transcribed.'
    result['warnings'].append('Sampled visual analysis; audio is not transcribed')


def process(path, ext, prefix):
    result = {'text': '', 'frames': [], 'warnings': [], 'kind': 'document'}
    if ext in ('mp4','mov','avi','mkv','webm','m4v','mpeg','mpg','wmv','flv','3gp'):
        result['kind'] = 'video'
        try: video(path, prefix, result)
        except Exception:
            result['frames']=[]
            video_fallback(path,prefix,result)
    elif ext in ('png','jpg','jpeg','gif','webp','bmp','tif','tiff','heic','heif','avif','svg'):
        result['kind'] = 'image'
        if ext in ('heic','heif'):
            import pillow_heif
            pillow_heif.register_heif_opener()
        from PIL import Image
        if ext == 'svg':
            import pymupdf as fitz
            # External links must not fetch network/file resources during rendering.
            source = Path(path).read_bytes()
            upper=source.upper()
            if b'<!ENTITY' in upper or b'<!DOCTYPE' in upper or b'@IMPORT' in upper:
                raise ValueError('SVG with external references is not supported')
            tree=ET.fromstring(source)
            for node in tree.iter():
                for key,value in node.attrib.items():
                    if key.split('}')[-1]=='href' and not value.startswith(('#','data:image/')):
                        raise ValueError('SVG external image reference is not supported')
            for match in re.finditer(rb'url\(\s*[\'"]?([^\)\'"\s]+)',source,re.I):
                if not match[1].startswith(b'#'): raise ValueError('SVG external style reference is not supported')
            with fitz.open(stream=source,filetype='svg') as document:
                page=document[0]
                scale=1280/max(page.rect.width,page.rect.height,1)
                image=Image.open(io.BytesIO(page.get_pixmap(matrix=fitz.Matrix(scale,scale),alpha=False).tobytes('png')))
        else: image = Image.open(path)
        with image:
            if image.width*image.height>MAX_PIXELS: raise ValueError('image pixel limit exceeded')
            for index in sample_indices(getattr(image,'n_frames',1)):
                image.seek(index)
                image_frame(image.copy(),prefix,result['frames'],f'Image frame {index+1}')
    elif ext == 'pdf': render_pdf(path, prefix, result)
    elif ext in ('doc','ppt','rtf','odt','odp'):
        with tempfile.TemporaryDirectory(prefix='coa-office-') as folder:
            converted = office_pdf(path, ext, folder); render_pdf(converted, prefix, result)
    elif ext in ('docx','pptx','xlsx'): office_xml(path, ext, prefix, result)
    else: raise ValueError('unsupported attachment format')
    if len(result['text']) > MAX_TEXT: raise ValueError('text limit exceeded')
    if not result['text'].strip() and not result['frames']: raise ValueError('no readable content')
    return result


if __name__ == '__main__':
    warnings.filterwarnings('ignore')
    try:
        path, ext, prefix = sys.argv[1:]
        print(json.dumps(process(path, ext.lower(), prefix), ensure_ascii=True))
    except Exception as error:
        print(json.dumps({'error':str(error)}, ensure_ascii=True))
