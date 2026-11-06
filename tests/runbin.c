/* Host runner: loads a flat AArch64 binary at 0x10000 into user-mode guest RAM and runs it.
 * Guest stdout -> our stdout. Exit code = guest exit code (or 99 on emulator fault). */
#include "../app/src/main/cpp/arm64.h"
#include <stdio.h>
#include <stdlib.h>
static void out(void *u, const char *b, size_t n) { (void)u; fwrite(b, 1, n, stdout); }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    size_t sz = 8u << 20; uint8_t *mem = calloc(1, sz);
    size_t n = fread(mem + 0x10000, 1, sz - 0x10000, f); fclose(f);
    A64 *c = calloc(1, sizeof *c);
    a64_init(c, mem, 0, sz); c->out = out; c->pc = 0x10000;
    int r = a64_run(c, 4000000000ull);
    fflush(stdout);
    if (r != 0) { fprintf(stderr, "rc=%d %s (steps=%llu, %zu bytes)\n", r, c->err, (unsigned long long)c->steps, n); return 99; }
    fprintf(stderr, "steps=%llu exit=%d\n", (unsigned long long)c->steps, c->exit_code);
    return c->exit_code;
}
