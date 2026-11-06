/*
 * Image inspection.  Identifies what a user-supplied file is and states, per
 * finding, whether and why XNUron can or cannot run it.  Nothing here decrypts,
 * downloads, patches or bypasses any protection.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    A64_KIND_UNKNOWN = 0, A64_KIND_MACHO, A64_KIND_IM4P, A64_KIND_IMG4, A64_KIND_ZIP, A64_KIND_IPSW,
    A64_KIND_LZFSE, A64_KIND_DMG, A64_KIND_RAW_A64, A64_KIND_MACHO32, A64_KIND_APPLE_CONTAINER
} A64Kind;

typedef struct {
    A64Kind kind;
    bool runnable_user;       /* a user-mode Mach-O the HLE machine can execute */
    bool bootable_system;     /* always false for Apple images today; kept explicit */
} A64Inspect;

typedef int (*A64ReadAt)(void *ud, uint64_t off, void *buf, size_t len);   /* 0 = ok */

/* Inspect a whole in-memory file. Returns the report length. */
size_t a64_inspect_buffer(const uint8_t *d, size_t n, A64Inspect *res, char *rep, size_t cap);
/* Inspect a (possibly huge) file through random-access reads; used for IPSW zip archives. */
size_t a64_inspect_source(A64ReadAt rd, void *ud, uint64_t total, A64Inspect *res, char *rep, size_t cap);

/* DER helpers exposed for tests. */
size_t a64_img4_describe(const uint8_t *d, size_t n, A64Inspect *res, char *rep, size_t cap);
size_t a64_zip_describe(A64ReadAt rd, void *ud, uint64_t total, A64Inspect *res, char *rep, size_t cap);

/* IM4P payload extraction (no decryption).  data points into the input buffer. */
typedef struct { char type[5]; const uint8_t *data; size_t len; bool has_kbag, has_im4m; } A64Im4p;
int a64_im4p_unwrap(const uint8_t *d, size_t n, A64Im4p *out);   /* 0 ok */

#ifdef __cplusplus
}
#endif
