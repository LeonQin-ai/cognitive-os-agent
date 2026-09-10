# hw_audit.py — Huawei C coding standard (DKBA 2826-2011.5) violation audit
# Read-only scan of src/ + include/. Outputs a per-file, per-rule worklist.
import os
import re
import sys
import json

HERE = os.path.dirname(os.path.abspath(__file__))
BACKEND = os.path.dirname(HERE)
SRC = os.path.join(BACKEND, 'src')
INC = os.path.join(BACKEND, 'include')

report = {'files': {}, 'summary': {}}


def strip_comments(text):
    # keep line structure: replace comment bodies with spaces (newlines kept)
    out = []
    i = 0
    n = len(text)
    in_block = False
    in_line = False
    in_str = False
    in_chr = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if in_block:
            if c == '*' and nxt == '/':
                in_block = False
                out.append('  ')
                i += 2
                continue
            out.append('\n' if c == '\n' else ' ')
            i += 1
            continue
        if in_line:
            if c == '\n':
                in_line = False
                out.append(c)
            else:
                out.append(' ')
            i += 1
            continue
        if in_str:
            out.append(c)
            if c == '\\':
                if i + 1 < n:
                    out.append(nxt)
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if in_chr:
            out.append(c)
            if c == '\\':
                if i + 1 < n:
                    out.append(nxt)
                i += 2
                continue
            if c == "'":
                in_chr = False
            i += 1
            continue
        if c == '/' and nxt == '*':
            in_block = True
            out.append('  ')
            i += 2
            continue
        if c == '/' and nxt == '/':
            in_line = True
            out.append('  ')
            i += 2
            continue
        if c == '"':
            in_str = True
        if c == "'":
            in_chr = True
        out.append(c)
        i += 1
    return ''.join(out)


def line_map(text):
    # map offset -> line number (1-based)
    lines = [0]
    for i, ch in enumerate(text):
        if ch == '\n':
            lines.append(i + 1)
    return lines


def off_line(lines, off):
    lo, hi = 0, len(lines) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if lines[mid] <= off:
            lo = mid
        else:
            hi = mid - 1
    return lo + 1


FUNC_RE = re.compile(
    r'^(?:[A-Za-z_][A-Za-z0-9_ \t\*]*?[\s\*])?([A-Za-z_][A-Za-z0-9_]*)\s*\(([^;{)]*)\)\s*\{',
    re.M)
# crude but effective for this codebase: function definition = identifier(...) { at column 0-ish


