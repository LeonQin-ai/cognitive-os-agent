/* macos/http.c — macOS HTTP backend.
 * The plain-HTTP path, the libcurl dispatch and the socket/fs/proc backends
 * are shared POSIX code (src/os/posix/). The only macOS difference is the
 * libcurl dylib name: /usr/lib/libcurl.dylib. That name is already in the
 * shared dlopen candidate list in ../linux/http.c (dlopen of a nonexistent
 * name just fails harmlessly on the other platforms), so this file simply
 * reuses the Linux implementation. */
#include "../linux/http.c"
