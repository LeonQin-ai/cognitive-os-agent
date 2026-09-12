#!/usr/bin/env python3
# var_hoist.py — hoist function-local variable declarations to the top of
# the function body (top-level scope only), keep comments/strings intact,
# and add blank-line separation after top-level logic blocks.
#
# Key invariants:
#  - strip_comments() is LENGTH-PRESERVING (comment bodies and string/char
#    contents blanked, code kept): skeleton offsets map 1:1 to the original
#    text. Analysis uses the skeleton; every extracted SOURCE fragment
#    comes from the ORIGINAL text, and edits are applied to the original.
#  - find_functions() rejects control keywords BOTH by the matched name and
#    by the head text, so `if (...) { ... }` is never treated as a function.
#  - split_top_statements() tracks brace AND paren depth: `for (a;b;c;)`
#    headers and `if (...)` conditions never split statements; a block
#    statement (`if/for/while/switch ... { }`) is ONE span, so declarations
#    inside nested blocks are never touched.
#  - policies:
#      * no initializer / constant initializer  -> hoist whole declaration
#      * scalar with computed initializer       -> split: declare at top,
#        assign in place (leading comment carried with the declaration)
#      * aggregate/const/static/multi-declarator/function-pointer with
#        computed initializer -> SKIP (reported for manual review)
#      * declarations inside preprocessor conditionals -> SKIP
#      * hoisted declarations are inserted after the existing leading
#        declaration run (order preserved), then ONE blank line
#      * blank line after top-level `}` blocks, never doubling existing
#        blanks, never before else/while/do-while/}/# continuations
#  - idempotent: a second run finds nothing to move.
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BACKEND = os.path.dirname(HERE)
SRC = os.path.join(BACKEND, 'src')

CTRL_KW = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'else',
           'do', 'case', 'default'}


def strip_comments(text):
    """Length-preserving skeleton: comment bodies and string/char contents
    become spaces; code and newlines are kept."""
    out = list(text)
    i = 0
    n = len(text)
    in_block = in_line = in_str = in_chr = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if in_block:
            if c == '*' and nxt == '/':
                out[i] = out[i + 1] = ' '
                in_block = False
                i += 2
                continue
            if c != '\n':
                out[i] = ' '
            i += 1
            continue
        if in_line:
            if c == '\n':
                in_line = False
            else:
                out[i] = ' '
            i += 1
            continue
        if in_str or in_chr:
            if c == '\\' and i + 1 < n:
                out[i] = out[i + 1] = ' '
                i += 2
                continue
            if (in_str and c == '"') or (in_chr and c == "'"):
                in_str = in_chr = False
            elif c != '\n':
                out[i] = ' '
            i += 1
            continue
        if c == '/' and nxt == '*':
            out[i] = out[i + 1] = ' '
            in_block = True
            i += 2
            continue
        if c == '/' and nxt == '/':
            out[i] = out[i + 1] = ' '
            in_line = True
            i += 2
            continue
        if c == '"':
            in_str = True
        elif c == "'":
            in_chr = True
        i += 1
    return ''.join(out)


