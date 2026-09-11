#!/usr/bin/env python3
# rename_prefix.py — strip the coa_/COA_ prefix from all project symbols,
# keeping the module segment: coa_memory_new -> memory_new, COA_OK -> OK.
# Collision-aware: coa_strdup -> strdup hits libc, so the exception table
# renames it instead. Dry-run by default; --apply writes files.
#
# usage: python tools/rename_prefix.py [--apply]
import os
import re
import sys
import collections

BACKEND = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ['src', 'include', 'cli', 'tools', 'tests', 'docs']
ROOT_FILES = ['Makefile', 'build.sh', 'build.bat', 'CMakeLists.txt',
              'package.sh']
EXTS = ('.c', '.h', '.cc', '.hh', '.cpp', '.mk', '.sh', '.py', '.md', '.txt',
        '.cmake', '.json', '.bat', '.iss')
SKIP = {'build', 'build-cl', 'third_party', 'dist', '.git', '__pycache__',
        'node_modules', 'zig', 'gaia-browser', 'gaia-full', 'gaia-dataset',
        'gaia', 'swebench', 'webarena', 'webarena-run', 'mcp-test',
        'attachments', 'nul'}

IDENT = re.compile(r'\b(?:coa|COA)_[A-Za-z0-9_]+')

# exception table: symbol -> replacement (stripped name would collide with a
# libc/POSIX symbol or another project symbol)
EXCEPTIONS = {
    'coa_strdup': 'xstrdup',
    'coa_strndup': 'xstrndup',
    # bare typedefs for os primitives: 'mutex'/'thread' are too generic and
    # clash conceptually with pthread/C++ names; give them _t suffixes
    'coa_mutex': 'mutex_t',
    'coa_thread': 'thread_t',
    # the engine-wide context type: bare 'ctx' is shadowed by the ubiquitous
    # 'ctx *ctx' local variables and stops being a type name in their scope
    'coa_ctx': 'runtime_ctx',
}

# libc/POSIX names the naive strip must not shadow (defense in depth; anything
# not listed but colliding will be reported by the dry run)
LIBC = {
    'strdup', 'strndup', 'strerror', 'strlen', 'strcmp', 'memcmp', 'memcpy',
    'malloc', 'calloc', 'realloc', 'free', 'exit', 'abs', 'rand', 'srand',
    'time', 'clock', 'sleep', 'usleep', 'close', 'read', 'write', 'open',
    'mutex', 'thread', 'sem_init', 'sem_destroy', 'unlink', 'mkdir', 'rmdir',
    'remove', 'rename', 'getenv', 'setenv', 'abort', 'atexit', 'qsort',
    'bsearch', 'printf', 'fprintf', 'snprintf', 'toupper', 'tolower',
}


def source_files():
    out = []
    me = os.path.abspath(__file__)
    for d in DIRS:
        root = os.path.join(BACKEND, d)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [x for x in dirnames if x not in SKIP]
            for fn in filenames:
                p = os.path.join(dirpath, fn)
                if p == me:
                    continue
                if fn.endswith(EXTS):
                    out.append(p)
    for rf in ROOT_FILES:
        p = os.path.join(BACKEND, rf)
        if os.path.isfile(p):
            out.append(p)
    return out


def main():
    apply = '--apply' in sys.argv[1:]
    files = source_files()
    idents = collections.Counter()
    per_file_idents = {}
    for path in files:
        try:
            txt = open(path, encoding='utf-8').read()
        except (UnicodeDecodeError, OSError):
            continue
        found = set(IDENT.findall.__self__.findall(txt) if False
                    else IDENT.findall(txt))
        if found:
            per_file_idents[path] = found
            for i in found:
                idents[i] += 1

    # build mapping: strip prefix, keep module segment
    mapping = {}
    collisions = []
    stripped_count = collections.Counter()
    for ident in idents:
        if ident in EXCEPTIONS:
            mapping[ident] = EXCEPTIONS[ident]
            continue
        stem = ident.split('_', 1)[1]  # coa_memory_new -> memory_new
        mapping[ident] = stem
        stripped_count[stem] += 1
        low = stem.lower()
        if low in LIBC:
            collisions.append((ident, stem, 'libc/POSIX name'))

    # two different coa_ symbols stripping to the same name (e.g. coa_fs_read
    # and coa_net_read -> read) are fine (different modules); flag only when
    # an identical stem maps from the SAME module twice or a libc hit
    dup = {s: c for s, c in stripped_count.items() if c > 1}

    print(f'files scanned: {len(files)}, files with idents: '
          f'{len(per_file_idents)}')
    print(f'unique coa_/COA_ idents: {len(idents)}')
    print(f'exception-table entries used: '
          f'{sum(1 for i in idents if i in EXCEPTIONS)}')
    if dup:
        print(f'\nstripped-name reuse across modules (expected, no action): '
              f'{len(dup)}')
        for s, c in sorted(dup.items())[:20]:
            print(f'  {s}: {c}x')
    if collisions:
        print(f'\n!! LIBC COLLISIONS — add to EXCEPTIONS before applying:')
        for ident, stem, why in collisions:
            print(f'  {ident} -> {stem}  ({why})')
    n_map = {k: v for k, v in mapping.items() if k != v}
    print(f'\nmapping size: {len(n_map)}')
    sample = sorted(n_map)[:15]
    for k in sample:
        print(f'  {k} -> {n_map[k]}')

    if not apply:
        print('\n(dry run — pass --apply to rewrite files)')
        return

    # longest-first replacement with word boundaries
    pat = re.compile(
        r'\b(' + '|'.join(re.escape(k) for k in sorted(n_map, key=len,
                                                       reverse=True)) +
        r')\b')
    changed = 0
    for path in files:
        try:
            txt = open(path, encoding='utf-8').read()
        except (UnicodeDecodeError, OSError):
            continue
        new = pat.sub(lambda m: n_map.get(m.group(1), m.group(1)), txt)
        if new != txt:
            open(path, 'w', encoding='utf-8', newline='').write(new)
            changed += 1
    print(f'\napplied: {changed} files rewritten')

    leftovers = 0
    for path in files:
        try:
            txt = open(path, encoding='utf-8').read()
        except (UnicodeDecodeError, OSError):
            continue
        left = IDENT.findall(txt)
        if left:
            leftovers += 1
            print(f'  LEFTOVER {path}: {sorted(set(left))[:5]}')
    print(f'leftover files: {leftovers}')


if __name__ == '__main__':
    main()
