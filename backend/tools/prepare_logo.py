"""Refresh committed icon raster sizes from the repository logo.png (Pillow)."""
from pathlib import Path
from PIL import Image
root=Path(__file__).resolve().parents[2]
out=root/'backend/assets/logo';out.mkdir(parents=True,exist_ok=True)
with Image.open(root/'logo.png') as source:
    for size in (16,32,64,128,256,512):
        source.convert('RGBA').resize((size,size),Image.Resampling.LANCZOS).save(out/f'{size}.png')
