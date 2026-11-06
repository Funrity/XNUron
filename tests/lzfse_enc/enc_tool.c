/* test helper: lzfse_enc <in> <out>  (uses Apple's reference encoder, BSD licence) */
#include <stdio.h>
#include <stdlib.h>
#include "lzfse.h"
int main(int c, char **v) {
    FILE *f = fopen(v[1], "rb"); if (!f) return 1; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *in = malloc(n), *out = malloc(n * 2 + 4096); if (fread(in, 1, n, f) != (size_t)n) return 1; fclose(f);
    size_t k = lzfse_encode_buffer(out, n * 2 + 4096, in, n, NULL); if (!k) return 2;
    f = fopen(v[2], "wb"); fwrite(out, 1, k, f); fclose(f); return 0;
}
