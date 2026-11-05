#!/usr/bin/env python3
"""Rewrite the checksum block on the website download page.

The release workflow downloads every built artifact under dist/ and runs
this script. It flattens the artifacts into one directory, hashes each
file and regenerates the verify <pre><code> block in website/index.html so
the page lists exactly what was shipped. Pushing the updated page triggers
the Pages deployment.
"""
import hashlib
import os
import pathlib
import re
import shutil
import sys

dist = sys.argv[1] if len(sys.argv) > 1 else 'dist'
flat = sys.argv[2] if len(sys.argv) > 2 else 'flat'

os.makedirs(flat, exist_ok=True)
for root, _, files in os.walk(dist):
    for name in files:
        shutil.copy(os.path.join(root, name), os.path.join(flat, name))

names = sorted(os.listdir(flat))
if not names:
    sys.exit(f'no artifacts found under {dist}')

lines = ['sha256sum ' + names[0]]
for name in names:
    with open(os.path.join(flat, name), 'rb') as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    lines.append(f'{digest}  {name}')

block = '<pre><code>' + '\n'.join(lines) + '</code></pre>'
page = pathlib.Path('website/index.html')
html = page.read_text()
html, count = re.subn(r'<pre><code>sha256sum .*?</code></pre>', block, html,
                      count=1, flags=re.S)
if count != 1:
    sys.exit('verify block not found on the page')
page.write_text(html)
print(f'verify block now lists {len(names)} files')
