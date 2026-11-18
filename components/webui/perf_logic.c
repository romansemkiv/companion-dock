#include "perf_logic.h"

int perf_share_permille(const perf_sample_t *prev, size_t n_prev, bool have_prev, uintptr_t id, uint64_t run,
                        uint64_t dt_us)
{
    if (!have_prev || dt_us == 0) return -1;
    uint64_t base = 0;
    for (size_t i = 0; i < n_prev; i++) {
        if (prev[i].id == id) {
            base = prev[i].run <= run ? prev[i].run : 0;
            break;
        }
    }
    uint64_t d = run - base;
    uint64_t pm = d >= dt_us ? 1000 : d * 1000 / dt_us;
    return (int)pm;
}
