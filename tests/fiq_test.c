#include "../app/src/main/cpp/machine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 2) { printf("FAIL: no fiq binary\n"); return 1; }
    FILE *f = fopen(argv[1], "rb"); uint8_t buf[16384]; size_t n = f ? fread(buf, 1, sizeof buf, f) : 0; if (f) fclose(f);
    A64Machine *m = a64m_create_system(16u << 20);
    a64m_load_raw(m, buf, n, 0);
    A64MState st = a64m_run(m, 2000000, false);
    int ok = n > 0 && st == A64M_POWEROFF && a64m_exit_code(m) == 0;
    printf("%s: CNTP FIQ (state=%d exit=%d)\n", ok ? "  ok" : "  FAIL", (int)st, a64m_exit_code(m));
    a64m_destroy(m);
    return ok ? 0 : 1;
}
