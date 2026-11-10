/*
 * Experimental "try to boot an IPSW" pipeline.  It performs every step that can be done without
 * Apple keys and then really executes whatever it obtained, so the person sees the genuine
 * outcome (usually an emulator fault very early).  Every step is written to the boot log.
 * It never decrypts, never patches, never skips signature checks and never reports success
 * it did not observe.
 */
#pragma once
#include "machine.h"
#include "inspect.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Always returns a machine (NULL only on out-of-memory).  If a step cannot be completed the
 * machine is already in the FAULT state with the reason in its log. */
A64Machine *a64_attempt_ipsw(A64ReadAt rd, void *ud, uint64_t total);

/* Pieces, exposed for tests. */
/* Decompresses LZFSE ("bvx*") or Apple LZSS ("complzss") into a malloc'ed buffer; NULL + err on failure. */
uint8_t *a64_unpack_payload(const uint8_t *d, size_t n, size_t *out_len, char *err, size_t errcap);
/* Same decompression, but for an already-unwrapped raw LZSS stream (no 0x180 header). */
size_t a64_lzss_decode(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstcap);

/* iBoot-style placeholder-flag fix-up of an Apple device tree; returns flagged property count, -1 if malformed (untouched). */
/* MODELLED iBoot step: add a uint32 property to /chosen if missing. 1 added, 0 present, -1 malformed or no room (cap = size of the buffer). */
int a64_dt_chosen_add_u32(uint8_t *d, size_t *n, size_t cap, const char *name, uint32_t val);
int a64_dt_fixup(uint8_t *d, size_t n, char *first_name /* >=33 bytes or NULL */);

/* Marks the first /cpus child with a 'state' property as 'running'; returns 1 (and its reg), 0 none, -1 malformed. */
int a64_dt_boot_cpu(uint8_t *d, size_t n, uint32_t *reg, char *log, size_t logcap);

/* Fills all-zero /chosen random-seed and entropy-data properties using rng; returns count filled, -1 if malformed. */
int a64_dt_find_pa(uint8_t *d, size_t n, uint64_t want, char *name, size_t ncap, char *compat, size_t ccap, uint64_t *base, uint64_t *size);   /* /arm-io node whose reg covers a physical address */
int a64_dt_pmgr_optional_bridges(uint8_t *d, size_t n, uint32_t *nbr, uint32_t *oldm, uint32_t *newm, uint32_t *nmiss);   /* mark bridges without bridge-settings-<i> as optional in the 'pmgr' node, in place */
int a64_dt_node_add_empty_prop(uint8_t *d, size_t *n, size_t cap, const char *node, const char *prop);   /* add a zero-length property to the first node with this name; 1 added, 0 exists, -1 no room, -2 no node */
int a64_dt_node_add_prop(uint8_t *d, size_t *n, size_t cap, const char *node, const char *prop, const void *data, size_t len);   /* 0.4.82: add a property with data (NULL = zeros); 1 added, 0 exists, -1 no room, -2 no node */
int a64_dt_node_fill_empty_prop(uint8_t *d, size_t *n, size_t cap, const char *node, const char *prop, const void *data, size_t len);   /* 0.4.85: fill an existing zero-length property; 1 filled, 0 absent/not empty, -1 no room, -2 no node */
int a64_dt_node_add_zero_prop(uint8_t *d, size_t *n, size_t cap, const char *node, const char *prop, size_t len);   /* 0.4.74: add a zero-filled property of len bytes; 1 added, 0 exists, -1 no room, -2 no node */
int a64_dt_patch_u64x2(uint8_t *d, size_t n, const char *prop, uint64_t a, uint64_t b);   /* 0.4.122 */
int a64_plist_first_path(const char *p, size_t n, const char *key, char *out, size_t cap);   /* 0.4.122 */
int a64_attempt_chain(int i, uint64_t *fn, uint64_t *site, int *depth);   /* 0.4.159 diagnostic: callers chain of the code that references '/sbin/launchd' */
int a64_attempt_bsdinit(int chain_i, int j, uint64_t *site, uint64_t *target);   /* 0.4.160 diagnostic */
const char *a64_attempt_symbolize(uint64_t va, uint64_t *off);   /* 0.4.170: name of the kernelcache symbol at or below va (NULL if none) */
int a64_dt_node_prop_words(uint8_t *d, size_t n, const char *node, const char *prop, char *out, size_t cap);   /* 0.4.69: property data as hex u32 words */
int a64_dt_node_info(uint8_t *d, size_t n, const char *node, const char *prop, int64_t *plen, char *log, size_t logcap);   /* property length of the first node with this name + all its property names */
int a64_dt_soc_nodes(uint8_t *d, size_t n, A64SocNode *out, int max);   /* every /arm-io node with a reg: name, compatible, physical range */
int a64_dt_uarts(uint8_t *d, size_t n, uint64_t *pa, uint64_t *sz, char *log, size_t logcap);   /* UART nodes under /arm-io: physical addresses; -1 malformed */
int a64_dt_entropy(uint8_t *d, size_t n, int (*rng)(void *, size_t), size_t *bytes, char *log, size_t logcap);

#ifdef __cplusplus
}
#endif
