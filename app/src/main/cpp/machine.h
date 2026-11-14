/*
 * a64virt - the virtual machine XNUron emulates.  It is a plain, documented
 * AArch64 platform, NOT an Apple SoC.  Memory map:
 *
 *   0x09000000  UART   PL011-compatible subset: DR (+0x00, write = byte to boot log),
 *                      FR (+0x18) always reports TX empty / RX empty
 *   0x0A000000  FB     +0x00 WIDTH(ro) +0x04 HEIGHT(ro) +0x08 ADDR_LO +0x0C ADDR_HI
 *                      +0x10 ENABLE +0x14 FRAME(ro, flip counter) +0x18 FLIP(wo)
 *                      +0x1C FORMAT(ro: 1 = 32-bit XRGB8888, little endian)
 *   0x0A001000  TOUCH  +0x00 X +0x04 Y +0x08 STATE(bit0 = finger down) +0x0C SEQ
 *                      +0x10 CTRL(bit0 = IRQ enable) +0x14 STATUS(bit0 pending, write 1 to clear)
 *   0x0A002000  SYSCON write 0 = power off (exit code 0), non-zero = power off with that code
 *   0x40000000  RAM
 * The CPU's generic virtual timer (CNTV_*) is the other interrupt source.
 * Both interrupt sources share the single IRQ line; the guest tells them apart
 * through CNTV_CTL.ISTATUS and TOUCH.STATUS.
 *
 * Boot protocol: raw binary loaded at RAM base, PC = RAM base, EL1h, MMU off,
 * IRQ/FIQ/SError/Debug masked, x0 = boot argument, x1..x30 = 0.  No device tree.
 */
