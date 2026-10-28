#!/usr/bin/env python3
"""Checks relative links (incl. heading anchors) in all tracked Markdown files. Wiki pages (wiki/*.md) link each other without extension; absolute links to the GitHub wiki and, from wiki pages, to blob/main paths are checked against the repo. Usage: scripts/check-doc-links.py [repo-root]. Exit 1 on broken links."""
import os, re, subprocess, sys
root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
files = subprocess.check_output(['git', 'ls-files', '*.md', '--others', '--cached', '--exclude-standard'], cwd=root, text=True).split()
files = sorted(set(f for f in files if os.path.exists(os.path.join(root, f))))

def slug(h):
    h = h.strip().lower()
    h = re.sub(r'[`*_]', '', h)
    h = re.sub(r'[^\w\- ]', '', h)
    return h.replace(' ', '-')

anchors = {}
def get_anchors(path):
    if path not in anchors:
        a = set()
        in_code = False
        for line in open(path, encoding='utf-8'):
            if line.startswith('```'):
                in_code = not in_code
            if not in_code:
                m = re.match(r'#+\s+(.*)', line)
                if m:
                    a.add(slug(m.group(1)))
        anchors[path] = a
    return anchors[path]

WIKI_URL = r'https://github\.com/phabioo/uzume/wiki/?'
BLOB_URL = r'https://github\.com/phabioo/uzume/(?:blob|tree)/main/'
bad = 0
for f in files:
    p = os.path.join(root, f)
    text = open(p, encoding='utf-8').read()
    text = re.sub(r'```.*?```', '', text, flags=re.S)
    text = re.sub(r'`[^`\n]*`', '', text)
    for m in re.finditer(r'\[[^\]]*\]\(([^)\s]+)\)', text):
        t = m.group(1)
        wiki_page = f.startswith('wiki/')
        w = re.match(WIKI_URL + r'([^#/]*)(?:#(.*))?$', t)
        b = re.match(BLOB_URL + r'([^#]+)(?:#(.*))?$', t) if wiki_page else None
        if w:
            path, frag = (w.group(1) or 'Home') + '.md', w.group(2) or ''
            target = os.path.join(root, 'wiki', path)
        elif b:
            path, frag = b.group(1), b.group(2) or ''
            target = os.path.join(root, path)
        elif re.match(r'[a-z]+:', t):
            continue
        else:
            path, _, frag = t.partition('#')
            if wiki_page and path and not os.path.splitext(path)[1] and '/' not in path:
                path += '.md'
            target = os.path.normpath(os.path.join(os.path.dirname(p), path)) if path else p
        if not os.path.exists(target):
            print(f'{f}: missing {t}')
            bad += 1
        elif frag and target.endswith('.md') and frag not in get_anchors(target):
            print(f'{f}: missing anchor {t}')
            bad += 1

# Docs rule (docs/workflow.md): the root README stays a one-screen overview. A paragraph or list item over this many
# characters (outside code blocks and HTML) fails the check.
README_MAX_PARAGRAPH = 600
readme = os.path.join(root, 'README.md')
if os.path.exists(readme):
    text = open(readme, encoding='utf-8').read()
    text = re.sub(r'```.*?```', '', text, flags=re.S)
    text = re.sub(r'<!--.*?-->', '', text, flags=re.S)
    text = re.sub(r'<[^>]+>', '', text)
    for block in re.split(r'\n\s*\n', text):
        items, cur = [], []
        for line in block.split('\n'):
            if re.match(r'\s*([-*]|\d+\.)\s', line) and cur:
                items.append(' '.join(cur))
                cur = []
            cur.append(line.strip())
        items.append(' '.join(cur))
        for item in items:
            if len(item) > README_MAX_PARAGRAPH:
                print(f'README.md: paragraph of {len(item)} characters (limit {README_MAX_PARAGRAPH}), '
                      f'move detail to docs/ and link it: "{item[:60]}..."')
                bad += 1
print('broken:', bad, 'files:', len(files))
sys.exit(1 if bad else 0)
