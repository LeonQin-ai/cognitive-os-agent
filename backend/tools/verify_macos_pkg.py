"""Validate expanded pkgbuild metadata (runs on the actual macOS package)."""
import sys,xml.etree.ElementTree as ET
from pathlib import Path
root=Path(sys.argv[1]);files=list(root.rglob('PackageInfo'));assert files,'PackageInfo missing'
for path in files:
 node=ET.parse(path).getroot()
 assert node.attrib['identifier']=='org.cognitiveos.desktop'
 assert node.attrib['version']==sys.argv[2]
 assert node.attrib['install-location']=='/Applications'
 bundles=node.findall('.//bundle')
 assert any(b.attrib.get('id')=='org.cognitiveos.desktop' and 'Cognitive OS.app' in b.attrib.get('path','') for b in bundles),bundles
print('PASS: macOS Installer identifier, version, app payload and /Applications destination')