def find_functions(code):
    """Yield (name, body_start_off, body_end_off, params_text, is_static)."""
    res = []
    for m in re.finditer(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', code):
        # walk back to get the return type start (beginning of the statement line)
        ls = code.rfind('\n', 0, m.start()) + 1
        head = code[ls:m.start()]
        if ';' in head:
            continue
        if re.match(r'^\s*(if|for|while|switch|return|sizeof|else)\b', head):
            continue
        if re.match(r'^\s*\}', head):
            continue
        # find matching close paren
        depth = 0
        i = m.end() - 1
        n = len(code)
        while i < n:
            if code[i] == '(':
                depth += 1
            elif code[i] == ')':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if i >= n:
            continue
        params = code[m.end():i]
        # find the opening brace of the body
        j = i + 1
        while j < n and code[j] in ' \t\n':
            j += 1
        if j >= n or code[j] != '{':
            continue
        # find matching brace
        depth = 0
        k = j
        while k < n:
            if code[k] == '{':
                depth += 1
            elif code[k] == '}':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        if k >= n:
            continue
        name = m.group(1)
        is_static = bool(re.search(r'\bstatic\b', head))
        res.append((name, j, k, params, is_static, ls))
    return res


def count_params(ptext):
    if not ptext.strip():
        return 0
    if ptext.strip() == 'void':
        return 0
    depth = 0
    cnt = 1
    for ch in ptext:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif ch == ',' and depth == 0:
            cnt += 1
    return cnt


def max_nesting(code, body_start, body_end):
    depth = 0
    mx = 0
    for i in range(body_start, body_end):
        c = code[i]
        if c == '{':
            depth += 1
            if depth > mx:
                mx = depth
        elif c == '}':
            depth -= 1
    return max(0, mx - 1)


def code_lines(code, start, end):
    seg = code[start:end]
    n = 0
    for ln in seg.split('\n'):
        if ln.strip():
            n += 1
    return n


MAGIC_CTX = re.compile(r'(==|!=|>=|<=|[=+\-*/%<>]|\bif\s*\(|\bcase\s+)\s*(-?\d+)\b')


def audit_c(path, rel):
    text = open(path, encoding='utf-8', errors='replace').read()
    code = strip_comments(text)
    lines = line_map(code)
    f = {'long_funcs': [], 'deep_funcs': [], 'many_args': [],
         'switch_no_default': [], 'magic_numbers': [], 'statics_no_s': [],
         'globals_no_g': [], 'extern_in_c': 0}
    funcs = find_functions(code)
    for (name, bs, be, params, is_static, head_start) in funcs:
        nlines = code_lines(code, bs, be)
        if nlines > 50:
            f['long_funcs'].append((name, nlines, off_line(lines, head_start)))
        nest = max_nesting(code, bs, be)
        if nest > 4:
            f['deep_funcs'].append((name, nest, off_line(lines, head_start)))
        np = count_params(params)
        if np > 5:
            f['many_args'].append((name, np, off_line(lines, head_start)))
    # switch without default (rule 6.5)
    for m in re.finditer(r'\bswitch\s*\(', code):
        i = code.find('{', m.end())
        if i < 0:
            continue
        depth = 0
        k = i
        while k < len(code):
            if code[k] == '{':
                depth += 1
            elif code[k] == '}':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        body = code[i:k]
        if not re.search(r'\bdefault\s*:', body):
            f['switch_no_default'].append(off_line(lines, m.start()))
    # magic numbers (rule 5.4) — literals outside defines/const contexts, heuristic
    for m in MAGIC_CTX.finditer(code):
        v = m.group(2)
        if v in ('0', '1'):
            continue
        ln = off_line(lines, m.start())
        line = code[lines[ln - 1]:lines[ln] if ln < len(lines) else None]
        if '#define' in line or 'const' in line:
            continue
        f['magic_numbers'].append((v, ln))
    # statics without s_ (rule 3.3), globals without g_ (rule 3.2)
    for m in re.finditer(r'^\s*static\s+(?:const\s+)?[A-Za-z_][A-Za-z0-9_ \t\*]*?[\s\*]([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|;|\[)', code, re.M):
        name = m.group(1)
        if name.startswith('s_') or name.startswith('S_'):
            continue
        if re.match(r'^[a-z_]+[A-Za-z0-9_]*$', name) and '(' in code[m.end():m.end() + 2]:
            continue
        f['statics_no_s'].append((name, off_line(lines, m.start())))
    for m in re.finditer(r'^[A-Za-z_][A-Za-z0-9_ \t\*]*?[\s\*]([A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^=]|\[(?!]|$);)', code, re.M):
        name = m.group(1)
        head = m.group(0)
        if 'static' in head or 'extern' in head or 'const' in head.split(';')[0].split('=')[0]:
            continue
        if '(' in head:
            continue
        if name.startswith('g_'):
            continue
        f['globals_no_g'].append((name, off_line(lines, m.start())))
    # extern declarations in .c (rule 1.7)
    f['extern_in_c'] = len(re.findall(r'^\s*extern\s+', code, re.M))
    if any(f[k] for k in f):
        report['files'][rel] = f


def audit_h(path, rel):
    text = open(path, encoding='utf-8', errors='replace').read()
    code = strip_comments(text)
    lines = line_map(code)
    f = {}
    # rule 1.6: variable definitions in headers (non-extern)
    bad = []
    for m in re.finditer(r'^[A-Za-z_][A-Za-z0-9_ \t\*]*?[\s\*]([A-Za-z_][A-Za-z0-9_]*)\s*=[^=]', code, re.M):
        head = m.group(0)
        if 'extern' in head or 'static' in head or 'inline' in head or '#define' in head:
            continue
        if '(' in head:
            continue
        bad.append((m.group(1), off_line(lines, m.start())))
    if bad:
        f['var_def_in_h'] = bad
    # rule 1.5: include guard
    if '#ifndef' not in code and '#pragma once' not in code:
        f['no_include_guard'] = True
    if f:
        report['files'][rel] = f


def main():
    for root, dirs, files in os.walk(SRC):
        for fn in sorted(files):
            if not fn.endswith('.c'):
                continue
            p = os.path.join(root, fn)
            rel = os.path.relpath(p, HERE).replace('\\', '/')
            audit_c(p, rel)
    for root, dirs, files in os.walk(INC):
        for fn in sorted(files):
            if not fn.endswith('.h'):
                continue
            p = os.path.join(root, fn)
            rel = os.path.relpath(p, HERE).replace('\\', '/')
            audit_h(p, rel)
    # summary
    tot = {}
    for rel, f in report['files'].items():
        for k, v in f.items():
            if isinstance(v, list):
                tot[k] = tot.get(k, 0) + len(v)
            else:
                tot[k] = tot.get(k, 0) + 1
    report['summary'] = tot
    out = os.path.join(BACKEND, 'hw_audit.json')
    with open(out, 'w', encoding='utf-8') as fp:
        json.dump(report, fp, indent=1, ensure_ascii=False)
    print(json.dumps(tot, indent=1, ensure_ascii=False))
    print('files with violations:', len(report['files']))
    print('full report:', out)


if __name__ == '__main__':
    main()
