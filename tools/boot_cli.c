/* Headless runner: the same C sources as the APK, no phone needed.  Usage: boot_cli <ipsw.zip> [max_million_steps]
 * Runs the experimental boot attempt exactly like the app does (a64_attempt_ipsw + a64m_run) and prints the whole log
 * (boot log, kernel output, stop report) to stdout.  Nothing is faked: same stop reasons as on the phone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "attempt.h"
static int file_read(void *ud, uint64_t off, void *buf, size_t len) {
    FILE *f = ud; if (fseeko(f, (off_t)off, SEEK_SET)) return -1; return fread(buf, 1, len, f) == len ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s ipsw.zip [max_million_steps]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 2; }
    fseeko(f, 0, SEEK_END); uint64_t sz = (uint64_t)ftello(f);
    uint64_t maxm = argc > 2 ? strtoull(argv[2], 0, 10) : 4000;
    A64Machine *m = a64_attempt_ipsw(file_read, f, sz); if (!m) { fprintf(stderr, "out of memory\n"); return 2; }
    uint64_t steps = 0; A64MState st;
    while ((st = a64m_run(m, 200000, false)) == A64M_RUNNING) { steps += 200000; if (steps > maxm * 1000000ull) { fprintf(stderr, "step limit reached\n"); break; } }
    size_t off = 0, n; char *b = malloc(65536);
    while ((n = a64m_log_read(m, off, b, 65536)) > 0) { fwrite(b, 1, n, stdout); off += n; }
    fprintf(stderr, "final state %d, log %zu bytes\n", (int)st, off);
    return 0;
}
