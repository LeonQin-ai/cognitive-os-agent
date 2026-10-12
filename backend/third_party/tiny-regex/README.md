# tiny-regex-c

Source: https://github.com/kokke/tiny-regex-c
Revision: f2632c6d9ed25272987471cdb8b70395c2460bdb
License: Unlicense (see LICENSE).

Local changes: C11 thread-local compilation buffers, unsigned-char ctype inputs.
Only trusted, fixed expressions are used for SSH prompt extraction; this does
not expose a general regular-expression tool to model input.