#pragma once
#include "arm64.h"
#ifdef __cplusplus
extern "C" {
#endif

#define A64M_RAM_BASE   0x40000000ull
#define A64M_IOS_RAM_BASE 0x800000000ull   /* 0.4.311 ASSUMPTION: DRAM base of the real A12 (0x800000000) used for the iOS boot attempt; 0.4.310 showed the SIO DART bypasses its allowed-physical-range check only for addresses >= 2^35, so RAM at 0x40000000 made AppleS8000AES's DART mapping fail (-2) */
#define A64M_UART_BASE  0x09000000ull
#define A64M_FB_BASE    0x0A000000ull
#define A64M_TOUCH_BASE 0x0A001000ull
#define A64M_SYSCON     0x0A002000ull
#define A64M_FB_W 270
#define A64M_FB_H 480
/* 0.4.51: size of the IPSW-attempt framebuffer (MODELLED) and of the pixel buffer the app uses; the demo display (270x480) is shown 2x */
#define A64M_VID_W 540
#define A64M_VID_H 960
/* 0.4.85: the size of the IPSW attempt's framebuffer is a setting; A64M_VID_W/H are the defaults */
void a64m_set_video_size(int w, int h);   /* clamped to 128..2400, even; applies to the next attempt */
int a64m_video_w(void);
int a64m_video_h(void);

typedef struct A64Machine A64Machine;
typedef struct { char name[28]; char compat[28]; uint64_t pa, size; } A64SocNode;   /* a DeviceTree node under /arm-io with its physical range (diagnostics only) */
void a64m_set_soc_nodes(A64Machine *m, const A64SocNode *nodes, int n);

typedef enum { A64M_KIND_SYSTEM = 0, A64M_KIND_USER = 1 } A64MachineKind;
typedef enum { A64M_CREATED, A64M_RUNNING, A64M_IDLE, A64M_POWEROFF, A64M_FAULT, A64M_STOPPED } A64MState;

/* System machine (a64virt). ram_size is rounded to 64 KiB. NULL on failure. */
A64Machine *a64m_create_system(uint64_t ram_size);
A64Machine *a64m_create_system_at(uint64_t ram_size, uint64_t base);
/* User-mode machine: guest RAM [base, base+size), HLE syscalls, output -> log. */
A64Machine *a64m_create_user(uint64_t base, uint64_t size);
void a64m_destroy(A64Machine *m);

A64 *a64m_cpu(A64Machine *m);
/* Value of x0 at boot (a64virt: bit0 = guest stays interactive after its self-test). */
void a64m_set_boot_arg(A64Machine *m, uint64_t v);
/* 0.4.72: at a stop, find `text` in the kernel image, find the code that references it and dump `before`/`after` bytes of code around the first reference (up to 2 texts). */
void a64m_nm_add_dt_name(A64Machine *m, const char *name);   /* 0.4.79: DeviceTree property name known to exist; turns the missing-name probe on */
void a64m_set_diag_string(A64Machine *m, unsigned idx, const char *text, uint64_t before, uint64_t after);
void a64m_set_diag_code(A64Machine *m, uint64_t va, uint64_t len);
void a64m_set_quiet_exc(A64Machine *m, int q);   /* skip the data-access/instruction lists of the first exception (log budget) */
void a64m_set_block_hist(A64Machine *m, uint64_t pa, uint64_t len);   /* diagnostic: history (first 40, coalesced) of stores/first-word loads to a physical block, printed in the panic report */
unsigned a64m_block_hist_count(A64Machine *m);
void a64m_set_arg_watch(A64Machine *m, uint64_t v);   /* diagnostic: calls (BL/BLR) with x0 or x1 == v, returns with x0 == v (first 48) */
unsigned a64m_arg_watch_count(A64Machine *m);
void a64m_set_value_watch(A64Machine *m, uint64_t v);   /* record the first 8 stores of this 64-bit value (or its low half as a 32-bit store), wherever they go */
void a64m_set_store_watch2(A64Machine *m, int i, uint64_t pa, uint64_t len);
int a64m_add_unknown_mmio_named(A64Machine *m, uint64_t pa, uint64_t size, const char *label);   /* same, the log names the block */
int a64m_unknown_set_readback(A64Machine *m, uint64_t pa);
int a64m_unknown_set_psmirror(A64Machine *m, uint64_t pa);
int a64m_unknown_set_modelink(A64Machine *m, uint64_t pa, uint64_t w_off, uint64_t r_off);   /* 0.4.135 */
int a64m_unknown_lastreads(A64Machine *m, char *out, size_t cap);
int a64m_unknown_ts_window(A64Machine *m, uint64_t block_pa, uint64_t base_off);   /* 0.4.150 range of one temperature sensor inside a block (trace only) */
int a64m_unknown_ts_read(A64Machine *m, uint64_t block_pa, uint64_t off, uint64_t val);   /* 0.4.150 MODELLED fixed read of a sensor register */
const char *a64m_tempsensor_trace(void);
const char *a64m_i2c_trace(void);
int a64m_unknown_rw(A64Machine *m, uint64_t pa, int w, uint64_t off, uint64_t v, uint64_t *rv);   /* 0.4.218, for tests */   /* 0.4.217 diagnostic */   /* 0.4.147 diagnostic: first accesses to the temperature sensor windows */
int a64m_unknown_set_read(A64Machine *m, uint64_t pa, uint64_t off, uint64_t val);   /* MODELLED fixed read value at an offset of an unknown block */
int a64m_add_unknown_mmio_auto(A64Machine *m, uint64_t pa, uint64_t size, const char *label);   /* same, few accesses logged per block */
const char *a64m_aic_writes(A64Machine *m);   /* 0.4.235 diagnostic: distinct written AIC offsets: +off:count|OR-of-values */
int a64m_aic_raise(A64Machine *m, unsigned irq);   /* 0.4.234 MODELLED minimal AIC: a device raises interrupt irq (pending until unmasked) */
int a64m_unknown_summary(A64Machine *m, char *out, size_t cap);   /* touched blocks: name reads/writes last-offset */
int a64m_add_unknown_mmio(A64Machine *m, uint64_t pa, uint64_t size);   /* MODELLED: reads 0, writes ignored, accesses logged; behaviour of the real block is UNKNOWN */
int a64m_add_s5l_uart(A64Machine *m, uint64_t pa, uint64_t size);   /* MODELLED Apple S5L UART at a physical address taken from the DeviceTree */
void a64m_set_diag_data2(A64Machine *m, unsigned idx, uint64_t va, uint64_t len);   /* up to 3 windows */
void a64m_set_pc_probe2(A64Machine *m, uint64_t pc, unsigned reg, uint64_t val);   /* second probe; dumps code at the (PAC-stripped) value of x8 */
void a64m_set_pc_probe(A64Machine *m, uint64_t pc, unsigned reg, uint64_t val);   /* diagnostic: capture registers the first time pc is reached with x[reg] == val */
void a64m_set_diag_table(A64Machine *m, uint64_t va, uint64_t stride, uint64_t n);   /* diagnostic: print a table of structures with their text pointers */
void a64m_set_diag_data(A64Machine *m, uint64_t va, uint64_t len);   /* diagnostic: dump guest memory (kernel virtual) in the stop report */
void a64m_set_coverage(A64Machine *m, uint64_t base, uint64_t len);   /* diagnostic: which instruction words ran */
void a64m_set_ref_scan(A64Machine *m, unsigned idx, uint64_t va);   /* diagnostic: list the code that references a kernel global */
void a64m_set_caller_scan(A64Machine *m, uint64_t va);   /* diagnostic: who calls the function containing va */
void a64m_set_pc_trace(A64Machine *m, uint64_t va);   /* diagnostic: record the 400 pcs from the first execution of va on */
void a64m_set_call_trace(A64Machine *m, uint64_t va);
void a64m_set_call_trace_regs(A64Machine *m, uint64_t va);   /* ring of the last 40 executions of one instruction: x0 x8 x9 x10 x24 x25 x26 x27 */
void a64m_set_dt_phys(A64Machine *m, uint64_t pa);
void a64m_set_store_watch(A64Machine *m, uint64_t pa, uint64_t len);
void a64m_set_mpidr(A64Machine *m, uint64_t v);
/* Marks the machine as running an unknown raw image (adds a hint to fault reports). */
void a64m_mark_raw(A64Machine *m);
/* Copies a raw image into RAM at guest address addr (0 -> RAM base); sets PC there. */
int a64m_load_raw(A64Machine *m, const uint8_t *data, size_t len, uint64_t addr);
/* IPSW boot attempt support: remember the kernel's virtual/physical base so fault reports can
 * show kernel virtual addresses; a64m_abort ends a machine that cannot even start executing. */
void a64m_mark_attempt(A64Machine *m, uint64_t virt_base, uint64_t phys_base);
void a64m_abort(A64Machine *m, const char *why);
void a64m_set_pc(A64Machine *m, uint64_t pc);
/* 0.4.66: mirror new text of the kernel's message buffer (msgbuf) into the guest log; finds the buffer by its magic in RAM. */
void a64m_msgbuf_sync(A64Machine *m);
uint8_t *a64m_ram_at(A64Machine *m, uint64_t guest_addr, uint64_t len);

/* Runs up to max_steps instructions (or until idle/halt/fault/stop).  When the guest
 * idles (WFI) and realtime is true the call sleeps (<= 5 ms) and advances virtual time
 * accordingly; otherwise virtual time is fast-forwarded (deterministic, for tests). */
A64MState a64m_run(A64Machine *m, uint64_t max_steps, bool realtime);
A64MState a64m_state(const A64Machine *m);
void a64m_request_stop(A64Machine *m);     /* thread-safe */
int  a64m_exit_code(const A64Machine *m);

/* Boot log / diagnostics (thread-safe).  Returns bytes copied starting at offset. */
size_t a64m_log_read(A64Machine *m, size_t offset, char *buf, size_t cap);
size_t a64m_log_size(A64Machine *m);
/* kind 0 = everything in order, 1 = only what the guest wrote (UART / HLE output), 2 = only emulator messages. */
void a64m_log_collapse(A64Machine *m, bool on);   /* 0.4.137: collapse identical consecutive guest lines into '[last line repeated N more times]' (attempt machines do) */
void a64m_log_flush(A64Machine *m);                /* 0.4.137: write out the pending guest line / repeat note (end of a run) */
size_t a64m_log_read_kind(A64Machine *m, int kind, size_t offset, char *buf, size_t cap);
void a64m_log(A64Machine *m, const char *fmt, ...);

/* Input / display (thread-safe, may race benignly with the emulation thread). */
void a64m_touch(A64Machine *m, int x, int y, bool down);
/* Copies the guest framebuffer as 0xAARRGGBB; returns frame counter, or -1 if the guest
 * has not enabled the display. */
int64_t a64m_frame(A64Machine *m, uint32_t *dst, int w, int h);
/* IPSW attempt: describe the linear 32-bit framebuffer handed to the kernel in boot_args.Video; a64m_frame then shows it (frame counter = content changed) */
void a64m_set_video(A64Machine *m, uint64_t pa, uint32_t w, uint32_t h, uint32_t rowbytes);

#ifdef __cplusplus
}
#endif