def find_functions(code):
    """Yield (name, body_open, body_close) offsets of function definitions
    in skeleton code. body_open points at '{', body_close at matching '}'."""
    res = []
    for m in re.finditer(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(', code):
        if m.group(1) in CTRL_KW:
            continue
        ls = code.rfind('\n', 0, m.start()) + 1
        head = code[ls:m.start()]
        if ';' in head:
            continue
        if re.match(r'^\s*(if|for|while|switch|return|sizeof|else|do)\b', head):
            continue
        if re.match(r'^\s*#', head):
            continue
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
        j = i + 1
        while j < n and code[j] in ' \t\n':
            j += 1
        if j >= n or code[j] != '{':
            continue
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
        res.append((m.group(1), j, k, code[m.end():i]))
    return res


def split_top_statements(code, open_off, close_off):
    """Split a function body into top-level statement spans. Tracks brace
    and paren depth; `for (a;b;c)` never splits; block statements are one
    span."""
    body_start = open_off + 1
    body_end = close_off
    spans = []
    start = None
    depth = 0
    paren = 0
    i = body_start
    while i < body_end:
        c = code[i]
        if c == '(':
            paren += 1
        elif c == ')':
            paren = max(0, paren - 1)
        if c == '{':
            if depth == 0 and start is None:
                start = i
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0 and start is not None and paren == 0:
                # `= { ... };` aggregate initializer: the `}` must not end
                # the span or the trailing `;` is orphaned. Extend through
                # the `;` when it directly follows (skipping whitespace).
                # paren == 0 guard keeps GCC statement expressions `({...})`
                # intact (their `}` sits inside parens).
                j = i + 1
                while j < body_end and code[j] in ' \t\r\n':
                    j += 1
                if j < body_end and code[j] == ';':
                    spans.append((start, j + 1))
                    start = None
                    i = j
                else:
                    spans.append((start, i + 1))
                    start = None
        elif depth == 0 and paren == 0:
            if c == ';':
                if start is not None:
                    spans.append((start, i + 1))
                    start = None
            elif c not in ' \t\r\n' and start is None:
                start = i
        i += 1
    if start is not None:
        spans.append((start, body_end))
    return spans


def build_type_vocab():
    vocab = set()
    builtins = {'int', 'char', 'long', 'short', 'float', 'double', 'void',
                'unsigned', 'signed', 'size_t', 'ssize_t', 'ptrdiff_t',
                'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t', 'int8_t',
                'int16_t', 'int32_t', 'int64_t', 'bool', 'FILE', 'va_list',
                'time_t', 'off_t', 'pid_t', 'intptr_t', 'uintptr_t',
                'cJSON', 'strbuf', 'strmap'}
    vocab |= builtins
    inc = os.path.join(BACKEND, 'include')
    for root, _dirs, files in os.walk(inc):
        for fn in files:
            if not fn.endswith('.h'):
                continue
            text = strip_comments(open(os.path.join(root, fn),
                              encoding='utf-8', errors='replace').read())
            for m in re.finditer(r'\btypedef\s+(?:struct\s+\w+\s+)?'
                                 r'[A-Za-z_][A-Za-z0-9_ \t\*]*?'
                                 r'[\s\*]([A-Za-z_][A-Za-z0-9_]*)\s*;', text):
                vocab.add(m.group(1))
            for m in re.finditer(r'\btypedef\s+struct\s+([A-Za-z_]\w*)\s*\{', text):
                vocab.add(m.group(1))
    return vocab


TYPE_VOCAB = build_type_vocab()

DECL_RE = re.compile(
    r'^((?:static\s+|const\s+|volatile\s+)*'
    r'((?:struct|enum|union)\s+)?([A-Za-z_][A-Za-z0-9_]*)'
    r'([\s\*]+))'
    r'([A-Za-z_][A-Za-z0-9_]*)'
    r'(\s*\[[^\]]*\])?'
    r'\s*(=[^=].*)?$')

KW_STMT = re.compile(r'^\s*(if|for|while|switch|return|goto|break|continue|'
                     r'do|else|case|default)\b')


def parse_decl(stmt_text):
    """Parse a top-level statement as a single-declarator declaration."""
    s = stmt_text.strip().rstrip(';').rstrip()
    if not s or KW_STMT.match(s):
        return None
    if '(' in s.split('=')[0]:
        return None          # function pointers / calls
    m = DECL_RE.match(s)
    if not m:
        return None
    tag = (m.group(2) or '').strip()
    base = m.group(3)
    if not tag and base not in TYPE_VOCAB:
        return None
    if tag and tag not in ('struct', 'enum', 'union'):
        return None
    ptr = re.sub(r'[\s\t]', '', m.group(4))   # exact '**'/'***' preserved
    return {
        'is_const': 'const' in m.group(1),
        'is_static': 'static' in m.group(1),
        'tag': tag,
        'base': base,
        'ptr': ptr,
        'name': m.group(5),
        'arr': (m.group(6) or '').strip(),
        'init': m.group(7),
    }


def multi_declarator(stmt_text):
    """True when the statement declares more than one declarator.
    A comma at parenthesis/bracket/brace depth 0 separates declarators —
    including commas BETWEEN initializers (`char *a = NULL, *b = NULL;`),
    which sit after the first `=`. Commas inside (), [] or {} (call args,
    array subscripts, aggregate initializers) do not count."""
    s = stmt_text.strip().rstrip(';').rstrip()
    depth = 0
    for ch in s:
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        elif ch == ',' and depth == 0:
            return True
    return False


CONST_INIT_RE = re.compile(
    r'^=\s*(?:NULL|nullptr|true|false|0[xX][0-9a-fA-F]+|\d+(?:\.\d+)?[uUlLfF]*'
    r'|[A-Z_][A-Z0-9_]*(?:\s*\([^()]*\))?'
    r'|sizeof\s*\([^()]*\)'
    r'|\(\s*[A-Za-z_][A-Za-z0-9_ \t\*]*\)\s*(?:0|NULL|\d+)'
    r'|"(?:[^"\\]|\\.)*"'
    r')\s*$')
# NOTE: braced initializers `= {...}` are deliberately NOT "constant" here —
# `{&local, &local}` references other locals. They fall through to
# init_safe_to_hoist(), whose identifier check accepts `{0}` / `{NULL}` /
# `{param, ...}` and rejects anything mentioning another local.


def init_is_constant(init_text):
    """init_text comes from the ORIGINAL text (strings intact)."""
    if init_text is None:
        return True
    return bool(CONST_INIT_RE.match(init_text.strip()))


def param_names(params_text):
    """Last identifier of each parameter declarator."""
    names = set()
    if not params_text or params_text.strip() in ('', 'void'):
        return names
    depth = 0
    cur = []
    parts = []
    for ch in params_text:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append(''.join(cur))
    for p in parts:
        ids = re.findall(r'[A-Za-z_][A-Za-z0-9_]*', p)
        # drop leading type keywords; the last id is the param name
        # (function-pointer params 'void (*cb)(int)' -> 'cb')
        m = re.search(r'\(\s*\*\s*([A-Za-z_]\w*)\s*\)', p)
        if m:
            names.add(m.group(1))
        elif ids:
            names.add(ids[-1])
    return names


SAFE_TOK = {'NULL', 'nullptr', 'true', 'false', 'sizeof'}


def init_safe_to_hoist(skel_init, params):
    """True when evaluating the initializer at function entry is provably
    equivalent: no calls, no member/array access through anything (state
    may have been mutated earlier in the function), and every identifier is
    a parameter, an ALL-CAPS macro, or a literal. skel_init comes from the
    skeleton (string contents blanked)."""
    s = skel_init
    if re.search(r'[A-Za-z_]\w*(->|\.)', s):
        return False           # member access: state may have changed
    if re.search(r'[A-Za-z_]\w*\s*\[', s):
        return False           # array subscript: same reasoning
    if re.search(r'[A-Za-z_]\w*\s*\(', s):
        return False           # function call
    for m in re.finditer(r'[A-Za-z_][A-Za-z0-9_]*', s):
        t = m.group(0)
        if t in SAFE_TOK or t in params:
            continue
        if re.fullmatch(r'[A-Z_][A-Z0-9_]*', t):
            continue           # macro / enum constant
        return False
    return True


def in_pp_conditional(orig_text, body_start, offset):
    depth = 0
    for line in orig_text[body_start:offset].split('\n'):
        st = line.strip()
        if re.match(r'#\s*(if|ifdef|ifndef)\b', st):
            depth += 1
        elif re.match(r'#\s*endif\b', st):
            depth = max(0, depth - 1)
    return depth > 0


def leading_comment(orig, stmt_start):
    """If a comment-only line sits directly above stmt_start, return
    (line_start, text); else (None, '')."""
    ls = orig.rfind('\n', 0, stmt_start) + 1
    if ls == 0:
        return None, ''
    prev_le = orig.rfind('\n', 0, ls - 1) + 1
    prev = orig[prev_le:ls - 1]
    if re.match(r'^\s*(/\*|//)', prev):
        return prev_le, prev.rstrip()
    return None, ''


def transform_function(orig, code, name, open_off, close_off, params_text,
                       rel, report):
    """Hoist declarations of one function; return (new_text, changed)."""
    spans = split_top_statements(code, open_off, close_off)
    if len(spans) < 2:
        return orig, False
    first_logic = None
    for idx, (s, e) in enumerate(spans):
        st = code[s:e]
        if not (parse_decl(st) and not multi_declarator(st)):
            first_logic = idx
            break
    if first_logic is None:
        return orig, False          # all declarations already, no logic
    params = param_names(params_text)

    hoisted = []   # (source_line, leading_comment_or_empty)
    edits = []     # (start, end, replacement) — '' removes the statement
    for idx, (s, e) in enumerate(spans):
        if idx < first_logic:
            continue
        stmt_src = orig[s:e]                  # ORIGINAL text, strings intact
        d = parse_decl(stmt_src)
        if not d or multi_declarator(stmt_src):
            continue
        if not stmt_src.strip().endswith(';'):
            # span got mangled (e.g. multi-declarator aggregate init) —
            # hoisting it would drop the terminating `;`. Manual review.
            report['skipped'].append((rel, name, stmt_src.strip()[:70]))
            continue
        if in_pp_conditional(orig, open_off + 1, s):
            report['pp_guarded'].append((rel, name))
            continue
        if d['init'] is None or init_is_constant(d['init']):
            cs, ct = leading_comment(orig, s)
            hoisted.append((stmt_src.strip(), ct))
            rm_s = cs if cs is not None else s
            edits.append((rm_s, e, ''))
            continue
        # computed initializer: hoisting the WHOLE declaration is safe iff
        # evaluating it at function entry is provably equivalent
        skel_d = parse_decl(code[s:e])
        skel_init = skel_d['init'] if skel_d else ''
        if skel_init and init_safe_to_hoist(skel_init, params):
            cs, ct = leading_comment(orig, s)
            hoisted.append((stmt_src.strip(), ct))
            rm_s = cs if cs is not None else s
            edits.append((rm_s, e, ''))
            continue
        if d['is_const'] or d['is_static'] or d['arr']:
            report['skipped'].append((rel, name, stmt_src.strip()[:70]))
            continue
        decl_src = f"{d['tag'] + ' ' if d['tag'] else ''}{d['base']} {d['ptr']}{d['name']};"
        assign_src = f"{d['name']} {d['init'].strip()};"
        cs, ct = leading_comment(orig, s)
        hoisted.append((decl_src, ct))
        edits.append((s, e, assign_src))
    if not hoisted:
        return orig, False

    # apply statement edits from the end backwards
    for (s, e, rep) in sorted(edits, key=lambda x: -x[0]):
        if rep == '':
            ls = orig.rfind('\n', 0, s) + 1
            le = orig.find('\n', e)
            le = le + 1 if le > 0 else len(orig)
            if orig[ls:s].strip() == '' and not re.search(r'\S', orig[e:le]):
                orig = orig[:ls] + orig[le:]
            else:
                orig = orig[:s] + orig[e:]
        else:
            orig = orig[:s] + rep + orig[e:]

    # insert the hoisted declarations after the leading declaration run
    indent = '    '
    lines = []
    for src, ct in hoisted:
        if ct:
            lines.append(ct.strip())
        lines.append(src)
    block_body = ('\n' + indent).join(lines)
    if first_logic > 0:
        # insert before the first logic statement's line, keep one blank
        ls = orig.rfind('\n', 0, spans[first_logic][0]) + 1
        block = indent + block_body + '\n\n'
        # remove one existing leading blank line, if any, to stay idempotent
        prev_le = orig.rfind('\n', 0, ls - 1) + 1
        if orig[prev_le:ls - 1].strip() == '' and ls - prev_le >= 1:
            ls = prev_le
        orig = orig[:ls] + block + orig[ls:]
    else:
        insert_at = open_off + 1
        nl = orig.find('\n', insert_at, insert_at + 4)
        if nl > 0 and orig[insert_at:nl].strip() == '':
            insert_at = nl + 1
        orig = orig[:insert_at] + indent + block_body + '\n\n' + orig[insert_at:]
    return orig, True


def blank_pass(orig, code, open_off, close_off):
    """Insert a blank line after top-level `}` blocks (function-body scope),
    skipping else/while continuations, the final brace, existing blanks and
    preprocessor lines."""
    edits = []
    depth = 0
    stack = []          # 'init' | 'block' for each open brace
    last_sig = ''       # previous non-whitespace char
    i = open_off
    while i < close_off:
        c = code[i]
        if c == '{':
            depth += 1
            # aggregate-initializer braces (`= {0}`, `, {1}`, nested `{`)
            # are not blocks: no blank line after them
            stack.append('init' if last_sig in ('=', ',', '{') else 'block')
        elif c == '}':
            depth -= 1
            kind = stack.pop() if stack else 'block'
            if depth == 1 and kind == 'block':   # closed a top-level block
                j = i + 1
                while j < close_off and code[j] in ' \t':
                    j += 1
                if j < close_off and code[j] == '\n' and j + 1 < close_off \
                        and code[j + 1] != '\n':
                    p = j + 1
                    while p < close_off and code[p] in ' \t\r':
                        p += 1
                    if p < close_off:
                        ch = code[p]
                        rest = code[p:close_off]
                        if ch not in ('}', '#') and \
                                not re.match(r'(else|while)\b', rest):
                            edits.append(j + 1)
        if c not in ' \t\r\n':
            last_sig = c
        i += 1
    for pos in sorted(set(edits), reverse=True):
        orig = orig[:pos] + '\n' + orig[pos:]
    return orig


def transform_file(path, rel, report):
    orig = open(path, encoding='utf-8', errors='replace').read()
    changed = False
    remaining = None
    while True:
        code = strip_comments(orig)
        funcs = find_functions(code)
        if remaining is None:
            remaining = len(funcs)
        if remaining == 0:
            break
        remaining -= 1
        name, j, k, params_text = funcs[remaining]
        orig, ch = transform_function(orig, code, name, j, k, params_text,
                                      rel, report)
        if ch:
            changed = True
    while True:
        code = strip_comments(orig)
        funcs = find_functions(code)
        did = False
        for (name, j, k, _params) in reversed(funcs):
            new = blank_pass(orig, code, j, k)
            if new != orig:
                orig = new
                changed = True
                did = True
                break
        if not did:
            break
    if changed:
        open(path, 'w', encoding='utf-8', newline='').write(orig)
        report['changed'].append(rel)
    return changed


def main():
    report = {'changed': [], 'skipped': [], 'pp_guarded': []}
    if len(sys.argv) > 1:
        p = os.path.join(BACKEND, sys.argv[1])
        transform_file(p, sys.argv[1], report)
    else:
        for root, _dirs, files in os.walk(SRC):
            if 'third_party' in root:
                continue
            for fn in sorted(files):
                if not fn.endswith('.c'):
                    continue
                p = os.path.join(root, fn)
                rel = os.path.relpath(p, BACKEND).replace('\\', '/')
                transform_file(p, rel, report)
    print('changed files:', len(report['changed']))
    for r in report['changed']:
        print('  M', r)
    print('skipped (manual review):', len(report['skipped']))
    for r in report['skipped'][:40]:
        print('  ?', r)
    print('pp-guarded (skipped):', len(report['pp_guarded']))


if __name__ == '__main__':
    main()
