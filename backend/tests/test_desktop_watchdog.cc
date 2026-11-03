/* The desktop must never turn an HTTP probe failure into a backend kill. */
#include "../tools/desktop.cc"
#include <cassert>

int main() {
    assert(!backend_restart_required(WAIT_TIMEOUT));
    assert(!backend_restart_required(WAIT_FAILED));
    assert(backend_restart_required(WAIT_OBJECT_0));
    return 0;
}
