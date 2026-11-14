# История версий и разбор остановок

## IPSW boot attempt (0.2.2, experimental)

Source "Chosen file: IPSW - TRY to boot kernelcache". It really performs, in order: find the kernelcache in the ZIP (zip64, stored/deflate) ->
unwrap IM4P/IMG4 (DER) -> LZFSE (Apple's BSD reference decoder, `app/src/main/cpp/lzfse/`) or `complzss` decompression -> parse the arm64 Mach-O kernel ->
load its segments into a64virt RAM (physical = RAM base + 4 MiB + vm offset) -> optionally unwrap the DeviceTree -> build an XNU-style `boot_args`
(layout written from memory of XNU's public header: unverified) -> enter the kernel's LC_UNIXTHREAD entry at EL1, MMU off, x0 = boot_args.
Every step goes to the Boot log; the run ends with the real emulator fault (kernel virtual PC, instruction word, registers).

It stops *before* executing anything, and says so, when: the payload carries a KBAG (encrypted; no keys are held, derived or fetched), the kernel is 32-bit ARM,
the data is not an arm64 Mach-O, or no kernelcache is present. It never decrypts, patches, skips signature checks or reports a success it did not observe.
Expect modern kernels to fault within the first instructions (Apple system registers, PAC/SIMD, enabling the MMU). That is the intended, honest outcome of this feature.

Tests: `tests/run_all.sh` group 5b builds synthetic IPSWs (arm64 test kernel in IM4P+LZFSE, IM4P+complzss, plain, encrypted, 32-bit, garbage, no kernel) and checks the log and states.


## UI (0.3.0)

One screen in Material 3 style (dynamic colour on Android 12+, baseline palette otherwise; follows light/dark): a source card (file name, what Run will do),
"Choose file" and "Demos", one large Run/Stop button, and a Display / Log / Info switch. The run mode is chosen from the inspector result
(Mach-O -> user mode, IPSW -> boot attempt, unknown -> raw image, anything else is refused with the reason in Info). The display tab opens by itself when the guest starts drawing.

## Software MMU (0.3.6)
Stage-1 EL1&0 translation: 4K/16K/64K granules, TTBR0/TTBR1, TBI, block/page descriptors, access flag (HA optional), proper DFSC/FAR on aborts, translated instruction fetch, 256-entry TLB flushed on TTBR/TCR/SCTLR writes and TLBI. Permission bits (AP/XN/PXN/PAN, Apple APRR), ASIDs, stage 2 and 52-bit addresses are NOT modelled. Tested by `tests/asm/mmu.S` (guest builds its own page tables). UI: the Info tab and the built-in demo/test kernels are gone from the app (they remain only as host-side tests); tabs are Display / Kernel (what the guest writes) / Emulator (what the emulator says).

## SIMD / floating point (0.3.13)
`simd.inc` (included by `arm64.c`): V0-V31, all SIMD&FP loads/stores (incl. LD1-LD4/ST1-ST4, lane and replicate), scalar single/double FP (arithmetic, compare, convert, FMOV, FCSEL, FMADD family), and the common AdvSIMD groups (copy, modified immediate, three-same, two-register misc, across lanes, shift by immediate, permutes, EXT, TBL). Verified by a differential test (`tests/diff/simdprog.c`, clang at -O0..-O3 auto-vectorising to NEON vs a host build) and hand-checked asm (`tests/asm/simd_tests.S`). An instruction outside this subset stops the emulator with "unimplemented SIMD/FP instruction 0x........" instead of faulting the guest. Not modelled: half precision, FPCR modes, FPSR flags, FP traps, crypto/dot-product.

## 0.3.17 – device-tree fix-up
The 0.3.16 log showed the kernel walking the DeviceTree and stepping 2 GiB past a property (pointer 0xc0010078): in Apple's tree a
property length with bit 31 set marks a placeholder that iBoot resolves before booting, and the kernel's walker does not mask it.
The attempt now clears only that flag (bytes unchanged, nothing invented) and logs it as a MODELLED subset of iBoot's work.
Also logs the first guest reads of boot_args and the last 48 instructions before the first exception.

## 0.3.18 – diagnostics for the next stop
After the device-tree fix the first fault is `mrs x8,TPIDR_EL1; ldr x8,[x8,#0x468]` with TPIDR_EL1 = 0 (the kernel's per-thread pointer
has not been set yet). The log now also shows the frame-pointer call chain and any text the registers point to at the first exception,
and every guest write to TPIDR_EL1 (or that it was never written).

## 0.3.19 – boot CPU
0.3.18's log showed the kernel's own message: "unable to determine boot cpu!" (the kernel panicked correctly, then crashed because
TPIDR_EL1 was still 0 that early). iBoot normally marks the boot CPU's node under /cpus with state="running". The attempt now does the same
(first cpu node with a "state" property, same 8 bytes), logs all cpu nodes, and models MPIDR_EL1 = 0x80000000 | reg of that node. Modelled, labelled as such.

## 0.3.20 – LSE atomics
0.3.19 got past the boot-CPU check (TPIDR_EL1 set by the kernel itself). The next stop was an "undefined instruction" `b829011f`, which is
`ldadd w9, wzr, [x8]` (= STADD) from ARMv8.1 Large System Extensions. Implemented (single core: load-modify-store): LDADD/LDCLR/LDEOR/LDSET/
LDSMAX/LDSMIN/LDUMAX/LDUMIN (B/H/W/X, A/L variants), SWP, LDAPR, CAS, CASP, LDXP/STXP. ID_AA64ISAR0_EL1 now reports Atomic=2.
No memory-ordering effects are modelled (one core, sequential). Test: tests/asm/lse_tests.S.

## 0.3.21 – PAN
0.3.20's log: the kernel ran ~48k more instructions, then `d500419f` = `msr PAN, #1` (ARMv8.1 Privileged Access Never) was undefined.
PSTATE.PAN is now tracked (set/cleared by MSR, saved in SPSR bit 22, restored by ERET, set on exception entry when SCTLR_EL1.SPAN=0,
readable as the PAN register, advertised in ID_AA64MMFR1_EL1) but NOT enforced: the MMU has no AP/PXN permission checks (documented limitation).

## 0.3.22 – entropy placeholders
0.3.21's log is a genuine kernel panic: `EntropyData needed %lu bytes, but got %u.` (the kernel's trap `udf #0xdeff` follows the panic).
iBoot fills /chosen `random-seed` / `entropy-data` with random bytes; in the IPSW tree they are empty placeholders. The attempt now fills
each all-zero such property with bytes from the host's random source (/dev/urandom), lists all /chosen properties, and logs this as a MODELLED iBoot step.
Also: for registers at the first exception that point to kernel data, the log shows the first four 64-bit words (decodes panic() arguments).

## 0.3.23 – data-access history
0.3.22's log: the entropy panic is gone (the kernel got its random-seed) and the run continues ~80k instructions further. The new first
fault is a software page-table walk (`pmap`-style) in the kernel: it reads entry 7 of the root table that its own global points to, gets 0,
turns it into physical address 0 and dereferences phystokv(0). To see which table that global points to and what the kernel wrote there before,
the log now contains the last 40 data accesses (pc, read/write, size, virtual address, value) before the first exception.

## 0.3.24 – AT (address translation) instructions
0.3.23's access log showed the kernel's own root table (global at 0xfffffff0077ddee0 -> 0xfffffff0077a4000) holding entry 7 = 0x3: a valid table
descriptor whose address field is 0. Table addresses are normally produced by the kernel's virtual-to-physical routine, which on arm64 uses
`AT S1E1R` + `mrs PAR_EL1`. The emulator treated `SYS` (AT) as a no-op, so PAR_EL1 stayed 0 and the address came out as 0. AT S1E1R/W and
S1E0R/W now run the stage-1 walk and fill PAR_EL1 (PA, F/FST on faults; permissions not modelled). The panic report counts AT executions.
This is the most likely cause, not a proven one: the next log shows whether it was.

## 0.3.25 – diagnostic: store watch on the kernel global holding the DeviceTree base (0xfffffff0091baaa8)
The panic report lists the first 16 stores to it (value, pc) to find where 0x6ce50000 comes from.

## 0.3.26 – diagnostic: registers and code around the stores to the DeviceTree base global
0.3.25 showed two stores: 0x40010000 (pc 0x810e17c) and then a different, run-dependent value 0x58e50000/0x6ce50000 (pc 0x7ba3788). The report now includes registers and the preceding code for these stores.

## 0.3.27 – shorter panic report
0.3.26 log was cut. The report now prints store #2 (the run-dependent DT pointer) first and omits the boot_args access list.

## 0.3.28 – longer code window before the DT-pointer store #2

## 0.3.29 – deviceTreeP is a kernel virtual address (MODELLED)
0.3.28 trace: the kernel does DT_global = old_value + (new_region_va - old_region_va). With a physical deviceTreeP that gives garbage (0x64e50000). The DT now sits above the kernel data (phys top+1 MiB) and boot_args.deviceTreeP is its kernel virtual address (only for high-half kernels; the flat test kernel keeps the physical one).

## 0.3.30 – diagnostics after 0.3.29
0.3.29 log: deviceTreeP now virtual; the kernel remaps the region to a random VA (0xfffffff00e218000) but the data there reads as ASCII ("external...") instead of the DT header. The report now walks that VA and shows the DT physical words.

## 0.3.31 – DT placed 0x20000 above the physical address deviceTreeP names (MODELLED, from the 0.3.30 trace)
The kernel remapped the DT region at a random VA, but that VA translated to phys 0x433f8000 while the DT was at 0x433d8000 (intact). The kernel maps the region from topOfKernelData+0x20000 (code: add x12, top, #32, lsl #12), so the DT is now placed 0x20000 higher. This is an empirical shim, not a documented iBoot layout.

## 0.3.32 – DT at topOfKernelData+0x120000, deviceTreeP points exactly at it (MODELLED)
0.3.31 log showed the kernel no longer relocates the DT when it sits clear of its early page-table area (tables now built at 0x432d8000/0x432dc000), and then reads it through its own linear map at the virtual address deviceTreeP names. The 0.3.31 +0x20000 shim was wrong (the pointer pointed 0x20000 below the DT) and is removed; only the placement stays.
New stop: "No cpus found!" – the kernel found no DT at the old address (it read zeros).

## 0.3.33 – diagnostic only
0.3.32 log: DT early read OK (pointer -> DT), then the kernel slides the pointer by delta and maps the region so that the slid pointer lands 0x20000 above the DT (inside the DT, which is why it reads "external..." garbage). The panic report now dumps the mapping routine (0xfffffff007ba1a70, 0x140 bytes).

## 0.3.34 – diagnostic only: dump the whole routine around 0xfffffff007ba3380..0x7ba3640
0.3.33 showed the mapping routine (va, size, delta, flags). It is a per-32MiB L2/L3 mapper; the last call in the region loop has size 0 (x21=0). The caller loop builds the region list; its start is needed to see where the region bases and the 0x20000 come from.

## 0.3.35 – diagnostic: who writes the two kernel globals the region-slide uses
0.3.34 code: the kernel builds a region list; each region maps new_va -> physical start [page 0xfffffff0077de000 + 1496] while the DT pointer is slid by (new_va - [+1096]). The two bases differ by 0x20000 (0x432f8000 vs 0x432d8000). The report now lists the first stores to both globals.

## 0.3.36 – diagnostic: arguments of every call to the range-mapping routine (0xfffffff007ba1a70)
0.3.35: [+1096]=0xfffffff0092d8000 (virt of topOfKernelData) and [+1496]=0xfffffff0092bc000 (__LINKEDIT start) are written by the kernel itself (pc 0x7ba2eb0 / 0x7ba2b9c). The DT slide adds (new_va - 0x92d8000) while the region maps to top+0x20000: the 0x20000 is most likely the kernel own early page tables allocated at topOfKernelData. The report now lists the mapping calls (va, size, delta).

## 0.3.37 (diagnostic only)
The call trace of the region-mapping routine now records only calls with a non-zero delta (x2), up to 40, because the first 16 calls of 0.3.36 were all the kernel's own image at its old addresses (delta 0). This should list the relocated regions and which physical range backs the last one.

## 0.3.38 (fix of the DeviceTree layout, MODELLED)
The 0.3.37 trace of the kernel's own region list showed how it expects memory to be laid out: one region [end of kernel image, topOfKernelData) directly behind the image (the DT pointer is slid with that region's delta), then the memory above topOfKernelData + 0x20000 (its own first page tables). Before, the DT was placed above topOfKernelData, so the slid pointer was 0x20000 off. Now the DT sits right behind the image and topOfKernelData is the 16K-aligned end of the DT.
Also fixed: boot_args.memSize now counts from physBase (the kernel computes end = physBase + memSize; it was 0x10e4000 too large). Diagnostics: watch of kernel global +1064 instead of +1496.

## 0.3.39 (Apple S5L UART, MODELLED)
0.3.38 confirmed the DeviceTree layout fix: the kernel now slides the DT pointer correctly and runs ~60 million instructions. It then stops on a store to virtual 0xfffffff016000000, which the kernel itself maps to physical 0x235200000 (device memory): the console UART, for which no device existed (external abort).
- New: the UART node(s) under /arm-io are read from the DeviceTree (`ranges` of arm-io + `reg` of uart*), and an Apple S5L UART is modelled at the resulting physical address: UTRSTAT always reports "transmitter empty", nothing is received, other registers read back; bytes written to UTXH go to the Kernel tab. The DT facts (raw ranges, reg values, computed address) are logged.
- New diagnostic: the stop report says whether a physical address with no RAM and no device was accessed (first address, read/write).
- Tests: DT parsing of both `ranges` layouts, malformed trees, missing ranges; the UART model driven by real guest instructions.

## 0.3.40 (fix: arm-io `ranges` parsing)
The 0.3.39 log showed the real `ranges` of /arm-io is 48 bytes (two entries of u64 child, u64 parent, u64 size: 0 -> 0x200000000, size 0x100000000, ...), not 24, so no UART device was made and the kernel's console write (physical 0x235200000, uart0) still hit nothing. Parsing now accepts any number of entries (24-byte or 16-byte layout), picks the entry that contains each uart's `reg`, and logs every entry. Test added for the 48-byte shape.
The "unknown instruction" at 0x7a43b98 seen in 0.3.38/.39 is Apple's trap opcode (0xe7ffdeff) after a call to a panic routine: it is the kernel's panic path after the UART fault, not a separate emulator gap.

## 0.3.41 (EL1 physical timer registers; clearer stop message)
0.3.40 log: the UART works (three S5L UARTs from the DeviceTree) and the kernel went on to `msr CNTP_CTL_EL0` (S3_3_C14_C2_1), which was missing, so it trapped and the kernel's own panic path then spun. Added CNTP_CTL/CVAL/TVAL_EL0 (registers plus ISTATUS; the timer interrupt is NOT delivered - Apple routes it as FIQ, not modelled yet). The stop message no longer says "waiting for hardware" when the loop follows an exception, and prints the real number of guest output bytes (it used to say "none" unconditionally).

## 0.3.42 (diagnostics for the kernel's own panic)
With UART and CNTP registers in place the kernel now runs far enough to panic by itself and prints it on the Kernel tab: `panic(cpu 0 caller 0xfffffff007ab26ec): "Invalid waitq: 0xfffffff0091aef48"` (kernel data abort follow-ups, same text repeated). The waitq lives in kernel BSS; the question is why it was never initialised. This build only adds diagnostics: two code windows (the function that panicked and its caller), a data window around the waitq, and a record of which instructions store into the waitq and its 256-byte neighbourhood. The stop report also keeps the older diagnostics out of the way.

## 0.3.43 (diagnostics, second round on "Invalid waitq")
0.3.42 showed the waitq the kernel rejected (0xfffffff0091aef48) is all zeros and that nothing ever stored into it or the 256 bytes before it, i.e. it was never initialised (the panicking function tests a flag bit in its first word). The caller picks a "global waitq" by hashing an event (index = hash & (N-1), base + index * size). This build records the last 40 executions of that call (x0 = the waitq, plus the registers holding base, mask, index, stride) and dumps the caller's preamble, so it can be seen whether earlier calls used valid waitqs, and what N is. Panic-trap first exceptions no longer print the long "last accesses / instructions" lists (they were noise and made the log get cut).

## 0.4.0 — ребрендинг: XNUron
Технические идентификаторы тоже сменены: пакет и applicationId `com.xnuron.app` (старые `com.example.a64lab` больше нет), имена JNI, артефакт сборки `xnuron-apk`, архив `xnuron.zip`. Это новое приложение: старую версию придётся удалить, поверх она не встанет. Проект переехал в новый репозиторий. Содержимое 0.3.43 (диагностика «Invalid waitq») без изменений.

## 0.4.2

Новый значок и аватар проекта (красный фон, логотип с яблоком и орбитой). Адаптивный значок: логотип уменьшен и стоит в безопасной зоне, поэтому маска лаунчера (круг, «squircle») его не обрезает. Для Android 7 — обычные квадратные и круглые значки.

## 0.4.3

**Настройки.** Шестерёнка справа вверху открывает панель (Material 3, крупные скругления): тема «System / Light / Dark» и цвета «XNUron (red)» / «Dynamic». По умолчанию — фирменная красно-белая тема (в тёмной — тёмно-красная); «Dynamic» берёт цвета обоев (Material You, Android 12+; на старых версиях недоступно). Выбор запоминается; после смены экран пересоздаётся (идущий запуск при этом останавливается). Надпись «Experimental AArch64 emulator» убрана.

**Диагностика «Invalid waitq».** По логу 0.4.1: паника берёт глобальную очередь `0xfffffff0091aef48` из массива в BSS (нули, в ядре не записывалась), базу и размер массива читает из структуры на `0xfffffff008f95c00/08`. Теперь стоп-отчёт печатает эту структуру (окно данных 1), чтобы увидеть, запущена ли инициализация массива вообще. Исправления эмулятора тут пока нет: причина не подтверждена.

## 0.4.4

Из лога 0.4.3: структура глобальных очередей на `0xfffffff008f95c00` содержит базу массива `0xfffffff0091aef48` и размер 1, то есть массив зарегистрирован, но его единственный элемент (нули, ни одной записи) не инициализирован. Паника — первая же операция над ним. Теперь отчёт показывает (а) вызовы функции проверки очереди `0xfffffff007ab269c` (последние 40, с x0 = очередь), чтобы увидеть, работали ли до этого другие очереди, и (б) кто и когда пишет в базу/размер массива (`pc=значение`) либо они лежат в образе статически.

## 0.4.5

Из лога 0.4.4: массив глобальных очередей получает базу во время работы кода ядра (запись по `pc=0xfffffff007ba0378`, значение `0xfffffff0091aef48`), размер остаётся 1, а в сам элемент очереди ни одной записи нет; функция проверки очереди вызывается один раз за весь запуск — сразу с этим пустым элементом. Значит, инициализация, которая должна идти после выделения массива, не выполняется. Теперь отчёт показывает код вокруг этой записи и 400 инструкций, выполненных сразу после неё, чтобы увидеть, куда уходит управление.

## 0.4.6

Из лога 0.4.5: `0xfffffff007ba0378` — это цикл самого ядра, который применяет цепочки релокаций (chained fixups) к своим данным; он и записал в статическую структуру адрес массива очередей. То есть массив — статический, а вот инициализирующего кода (он должен выставить в элементе флаг «valid», бит 5) мы не видели. Теперь стоп-отчёт ищет в образе ядра все инструкции, которые обращаются к этой структуре и к самому массиву (ADRP + LDR/STR/ADD), и помечает каждую `+` (выполнялась) или `-` (ни разу не выполнялась). Так видно, дошёл ли запуск до инициализирующей функции. Для этого включена карта покрытия кода ядра (по биту на инструкцию, около 280 КБ).

## 0.4.7

Из лога 0.4.6: все показанные обращения к структуре очередей (первые 24 из списка) ни разу не выполнялись, а обращений к самому массиву по прямому адресу в коде нет (он достаётся только через указатель). Список был обрезан. Теперь: (а) в отчёте весь список обращений, каждое помечено `+/-` (выполнялось / нет) и `l/s/a` (чтение / ЗАПИСЬ / взятие адреса) — записи покажут, где инициализация; (б) последние 160 функций, вызванных в первый раз (цели `bl`) по порядку: видно, какие функции ядра шли прямо перед паникой, и что в этой цепочке не успело выполниться.

## 0.4.8

Из лога 0.4.7: структура `0xfffffff008f95c00/08` — это `global_waitqs` / `g_num_waitqs`, на неё ссылаются больше 200 мест (встроенный `global_eventq(event)`), и ни одно, включая единственное «взятие адреса» в `0x7aa7f40` (похоже на `waitq_bootstrap`), до паники не выполнялось. То есть ядро доходит до функции `0x7f1500c`, которая будит событие по глобальной очереди, РАНЬШЕ, чем инициализация очередей. На настоящем телефоне она до этого места, видимо, не доходит: цикл в ней идёт только при `count >= 1`, а счётчик `0x8fb2930` у нас не ноль. Теперь отчёт показывает счётчик, пределы, таблицу, кто их пишет (адрес команды и значение) и аргумент функции. Список обращений сокращён (нелюбопытные чтения только считаются), потому что длинный лог обрезался.

## 0.4.9

Из лога 0.4.8: счётчик записей `0x8fb2930` = 1 лежит в самом образе (в рантайме в него ничего не писали), указатель на таблицу `0x91496f8` = 0, все три «предела» (`0x91494a8`, `0x91494ac`, `0x91494d4`) = 0, аргумент функции = 0. То есть всё это начальное состояние, а решает всё условие вида «аргумент > предел». На настоящем телефоне до этого вызова пределы, по-видимому, уже заполнены. Теперь отчёт: (а) для каждого окна кода печатает текстовые строки, на которые код ссылается (они называют функцию), (б) показывает хвост паникующей функции, её первый вызов, код над её вызывающей и вызывающую часть, (в) находит, кто вызывает функцию с `0x7aa7efc` (предположительно `waitq_bootstrap`) и выполнялись ли эти вызовы.

## 0.4.10

**Настройки (уточнено в 0.4.11).** Тема «System / Light / Dark» и цвета «XNUron» (фирменная красно-белая, по умолчанию) / «Dynamic» (Material You, Android 12+) / «Classic» (нейтральная чёрно-белая без цветных акцентов, светлая или тёмная по выбору темы).

**Паника «Invalid waitq», найдена причина (по логу 0.4.9).** Падающая функция `0x7f1500c` — «будильник» по свободным страницам: её вызывает `pmap_startup` (строка `"pmap_startup"` и проверки `"overflow detected"` в коде) с аргументом = число страниц, которые она только что освободила в список свободных. Аргумент равен 0. Если страниц больше нуля, функция уходит по `b.hi` и очередей не трогает (пределы у нас нули); при 0 она заходит в цикл и будит событие по очереди, которой ещё нет (`waitq_bootstrap` вызывается из `kernel_bootstrap` позже). То есть настоящая проблема — `pmap_startup` у нас освободил НОЛЬ страниц. Теперь отчёт показывает весь код `pmap_startup` (со строками), счётчики страниц и запись в них.

## 0.4.11

Настройки: вернул выбор цветов и добавил третий пункт — «Classic» (чисто чёрно-белая/серая тема без красного и без динамических цветов). Содержимое остальное — как в 0.4.10.

## 0.4.12
Лог 0.4.10 обрезало окном кода pmap_startup (0x630 байт). Окно убрано; вместо него печатаются глобалы, из которых pmap_startup считает число страниц: `avail_end` (0x77de420), `first_avail` (0x77de4d8), указатели массива страниц (0x912b830…), а также store-watch на запись в них. Цель — понять, почему pmap_startup освобождает 0 страниц (MODELLED-исправление будет только по данным).

## 0.4.13
Лог 0.4.12: `first_avail` (0x77de4d8) к моменту `pmap_startup` уже равен `avail_end` (0x472e0000) — всю свободную память (0xf7e страниц, ~63 МиБ) съедает цикл с записью из pc 0x7b53e50 внутри функции 0x7b53d60, поэтому `pmap_startup` освобождает 0 страниц, а потом паника «Invalid waitq». Теперь печатается код этой функции, её вызывающий и регистры на входе — выясняем, откуда у неё такой размер (подозрение: размер/адрес памяти в boot_args или DT).

## 0.4.14
Лог 0.4.13 (обрезан приложением/копированием на 20000 символов): функция 0x7b53d60 — это `pmap_steal_memory(size)` (строка "pmap_steal_memory() size: 0x%llx"), её вызывают 5 мест из 0x7b5306c (`vm_page_bootstrap`) и `pmap_startup` (0x7b53fac, массив `vm_page`). Один из вызовов крадёт ~63 МиБ — всю память. Теперь печатается только кольцо вызовов `pmap_steal_memory` (x0 = размер), остальные окна убраны, чтобы лог влез.

## 0.4.15
Лог 0.4.14: все 6 вызовов `pmap_steal_memory` маленькие (последний — массив `vm_page`, 0x2e7a0 байт), а `pmap_startup` как раз освободил 0xf7e страниц, так что теория «память съедена» была неверной. Паника идёт из функции 0x7f1500c: она вызвана с x0=0, обходит таблицу с нулевой базой [0x91496f8] и пропускает обход только если процент свободных страниц (функция 0x7b41a20) больше порогов 0x91494a8/0x91494ac. Теперь печатаются код 0x7b41a20, пороги и значения, на которые она делит (0x91494a0, 0x8fb2740, 0x912e3c0), и кольцо вызовов 0x7f1500c.

## 0.4.16
Лог 0.4.15: функция 0x7b41a20 считает `pct = free*100/(total-wired)`, где free = [0x8fb2748] — аргумент 0x7f1500c; пороги [0x91494a8…] статически 0, поэтому обход таблицы пропускается только при pct>0. Аргумент здесь 0, хотя `vm_page_free_count` ([0x912b84c]) = 0xf7e. Аргумент собирается в конце `pmap_startup` (0x7b5452c) — печатаем этот код и слова, которые он складывает (0x9127e40.., 0x9127870, 0x912b830..). Это по-прежнему диагностика, без исправлений.

## 0.4.17
Лог 0.4.16: аргумент D считается как `w0 = [0x9127e5c] + [0x912b840] + ([0x9127878]==1 ? 0 : [0x9127e40])`; здесь все три слагаемых = 0, хотя `vm_page_release_startup` (0x7b54614, вызов на 0x7b54520 для каждой страницы) выполнялась — значит, счётчики свободных страниц в этом прогоне она не увеличивает. Печатаем её код, число её вызовов (кольцо) и store-watch на оба счётчика. Диагностика, не фикс.

## 0.4.18
Причина паники «Invalid waitq» найдена по логу 0.4.17: `vm_page_release_startup` (0x7b54614) сначала кладёт страницы в *secluded*-очередь, пока их меньше цели (~500 МиБ, [0x912b860]=0x1f400000), и только потом в обычную очередь свободных. При 114 МиБ RAM все 3966 страниц ушли в secluded ([0x912b844]=0xf7e), `vm_page_free_count` ([0x912b840]) остался 0, `pmap_startup` позвал будильник (0x7f1500c) с 0 свободных страниц, он пошёл по таблице до инициализации `waitq_bootstrap` → «Invalid waitq». У настоящего iPhone XS 4 ГиБ, поэтому свободных страниц хватает. Исправление (MODELLED, это размер памяти, а не подмена ядра): гостю даётся 768 МиБ RAM (запасной вариант 640 МиБ, затем минимум, если хост не даёт); память выделяется лениво, платятся только затронутые страницы.

## 0.4.19
Лог 0.4.18 (768 МиБ RAM): паника «Invalid waitq» ушла, загрузка идёт в 2,3 раза дальше (25 млн инструкций) и падает в поиске по хеш-таблице (0x803ad20–0x803ae08, похоже на пул символов IOKit): у корзины по VA 0xffffffe000889ef0 `count` читается как 0x914dc30 (мусор, цикл идёт по стольким указателям), а массив указателей в BSS ядра (0x914dc58) заканчивается числом 0x98 вместо указателя. Окна данных теперь читают память через страничные таблицы гостя (работают для зоны/апертуры), печатается физический адрес; поставлены store-watch на массив и корзину, чтобы увидеть, кто и каким pc их пишет. Диагностика, не фикс.

## 0.4.20
Лог 0.4.19: корзина `{count, ptr}` (VA зоны случайный на каждый запуск, в этот раз 0xffffffe0000da2f0) содержит пару указателей `{0xfffffff00914dc30, 0xfffffff00914dc58}` — на статический массив 40-байтных записей в BSS ядра; значит, память таблицы перекрыта чужими данными или не инициализирована. Добавлен «value watch» (`a64m_set_value_watch`): печатаются первые 8 записей значения 0xfffffff00914dc30 (pc, VA, PA), где бы они ни происходили; список «первых вызовов» сокращён до 40, чтобы лог влезал в 20000 символов. Диагностика, не фикс.

## 0.4.21
Лог 0.4.20: корзину пула символов `{count, ptr}` заполнила пара `{0xfffffff00914dc30, 0xfffffff00914dc58}` (два статических `OSMetaClass`, шаг 0x28) из pc 0x79fd59c — это процедура копирования; источник (VA …329ef0) до этого записал pc 0x800a02c. То есть блок kalloc заполнен копированием и после этого не обнулён (или выдан повторно без обнуления). В value watch теперь пишутся lr и x0–x3 в момент каждой записи, чтобы увидеть вызывающего копирование. Диагностика, не фикс.

## 0.4.22
Лог 0.4.21: значение 0xfffffff00914dc30 (указатель на `OSMetaClass`) копируется процедурой 0x79fd570/0x79fd59c (вызов дважды из lr 0x8009ffc) в 1-КиБ-блок …55e400 из соседнего блока …55e800; позже этот блок используется как корзины `OSSymbolPool`. Печатаем код функции вокруг 0x8009ffc (строит записи `OSMetaClass`), процедуру копирования и вызывающих; списки доступов/инструкций у первого исключения отключены (бюджет лога 20000 символов). Диагностика, не фикс.

## 0.4.23 (диагностика + воспроизводимость)

Лог 0.4.22 упал уже в другом месте (NULL-чтение, ELR 0x8105d34, lr 0x8105c78), хотя версия та же: энтропия в /chosen брались из /dev/urandom хоста, ядро рандомизировало зоны по-разному, поэтому каждый запуск вёл себя по-разному.
- Энтропия DT теперь фиксированный псевдослучайный поток (MODELLED, постоянный seed; в логе так и написано) — запуски воспроизводимы, логи сравнимы.
- Окна диагностики больше не дублируются при повторном запуске попытки.
- Строки «Apple register» сжаты до одной строки (лог обрезается на 20000 символов).
- Диагностика: код вокруг 0x8105c20 и вызывающие 0x8105d34. Фикса пока нет.

## 0.4.24 (диагностика)

Лог 0.4.23 (теперь воспроизводимый) показал: второй сбой по ELR 0x8105d34 — только следствие (это обработчик «PowerOff/Restart/PagingOff», он читает NULL, потому что PMGR нет). Первое исключение прежнее: ELR 0x803ade0, FAR 0xa4.
Главная находка: таблица корзин символов (63×16 байт, x10=0xffffffe000725c00) и новый, выросший массив OSMetaClass-указателей (память под него отдана memmove по lr 0x8009ffc, dst=0xffffffe000725c00) — это ОДИН и тот же блок зоны 1 КиБ (pa 0x4ce51c00). Т.е. зона выдала блок дважды или освободила занятый.
- Добавлена история обращений к физическому блоку (`a64m_set_block_hist`): первые 40 записей (с коалесцированием циклов) с шагом, pc, lr и вызывающим lr; плюс чтение первого слова (ссылка свободного списка). Тест в machine_test.
- Фикса пока нет.

## 0.4.25 (диагностика)

Лог 0.4.24 разобран. История блока pa 0x4ce51c00 (элемент зоны kalloc.1024, va 0xffffffe000725c00):
1. zcram кладёт его в свободный список последним (ссылка = cookie 0xc0ffee13ac145ae2, т.е. next=NULL);
2. zalloc берёт его (чтение первого слова), пишет 0xdeadbeef в первое/последнее слово (шаги ~25.32M, lr 0x7a5554c);
3. memmove (lr 0x8009ffc) копирует в него массив «stalled»-классов OSMetaClass — это законная выдача, блок теперь принадлежит этому массиву.
Дальше 40 записей закончились на цикле копирования, и мы не видим, КТО потом снова получил этот же блок как таблицу корзин OSSymbolPool. Пока нельзя отличить баг эмулятора от ошибки моей модели — фикса нет.
- История блока: 64 записи, цикл склеивается по окну из 4 pc.
- Новое: «arg watch» — все вызовы (BL/BLR) с x0 или x1 = адрес блока (kfree/bzero/memmove) и все возвраты с x0 = адрес (kalloc/zalloc), первые 48.
- Отчёт теперь печатает диагностику один раз (раньше дублировалась); история PC схлопывает повторы; выброшены список свойств /chosen и строка UART-узлов, чтобы поместиться в 20000 символов.

## 0.4.26 (ФИКС: DC ZVA)

Лог 0.4.25 разобран до конца, причина найдена.
- Блок kalloc.1024 законно перешёл от массива OSMetaClass (после его роста и kfree) к таблице корзин OSSymbolPool.
- Таблицу обнуляет bzero(0x3f0 байт): 4 stp в голове, 4 stp в хвосте и 70 шагов всего. Середину bzero Apple обнуляет командой `DC ZVA` по 64 байта; эмулятор считал любую SYS-команду кэша NOP, поэтому середина (в т.ч. корзина 0x2f с +0x2f0) оставалась с мусором — старыми указателями OSMetaClass. Отсюда count=мусор и обращение по 0xa4.
- Фикс (эмулятор): `DC ZVA, Xt` в системном режиме обнуляет 64-байтный блок, содержащий Xt (через трансляцию адресов, с исключениями при ошибках). DCZID_EL0 в системном режиме = 0x4 (MODELLED: блок 64 байта, ZVA разрешён; в пользовательском режиме по-прежнему запрещён).
- Тест в tests/asm/mmu.S (шаги 19–23): блок обнуляется целиком, соседние байты не тронуты, DCZID_EL0 = 4.
- Диагностика 0.4.25 оставлена.

## 0.4.27 (SHA-1/SHA-256/AES + fixed-point FCVT)

Лог 0.4.26: фикс DC ZVA сработал — ядро прошло прежнее место падения (≈148 млн инструкций вместо 25 млн) и остановилось на нереализованной SIMD-инструкции `sha1su0` (0x5e023020, pc 0x81e4c34): это SHA-1 из corecrypto.
- Реализовано расширение Armv8 crypto по псевдокоду Arm ARM: SHA1C/P/M, SHA1H, SHA1SU0/SU1, SHA256H/H2/SU0/SU1, AESE/AESD/AESMC/AESIMC. Публичные примитивы, никаких ключей. Нет: PMULL, SHA-512, SHA-3, SM3/SM4.
- ID_AA64ISAR0_EL1 теперь честно сообщает то, что реализовано: Atomic=2, SHA2=1, SHA1=1, AES=1 (0x201110).
- Добавлены скалярные FCVTZS/FCVTZU и SCVTF/UCVTF с фиксированной точкой (нужны были тесту).
- Тест (tests/asm/crypto_test.c, шаг 2b в run_all.sh): настоящая программа, скомпилированная clang на NEON-интринсиках, считает SHA-1("abc"), SHA-256("abc") и AES-128 (шифрование и расшифровка, FIPS-197 C.1) и сверяет с опубликованными векторами. Ошибка, которую тест поймал по дороге: порядок слов T0 в SHA256SU1.

## 0.4.28 (дисплей: линейный фреймбуфер из boot_args.Video)

Что это на самом деле. Настоящий драйвер дисплея iPhone (AppleCLCD/DCP, Apple SoC) не реализуем без железа и прошивок. Но на раннем этапе ядро XNU рисует (verbose-загрузка `-v`, текст, паника) в ЛИНЕЙНЫЙ фреймбуфер, который ему передаёт iBoot в `boot_args.Video`. Его и реализовано (MODELLED).
- Фреймбуфер 270×480, 32 bpp BGRX, rowBytes 1088, в самом верху ОЗУ; из `memSize` он вычтен, чтобы ядро не раздало эти страницы как свободную память. Реальный iBoot даёт 1125×2436 и уже нарисовал логотип; здесь экран чёрный, рисует только само ядро.
- `boot_args.Video` = {baseAddr(40), display(48), rowBytes(56), width(64), height(72), depth(80)} по моему знанию раскладки (не проверено на реальном образе: если ядро не рисует — это первое, что проверить).
- Вкладка «Дисплей» показывает этот буфер; счётчик кадров растёт, когда содержимое меняется. Автопереключение на «Дисплей» только при первом реально нарисованном кадре (чёрный экран лог не закрывает).
- Тест (machine_test): чёрный буфер, нарисованный пиксель появляется с учётом rowBytes, счётчик кадров.
Честно: пока ядро не дошло до инициализации консоли, на экране ничего не будет.

## 0.4.29 (PMULL)

Лог 0.4.28: ядро дошло уже до ≈214 млн инструкций и остановилось на `pmull2 v1.1q, v0.2d, v2.2d` (0x4ee2e001, pc 0x81d6834) — полиномиальное (carry-less) умножение, на нём строятся GCM и CRC в corecrypto.
- Реализованы PMULL/PMULL2: 64×64→128 (crypto-расширение) и 8×8→16 по байтовым линиям. ID_AA64ISAR0_EL1: AES=2 (AES+PMULL), итог 0x201120.
- Тест (crypto_test.c): vmull_p64, vmull_high_p64 и vmull_p8 на 36+ парах значений сверяются с эталонным побитовым clmul.
- Про экран: строки «iBoot version: …» на вкладке «Дисплей» рисует само ядро в наш фреймбуфер (в коде XNUron такой строки нет).

## 0.4.30 — AdvSIMD: «three different» и «by element»

Лог 0.4.29: ядро дошло примерно до 323 млн шагов и остановилось на `uaddw` (pc 0xfffffff008f559c0). Это не было ошибкой ядра, интерпретатор просто не знал эту инструкцию.

Добавлено (по псевдокоду Arm ARM): семейство «three different» (UADDL/UADDW/USUBL/USUBW, SADDL/SADDW, UMULL/SMULL, UMLAL/UMLSL, ADDHN/RADDHN, UABAL/UABDL и т. п.) и формы «by element» (MUL/MLA/MLS, UMULL/UMLAL/SMULL/SMLAL, FMUL/FMLA/FMLS). Не реализованы: SQDMLAL/SQDMLSL/SQDMULL, SQDMULH/SQRDMULH by element, FMULX by element.

Тест: `tests/asm/simd2_test.c` (шаг 2c в `run_all.sh`), реальный clang-код, проверка результатов в эмуляторе.

## 0.4.31 — новый интерфейс и диагностика остановки

Лог 0.4.30: ошибок эмулятора нет, ядро отработало ≈1,54 млрд инструкций, остановлено пользователем (не падением). Где именно ядро было, по логу видно не было, поэтому при ручной остановке теперь печатаются pc, lr, sp и последние адреса.

Интерфейс переработан: на главном экране только выбор файла и кнопка **Start**. После запуска открывается полноэкранный дисплей (без системных панелей, экран не гаснет). Сбоку кнопки **Logs** и **Stop**. В панели логов два режима: **Kernel** (то, что гость пишет в UART) и **Normal** (сообщения эмулятора и отчёт об инспекции файла). После остановки кнопка Stop превращается в Close. Системная кнопка «назад»: закрывает логи, затем останавливает, затем закрывает экран.

## 0.4.32 — что делает ядро в «вечном» цикле

Лог 0.4.31 (D331AP, `kernelcache.release.iphone11`): ядро отработало 2,14 млрд инструкций без ошибок эмулятора; остановка пользователем. pc 0xfffffff007a08944, lr 0x…7a6ea60, sp на стеке потока (`0xffffffe0…`), то есть это уже не ранняя загрузка, а работающий поток ядра, крутящийся в цикле по адресам 0x7a088c0–0x7a090c0 и 0x7b9d340.

Фикса нет, это диагностика: при ручной остановке теперь печатаются регистры и код цикла, его помощника (0x7b9d340) и места вызова (около lr). Убраны старые диагностики блока зоны (block history / arg watch), они замедляли каждый вызов функции.

## 0.4.33 — FIQ от физического таймера (CNTP), кнопки в углу

Лог 0.4.32: ядро в цикле смены потоков (`Switch_context` 0x7a088c0, проверка времени по `CNTPCT` в 0x7b9d358), стек переключается между потоком и загрузочным стеком. Эмулятор доставлял только виртуальный таймер как IRQ; физический таймер (CNTP) прерывание не давал, хотя на Apple SoC оно приходит как FIQ. Ядро ждало тика планировщика, которого не было.

**MODELLED:** FIQ от CNTP (ctl: enable=1, imask=0, counter ≥ cval) доставляется в вектор +0x100/+0x300 при незамаскированном F. Реальное железо дополнительно гейтит это Apple-регистрами, здесь учитываются только архитектурные биты. WFI просыпается и по этому таймеру. Предположение, не проверено на железе: идёт ли decrementer ядра через CNTP.

Диагностика при Stop: состояние таймеров, число исключений и FIQ, цепочка вызовов по x29. Кнопки Logs/Stop перенесены в верхний правый угол.

## 0.4.34 — ядро дошло до настоящей паники

Лог 0.4.33: с FIQ от CNTP ядро пошло дальше (первое исключение на ≈297 млн инструкций вместо вечного цикла планировщика). Сообщения, которые увидел пользователь: «thread_invoke: preemption_level 1…», «thread already waiting on …», «hw_lock_bit(): timed out».

Разбор: первое исключение — `udf #0xdeff` в 0x7a43b98 сразу после вызова panic-функции (`bl`), то есть panic **вернулась**. Вызвана из кода AppleImage4 (цепочка 0x8e40b44 → 0x8e40ae0 → 0x8e427e8 → 0x8e42810 → 0x8e45010 → 0x801ca28; x1 = "panic", x2 = «"%s"@/BuildRoot/…/AppleImage4…»). Остальные паники («thread_invoke», «already waiting», «hw_lock_bit») скорее всего последствия: ядро не останавливается на panic (на железе она кончается рестартом через PMU/watchdog, которых у нас нет), продолжает работать и ломает собственные блокировки. Это гипотеза, текст первой паники ещё не получен.

Диагностика (не фикс): при первом исключении сохраняются 64 байта за x0–x7 (указатели ядра), печатается до 250 символов текста и тексты, на которые указывают эти слова (va_list на стеке с аргументами panic). Окна кода сменены на места вызова из цепочки AppleImage4.

## 0.4.35 — текст первой паники: «could not instantiate ppl environment: 0x60»

Лог 0.4.34 дал причину первой паники: AppleImage4 (`abort.h:24`) пишет **«could not instantiate ppl environment: 0x60»**. Функция 0x8e40aac вызывает 0x8e4210c(x0=0x…79d42d8, x1=0x…79d3ae0, x2=0) и получает ненулевой код 0x60. Остальные сообщения (thread_invoke, hw_lock_bit) — последствия: после panic ядро не останавливается (на железе её завершает рестарт через PMU), и работает дальше с испорченным состоянием.

Это ещё не фикс: что именно возвращает 0x60 (PPL-вызов через GENTER? проверка среды?), по коду неизвестно. Диагностика: дамп кода 0x8e4210c и функций рядом, и двух структур-аргументов. Пользователю достаточно присылать только блок `[panic]`.

## 0.4.36 — откуда берётся 0x60 в AppleImage4

Лог 0.4.35: структура-аргумент — `{0, "имя", таблица указателей на функции…}` (x0), `{1, 0x…79d42b8, 0, 0}` (x1). Функция 0x8e4210c сначала вызывает 0x8e40b70(buf, 0); при ошибке уходит на 0x8e422dc, иначе обходит таблицу указателей по 0xfffffff0090b5f08 (читает поля +0x18 и +0x68 у каждого элемента). Какой именно шаг возвращает 0x60, пока неизвестно (диагностика, не фикс). Теперь печатаются код 0x8e40b70, хвост цикла (0x8e421d0–0x8e42330) и таблицы.

## 0.4.37 — какое из 19 свойств даёт 0x60

Лог 0.4.36 расшифрован: 0x8e40b70 — это генератор/чтение nonce (ищет `/chosen`, `/options`, `AppleMobileApNonce` в DeviceTree; при отсутствии узла возвращает 3). Затем 0x8e4210c обходит **19 записей** (таблица 0x79d2e68, шаг 0xa8) и вызывает их виртуальные методы; если метод вернул не 0 и не 2 — функция выходит с этой ошибкой. 0x60 приходит из одного из этих методов. Гипотеза (не проверена): AppleImage4 ожидает от iBoot свойства `/chosen` (chip-id, board-id, ecid, security-domain, production-status и т. п.), которых в DeviceTree из IPSW нет.

Диагностика: пробник по pc (первый раз, когда w24 == 0x60 в 0x8e422a4: x25 = номер записи, x22 = запись, все регистры), таблица 19 записей с текстами (имя свойства), код метода 0x8e42320. Ничего не выдумываем: значения ECID, nonce и подписанные хэши в образе недоступны и придумываться не будут.

## 0.4.38 — какого свойства /chosen не хватает AppleImage4

Лог 0.4.37: зонд сработал. Ошибка 0x60 приходит на **записи №1 таблицы (тег `CEPO`, uint32)** — это `certificate-epoch`. Записи №0 (`crypto`) проходят; остальные 17 ещё не проверялись (цикл останавливается на первой ошибке). Метод читает значение из `/chosen` через 0x8e40c64/0x8e420a0; 0x60 — «значение не найдено/не подошло», точную семантику кода не подтверждаю.

Что дальше: сначала нужно увидеть, есть ли в `/chosen` из IPSW свойство `certificate-epoch` и какие вообще есть. Поэтому в лог возвращён список свойств `/chosen` (имя:длина). Ожидаемые AppleImage4 свойства (по тегам в таблице): CEPO certificate-epoch, CHIP chip-id, BORD board-id, ECID, SDOM security-domain, CPRO/CSEC (production/secure), BNCH nonce-hash, DGST digest … — это предположения по тегам, не по документации.

Принцип сохраняем: значения, которых нет в IPSW (ECID, подписанные хэши), не выдумываются; значения из BuildManifest/DeviceTree могут быть подставлены только как явно помеченное MODELLED.

## 0.4.39 — MODELLED: свойства /chosen, которых нет в IPSW (iBoot создаёт их сам)

Лог 0.4.38 (список свойств `/chosen`): есть crypto-hash-method, board-id, chip-id, unique-chip-id, production/security-mode, boot-manifest-hash, boot-nonce, image4-supported … Нет **`certificate-epoch`** (тег `CEPO`, запись №1 таблицы AppleImage4 — на ней и приходит 0x60) и **`security-domain`** (тег `SDOM`, запись №5). Записи таблицы с флагом 4 (CEPO, CHIP, BORD, ECID, SDOM, CPRO, CSEC, AMNM) — обязательные; остальные свойства из списка у нас есть.

Что сделано (MODELLED, помечено в логе): iBoot-часть «добавить в /chosen недостающее»: `security-domain = 1` (производственный домен безопасности розничных прошивок, ApSecurityDomain 1) и `certificate-epoch = 0`. **Предположение:** значение epoch iBoot берёт из цепочки сертификатов подписанного манифеста загрузки, которого в обычном IPSW нет; настоящее значение здесь неизвестно, 0 — заглушка, чтобы свойство существовало. Ни подписи, ни проверки не обходятся: AppleImage4 здесь лишь собирает свойства окружения.

Тест: вставка свойства в `/chosen` (дерево остаётся корректным, соседний узел цел, повтор — «уже есть», нет места/битое дерево — отказ).

## 0.4.40 — диагностика: геттер записи CEPO в AppleImage4

Лог 0.4.39: ошибка `could not instantiate ppl environment: 0x60` осталась на той же записи #1 (CEPO), хотя `certificate-epoch` и `security-domain` теперь есть в `/chosen`. Значит, гипотеза «свойства просто нет» не подтвердилась (или геттер ищет значение в другом месте). Здесь ничего не исправлено: добавлен второй зонд (`a64m_set_pc_probe2`) на виртуальный вызов геттера в 0x8e42240 для записи #1; при срабатывании в логе печатаются регистры и код вызываемой функции (адрес x8 без PAC-подписи).

## 0.4.41 — диагностика: callback окружения AppleImage4

Лог 0.4.40: геттер записи CEPO (0x8e41afc) — диспетчер: берёт `[env+0x18]` (если 0 — возвращает 2), иначе вызывает этот callback с (env, ключ, out); именно он возвращает 0x60. Здесь ничего не исправлено: в зонд добавлен дамп объекта env (16 qword) и кода callback `env[0x18]`.

## 0.4.42 — диагностика: чтение свойства из /chosen в AppleImage4

Лог 0.4.41: callback окружения (0x8e423c0) для записи CEPO вызывает 0x8e40c64(источник, селектор, имя свойства, ключ, &длина=4), а она делает виртуальный вызов `[vtable+0x110]` на узле /chosen (или /options). Ничего не исправлено. Теперь: зонд ловит этот вызов (0x8e40cd0, длина==4) и печатает код цели и объект узла; дамп кода обработки результата 0x8e40cd4…; в таблице записей печатаются слова +0x20..+0x58 (имя свойства и селектор источника).

## 0.4.43 — ФИКС: свойство называется `chip-epoch`, а не `certificate-epoch`

Лог 0.4.42 напечатал таблицу записей AppleImage4 с именами свойств: для CEPO это `chip-epoch` (источник /chosen). В 0.4.39 я угадал имя неверно (`certificate-epoch`), поэтому ошибка 0x60 не уходила. Теперь в /chosen добавляется `chip-epoch` (MODELLED; значение 0 — заглушка, реальное значение поставляет iBoot и его нет в IPSW; ничего по нему здесь не проверяется). `security-domain` имя совпало. Остальные имена из таблицы (chip-id, board-id, unique-chip-id, certificate-*, mix-n-match-prevention-status, boot-manifest-hash, effective-*-ap, crypto-hash-method) уже есть в DeviceTree.

## 0.4.44 — AppleImage4 пройден; MODELLED: неизвестный блок регистров SoC на 0x200000000

Лог 0.4.43: ошибка 0x60 ушла (зонд на ней «never reached»), ядро пошло дальше (≈297M инструкций) и упало на новом месте: data abort (внешний, ESR 0x96000010) в `0xfffffff007babef0` (`ldr w8,[x0,#0x680]`), x0 = 0xffffffe0190b0000 — это device-отображение физического 0x200000000 (PTE 0x6000020000040f), где в эмуляторе нет ни RAM, ни устройства.

Что за блок и что он возвращает на настоящем железе — мне неизвестно. Поэтому: **MODELLED, предположение** — страница 16 КиБ на 0x200000000 ведёт себя как «неизвестный блок»: чтение даёт 0, запись игнорируется, первые 40 обращений печатаются в лог (`[emu] MODELLED unknown SoC block: …`), чтобы было видно, что именно ядро трогает. Плюс дамп кода функции (0x7babe80…) и вызывающего (0x7a75140…). Тест: чтение возвращает 0, внешнего аборта нет, лог содержит обращения.

## 0.4.45 — блок 0x200000000 расширен до 1 МиБ; кнопка Copy в логах

Лог 0.4.44: после чтения `0x200000680` ядро читает те же регистры (+0x680/+0x684) по 0x200040680 и т.д. Код функции `0x7babec0` сравнивает значения разных блоков (шаг 0x40000) и паникует со строкой «Inconsistent memory configuration», если они различаются, — похоже на регистры каналов контроллера памяти, которые должны совпадать. Что там на самом деле — неизвестно. **MODELLED, предположение:** весь 1 МиБ с 0x200000000 — «неизвестный блок» (чтение 0, запись игнорируется, обращения логируются), поэтому все каналы совпадают.

UI: в панели логов рядом с Close появилась кнопка **Copy** — копирует весь текст открытой вкладки (Kernel или Normal) в буфер обмена и показывает, сколько символов скопировано. (Java не компилировалась здесь — нет Android SDK; если CI упадёт, пришли ошибку.)

## 0.4.46 — ФИКС: лог в приложении обрезался (не буфер обмена)

Причина «копируется не всё»: приложение читало нативный лог кусками по 64 КиБ за один опрос, а после остановки — ровно один раз (`finishRun` → один `pollOnce`). Отчёт об остановке пишется сразу целиком и бывает больше 64 КиБ, поэтому его конец до экрана не доходил, и Copy копировал то, что было на экране. Исправлено: `pollOnce` читает в цикле, пока есть данные (до 64 раз), нативный буфер чтения 256 КиБ, в приложении хранится весь лог (до 1.1 млн символов, нативный лимит 1 МиБ), на экране — последние 150 000 символов (иначе огромный TextView тормозит), Copy копирует весь текст вкладки. Java здесь не компилировалась (нет Android SDK).

## 0.4.47 — диагностика: драйвер watchdog («clock speed does not match absolute time»)

Лог 0.4.46: проверка памяти («Inconsistent memory configuration») пройдена — блок 0x200000000 читается как 0, все каналы совпали (ядро читает +0x680/+0x684/+0x68c, пишет 1 в +0x688/+0x68c каждого канала). Ядро продвинулось до ≈375M инструкций и остановилось на новой панике: `AppleS5L8960XWatchDogTimer.cpp:55: "%s: clock speed %llu does not match absolute time"`, аргумент «clock speed» = 0. Ничего не исправлено: добавлен дамп кода вокруг вызова паники (0x8ed2300…), чтобы увидеть, откуда драйвер берёт частоту и с чем её сравнивает.

## 0.4.48 — диагностика: частота «clock» для watchdog

Разбор дампа 0.4.47: `AppleS5L8960XWatchDogTimer::start` получает объект-часы, вызывает его виртуальный метод `+0x548` (это «частота», x21), заполняет `clock_timebase_info {numer, denom}` (в логе 125/3 = 24 МГц — от нашего CNTFRQ) и требует `частота × numer == denom × 10⁹`. Метод вернул 0 — отсюда паника. Ничего не исправлено: зонд на этот вызов (0x8ed23f8) печатает код цели и объект-часы (16 qword), чтобы понять, откуда он берёт частоту (свойство DeviceTree, регистр или другой драйвер).

## 0.4.49 — диагностика: объект-клок watchdog

Лог 0.4.48: «частота» берётся методом 0x8150114 объекта `0xffffffe0004e3aa0` — это getter «частота по индексу»: `if ([obj+0x90] <= idx) return 0;` иначе спрашивает провайдера `[obj+0x88]` (vtable+0x540) с аргументом `[obj+0x98][idx]`. Для индекса 0 вернулся 0: либо список клоков пуст (счётчик [obj+0x90] = 0 — например, нет свойства в DeviceTree), либо провайдер вернул 0. Ничего не исправлено. Теперь печатаются: 32 qword объекта, массив [obj+0x98], объект-провайдер [obj+0x88] (16 qword) и код его метода +0x540.

## 0.4.50 — диагностика: таблица клоков провайдера (PMGR)

Лог 0.4.49: у объекта-клока watchdog счётчик клоков = 1, массив `[obj+0x98]` указывает на данные DeviceTree с значением 4 (id клока), провайдер `[obj+0x88]` = 0xffffffe000289c40. Метод провайдера (0x814fa84) — поиск по id: для id ниже «особой» базы `[+0xc4]` он берёт `[+0xb0][id]` (если `[+0xa0]` > id) и возвращает 8-байтовое значение из таблицы — это и есть «частота», и она оказалась 0. Ничего не исправлено. Теперь печатаются: 28 qword провайдера и таблицы `[+0xb0]`, `[+0xa8]`, `[+0xb8]` (по 12 qword), чтобы понять, чем и откуда заполняется таблица частот.

## 0.4.51 — экран побольше + диагностика таблицы частот PMGR

**Экран (изменение, не диагностика).** Модельный фреймбуфер IPSW-попытки поднят с 270×480 до **540×960** (32 bpp BGRX, rowBytes 2160, всё так же MODELLED: размер выбран нами, у настоящего XS панель 1125×2436, а раскладка `boot_args.Video` не проверена). Пропорции 9:16 сохраняются, шрифт ядра остаётся тем же, поэтому текст стал вдвое мельче относительно экрана. Демо-дисплей (270×480) показывается удвоенным, касания пересчитываются. Тест: рисование в правый нижний пиксель 540×960 и отказ при запросе старого размера.

**Диагностика.** Лог 0.4.50: у провайдера (PMGR, `0xffffffe000289c40`) число записей `[+0xa0]` = 6; таблица `[+0xb0]` — шесть нулей (частоты), таблица `[+0xa8]` — шесть указателей на объекты, особая база `[+0xc4]` = 0. Значит, для id 4 частота действительно 0, а не «не найдено». Кто и чем должен заполнять эти шесть значений, из дампа не видно: после таблиц лежат указатели в DeviceTree (`0xfffffff02411cf18`, `0xfffffff02411d298`). Ничего не исправлено. Теперь печатаются байты (hex + текст) по этим указателям, по указателям-полям провайдера и вокруг массива клоков.

## 0.4.52 — исправление: скалярное FMUL/FMLA/FMLS «по элементу»

Лог 0.4.51 (экран 540×960) показал, что ядро пошло дальше прежней паники watchdog: на экране уже печатаются `iBoot version`, строка AppleImage4 и `AppleARMBacklight::start …`; зонд на вызов watchdog (0x8ed23f8) в этом прогоне **не сработал**. Почему путь изменился (другой размер/адрес фреймбуфера, значит другая раскладка памяти и порядок запуска драйверов), я не знаю и не утверждаю; возможно, паника просто ещё впереди. Остановка теперь другая: эмулятор не знал инструкцию `0x5fc09863` = `fmul d3, d3, v0.d[1]` (скалярное умножение на элемент вектора; код подсветки экрана считает в double). Это **исправление** недостающей инструкции, а не обход: реализованы скалярные FMUL, FMLA, FMLS по элементу для S и D (размеры 10 и 11), остальной регистр обнуляется. Тест: точные кодировки D-форм (`fmul/fmla/fmls d, d, v.d[i]`, компилятор их сам не выдаёт, поэтому через inline asm) и S-формы из intrinsics, сравнение с эталоном на хосте.

## 0.4.53 — модель блока по адресу 0x23b100000 (из DeviceTree)

Лог 0.4.52: `fmul` прошла (ядро пошло дальше, 936 млн инструкций), но теперь **настоящая** остановка: первое исключение — синхронный external abort (ESR 0x96000010) на чтении `ldr w0,[x8,w1,uxtw]` по виртуальному адресу страницы, отображённой на физический `0x23b100000` (w1 = 0, т. е. регистр со смещением 0). Там нет ни RAM, ни устройства (`accesses to physical addresses with no RAM and no device: 1`), ядро паникует (дальше второй-третий слой исключений при разборе паники: стек падает на неотображённую страницу — следствие, не причина). Что за устройство, я не утверждаю: в 0.4.53 эмулятор при старте спрашивает DeviceTree, чья `reg` из `/arm-io` покрывает этот адрес, и пишет имя узла, `compatible`, базу и размер в лог. Если узел найден — это **MODELLED** неизвестный блок: чтения возвращают 0, записи игнорируются, первые 40 обращений логируются с именем узла. ASSUMPTION: реальные регистры блока (и любое поведение прерываний, если это контроллер прерываний) здесь НЕИЗВЕСТНЫ. Это не исправление, а способ увидеть, что именно ядро читает и пишет. Тесты: поиск узла по адресу (граница диапазона, malformed), именованный блок с подписью в логе.

## 0.4.54 — кнопка Copy копирует по частям (≤ 19000 символов)

Все присланные логи имеют размер ровно 20000 байт (короткие, меньше 20000, приходят целыми). Значит, обрезает не буфер обмена Android, а вставка в чат: длинный текст превращается во вложение не длиннее 20000 символов. Вывод — по размерам файлов, прямой проверки у меня нет; поэтому Toast показывает, сколько символов скопировано. Самая важная часть (стоп-отчёт) в конце лога, и именно её обрезало. Теперь Copy копирует **конец** лога (до 19000 символов, по границе строки), следующее нажатие — часть раньше, и так по кругу; Toast пишет «часть N из M». Если лог короче 19000 — копируется целиком. Часть не должна быть длиннее лимита, склейка частей даёт исходный текст (тест `tests/java/CopyParts.java`, зеркало алгоритма; сам Activity здесь не компилируется).

## 0.4.55 — исправление: таблица устройств была переполнена (8)

Лог 0.4.53/0.4.54 (голова логов пришла текстом, хвост — вложением): DeviceTree нашёл узел `aic` (compatible `aic,1`, физ. 0x23b100000, размер 0x8000), и этот блок смоделирован (MODELLED, читает 0). Но первое исключение теперь снова другое: чтение физического `0x200000680` — блока контроллера памяти, который работал в 0.4.46–0.4.52. Причина — моя ошибка: в машине максимум `A64_MAX_DEVS` = 8 устройств (4 платы + 3 UART + aic = 8), девятое (0x200000000) молча не добавилось, и в логе пропала строка «unknown SoC register block». **Исправлено:** лимит 16; тест добавляет все 5 SoC-устройств рядом с платой (с лимитом 8 он падает, это проверено); при неудаче добавления в лог пишется WARNING, а не тишина. Поведение aic по-прежнему НЕИЗВЕСТНО, это заглушка «читает 0».

## 0.4.56 — настоящая паника ядра: AppleInterruptController (ipid-mask)

Лог 0.4.55 (голова + хвост, оба пришли): память 0x200000000 снова на месте, ядро дошло до **настоящей паники ядра** (не отказ эмулятора): `AppleInterruptController::start: device tree ipid-mask property length is %d but should be %d` (AppleInterruptController.cpp:128). Аргументы паники лежат в `x3[3]`, `x3[4]` (так же было в панике watchdog): **0x40** и **0**, т. е. в DeviceTree свойство `ipid-mask` длиной 64 байта, а ядро ожидало 0. Перед паникой оно прочитало у блока `aic` регистры +0x0 и +0x4 (дважды), получило 0 (наша заглушка) и вывело ожидаемую длину 0. Исправление (MODELLED, **ASSUMPTION**): регистр `aic` +0x4 теперь возвращает число прерываний = длина `ipid-mask` из DeviceTree × 8 = 512. Предположение, что +0x4 хранит число прерываний в младших 16 битах, взято из публичных исходников драйвера Linux для контроллера Apple (раскладка INFO); для этого ядра оно не проверено, а сама проверка ядра сверяется с тем же свойством DT, так что она пройдёт «по построению», это не доказательство соответствия железу. В логе загрузки печатаются все свойства узла `aic` (имя:длина). Если ядро ждёт другую раскладку, мы увидим новую панику с числами. Остальные регистры `aic` по-прежнему читаются как 0. Тесты: длина свойства узла по имени, фиксированное значение чтения (с подписью MODELLED в логе); найдена и исправлена ошибка: запись о блоке от уничтоженной машины с тем же адресом перехватывала настройку новой.

## 0.4.57 — aic пройден, следующий неизвестный блок 0x23d1f0000

Лог 0.4.56 (голова + хвост): паника `ipid-mask` прошла. Ядро записало в `aic` +0x5024 (значение 0x80000001) и 64 слова по +0x3000…+0x308c (нули) — это записи, мы их игнорируем (MODELLED). Затем **первое исключение — external abort** при чтении физического `0x23d1f0000` (`ldr w0,[x8,w1,uxtw]`, смещение 0; цепочка вызовов идёт из `0x8c135ac`). Что за узел, из лога не видно. Сделано: (1) тем же методом, что для `aic`, DeviceTree спрашивается, чей это адрес, и этот узел моделируется как неизвестный блок (читает 0, записи игнорируются, первые обращения в лог, **ASSUMPTION**: реальные регистры неизвестны); (2) в стоп-отчёт добавлена строка «that address belongs to DeviceTree node …» с именем, compatible, базой и смещением для первого обращения к неотображённому адресу, чтобы следующие такие остановки опознавались сразу, без отдельного шага. В загрузочном логе пишется число /arm-io-узлов, известных отчёту. Это не исправление, а диагностика + заглушка: если ядро ждёт в регистре статус, мы увидим цикл или новую остановку. Тесты: список узлов из дерева; отчёт (в режиме попытки) называет узел `wdt`.

## 0.4.58 — все узлы /arm-io как неизвестные блоки (изменение подхода, MODELLED)

Лог 0.4.57: `nub-gpio` (`gpio,t8015`, 0x23d1f0000) прошёл: ядро прочитало +0x0…+0x1c (нули) и записало +0x800/+0x840/+0x880 (0xffffffff). Следующая остановка: **запись** в физический `0x24ac04108`, узел `dapf-aop` (`dapf,t8020`), смещение +0x108 (стоп-отчёт назвал его сам). В дереве 263 узла `/arm-io` с `reg`; по одному кругу на узел это дни. **Изменение подхода:** каждый узел `/arm-io`, ещё не смоделированный, становится неизвестным нулевым блоком (чтение 0, запись игнорируется; 0.4.58 их добавляет сразу, кроме уже смоделированных, пустых, огромных и перекрывающихся). Каждый блок пишет в лог только первые 6 обращений, а стоп-отчёт перечисляет **все затронутые блоки** с числом чтений/записей и последним смещением. Цена: драйвер, который ждёт в регистре бит готовности, теперь не получит external abort, а зациклится; отчёт покажет, на каком блоке. Это **ASSUMPTION**: реальные регистры всех этих блоков НЕИЗВЕСТНЫ, нули могут быть неверны (в том числе делать «успех» там, где железо ответило бы иначе); поведение проверяется только по тому, как далеко пойдёт ядро. Лимит таблицы устройств 400. Тесты: 300 блоков помещаются, бюджет лога 6 обращений, сводка с числами.

## 0.4.59 — исправление: массовые блоки стирали значение `aic`

Лог 0.4.58: паника ядра `AppleInterruptController::start: ipid-mask length is 64 but should be 0`. Значение `aic +0x4 = 512` из 0.4.56 не применялось (чтение шло как «assumed»). Причина — моя ошибка в 0.4.58: цикл по всем узлам `/arm-io` повторно «добавлял» уже созданный блок `aic`; слот находился и обнулялся (вместе с заданным значением), затем добавление устройства отклонялось из-за перекрытия, но слот оставался пустым. Теперь при отказе слот восстанавливается. Тест: повторное добавление того же блока отклоняется, а заданное чтение сохраняется. ASSUMPTION про раскладку `aic +0x4` по-прежнему не проверена на железе.

## 0.4.60 — диагностика: AppleDAPF.cpp:158 (`dapf-aop`)

Лог 0.4.59: `aic` (ipid-mask) и `nub-gpio` пройдены, ядро записало 530 слов в `aic` и 3 в `nub-gpio`. Новый стоп: `(dapf-aop) AppleDAPF.cpp:158: REQUIRE failed` после 33 записей в блок `dapf-aop` и одного чтения `+0x0`. Что проверяет условие, неизвестно. Диагностика (не исправление): `dapf-aop` стал именованным блоком (логируются первые 40 обращений, видно, что именно читается и после каких записей), а стоп-отчёт выгружает 0x1c0 байт кода перед адресом возврата 0xfffffff00848e118 (из цепочки вызовов 0.4.59) для дизассемблирования. Значения регистров DAPF остаются неизвестными (MODELLED нули).

## 0.4.61 — исправление: блоки `dapf` читают обратно записанное

Лог 0.4.60 + дамп кода: `AppleDAPF.cpp:158` — это не загадочная проверка. Код перед паникой: `orr w8,wzr,#0x80000001; str w8,[x10]; ldr w9,[x10]; cmp w9,w8; b.eq ok` — ядро включает DAPF записью 0x80000001 в регистр `+0x0` и требует прочитать то же значение. Нулевые заглушки игнорировали запись, и проверка падала. Теперь блоки с `compatible` `dapf*` (`dapf-aop` и остальные) — регистры с read-back в первых 4 КиБ: чтение возвращает последнее записанное 32-битное значение (0, если записи не было). MODELLED/ASSUMPTION: это только read-back, реальное поведение фильтров DAPF не смоделировано. Тест: запись `0x1234` в `+8` читается обратно, незаписанный `+4` читается как 0.

## 0.4.62 — диагностика: ApplePMGR.cpp:843

Лог 0.4.61: DAPF пройден (чтение `+0x0` вернуло `0x80000001`). Дальше ядро обратилось к `smc-gpio`, `aop-gpio`, `mcc`, `gpio`, `pmgr`, `usb-device`, и новый стоп: `ApplePMGR: initDriver ... ApplePMGR.cpp:843 REQUIRE failed: _optionalBridges & (0x1 << i)`. Что это за маска и откуда она берётся, из лога не видно (возможно, из значений регистров `pmgr`, которые сейчас читаются как 0 — это только гипотеза). Диагностика, не исправление: окно кода 0x2c0 байт перед местом паники (адрес возврата 0xfffffff008170f90 из цепочки вызовов) и `pmgr` с логом первых 40 обращений. Окно DAPF убрано (DAPF решён).

## 0.4.63 — диагностика: откуда `_optionalBridges` в ApplePMGR

Лог 0.4.62 + дизассемблирование выгруженного кода (llvm-objdump): паника `ApplePMGR.cpp:843` — в `ApplePMGR::initDriver` цикл по мостам (число мостов `this+0xe38`): ядро ищет у провайдера свойство `bridge-settings`; если его нет — для каждого моста `bridge-settings-%d`; если нет и его — бит `i` в `_optionalBridges` (`this+0xe3c`) обязан быть установлен, иначе REQUIRE. Значит, либо в DeviceTree узла `pmgr` этих свойств нет, либо моё чтение их не находит, либо `_optionalBridges` должно было быть задано раньше. Диагностика, не исправление: в boot-лог выводится список свойств узла `pmgr` (имя:длина) и длина `bridge-settings` (-1 = нет), стоп-отчёт выгружает код `0xfffffff008170900..0x170ce0` (начало `initDriver`, откуда берутся число мостов и `_optionalBridges`; строки-ссылки подписываются). Лимит списка свойств для `node_info` поднят с 900 до 2800 символов.

## 0.4.64 — модель: мосты `pmgr` без `bridge-settings-<i>` помечаются как optional

Лог 0.4.63 + дизассемблирование: `ApplePMGR::initDriver` читает из узла `pmgr` `#bridges` (→ `this+0xe38`) и `optional-bridge-mask` (→ `this+0xe3c`), затем для каждого моста `i` требует либо свойство `bridge-settings-<i>`, либо бит `i` маски. В дереве из IPSW есть `#bridges` и `optional-bridge-mask`, но нет ни `bridge-settings`, ни `bridge-settings-<i>`; паника при `i = 0` (в регистрах первого исключения `x26 = 0`). Откуда настоящие настройки (iBoot? фьюзы?), неизвестно. MODELLED фикс (не то, что iBoot, как известно, делает): до копирования дерева в RAM каждый мост без `bridge-settings-<i>` помечается optional (его бит добавляется в маску, на месте), в boot-лог пишутся `#bridges`, старая и новая маска. ASSUMPTION: настоящих настроек мостов нет, выдуман только флаг optional. Тест: 3 моста, маска 0x2, `bridge-settings-1` есть → маска 0x7, идемпотентность, битое дерево.

## 0.4.65 — модель: пустое свойство `voltage-states1` в узле `pmgr` + диагностика

Лог 0.4.64: проверка мостов пройдена (маска `0xe00 → 0x3ffff`, 18 мостов без `bridge-settings-<i>`), новый стоп: `ApplePMGR.cpp:899  voltage-states1 not found`. В узле `pmgr` дерева из IPSW есть `voltage-states0`, `2` и `9`, но нет `1`; настоящая таблица, скорее всего, зависит от кристалла и добавляется iBoot — здесь неизвестна. MODELLED фикс (не то, что iBoot, как известно, делает): в `pmgr` добавляется свойство `voltage-states1` нулевой длины (оно существует, но пустое; значений напряжений не выдумано). ASSUMPTION: пустая таблица может вызвать следующую остановку в разборе — это и покажет лог. Диагностика: стоп-отчёт выгружает код `0xfffffff008170fc0..0x1712c0` (после цикла мостов: как ядро ищет и использует `voltage-states*`). Тесты: добавление пустого свойства, идемпотентность, неизвестный узел, обрезанное дерево.

## 0.4.66 — вкладка Kernel: текст из буфера сообщений ядра (MODELLED)

**Почему Kernel была пуста.** Вкладка показывает только то, что гость пишет в UART. С framebuffer в `boot_args.Video` XNU выводит текст `-v` на экран, а не в serial, поэтому до паники ничего не приходило. (Это моё объяснение по поведению XNU; журнал 0.4.65 его не опровергает, но напрямую не доказывает.)

**Что сделано.** XNU хранит всё, что напечатано через printf, в кольцевом буфере `struct msgbuf` (magic `0x063061`, size, bufx, bufr, bufc). Эмулятор находит заголовок в RAM гостя по magic (проверяет size, индекс и указатель на текст), и новый текст зеркалится в Kernel-вкладку (раз в 2 млн шагов; поиск при неудаче — раз в 40 млн; плюс при остановке). Только чтение памяти гостя, ничего не выдумывается. ASSUMPTION: раскладка структуры как в исходниках XNU; если заголовок не найден — вкладка останется пустой, а в Normal-вкладке не будет строки «kernel message buffer (msgbuf) found».

**Диагностика стопа.** 0.4.65: прошёл `voltage-states1`, новый стоп — `AppleS5L8960XWatchDogTimer.cpp:55 "clock speed 0 does not match absolute time"`. Добавлен лог свойств узлов `wdt` и `arm-io` и дамп кода перед вызовом panic (0xfffffff008ed2380..).

Тесты: 6 новых проверок msgbuf (пустой буфер, новый текст, без повторов, дописывание, wrap-around, отказ на неправдоподобном заголовке).

## 0.4.67 — исправление сборки на GitHub (тестовые скрипты)

CI падал не на коде эмулятора, а на тестовых скриптах: более новый `ld.lld` на runner'е отказывается линковать тестовые ELF с `-Ttext=0x10000` / `-Ttext=0` («section '.text' address is smaller than image base 0x200000; specify --image-base»). Во все вызовы `ld.lld` в `tests/run_all.sh` и `tests/diff/run_diff.sh` добавлен `--image-base=0`. Локально (lld 18) все тесты проходят; на более новом lld из CI я это проверить не мог, поэтому результат CI — проверка.

## 0.4.68 — значки Copy / Save в логе; копирование по частям убрано

- В панели логов вместо слова «Copy» — значок (два листа), рядом значок Save (дискета).
- **Copy** копирует весь лог показанной вкладки (Kernel или Normal) целиком. Если буфер обмена не принимает такой размер, выдаётся сообщение с советом использовать Save.
- **Save** открывает системный диалог «Сохранить как» и записывает весь лог показанной вкладки в `.txt` (UTF-8; имя по умолчанию `xnuron-kernel-log.txt` / `xnuron-normal-log.txt`).
- Копирование по частям (19000 символов) и его тест `CopyParts` удалены.
- Android-сборка (новый код Activity, два векторных значка) мной не проверялась: здесь нет Android SDK; ориентир — сборка на GitHub.

## 0.4.69 — разбор стопа AppleS5L8960XWatchDogTimer.cpp:55 (диагностика)

Разбор кода из лога 0.4.68 (llvm-objdump): `wdt` спрашивает у провайдера (`obj+0x88`) частоту часов с id из своего `clock-ids` (= 4) и сравнивает `частота × numer` с `denom × 10^9` из timebase; при несовпадении — panic. Здесь частота равна 0: метод провайдера (`+0x540`) смотрит в его таблицу (`+0xb8` указатель, `+0xc0` количество, `+0xc4` первый id), а в логе `+0xb8 = 0`, `+0xc0/+0xc4 = 0`, то есть провайдер ещё не заполнил таблицу частот. Почему — пока неизвестно (ASSUMPTION: она заполняется из `clock-frequencies` узла arm-io или другим драйвером, который к этому моменту не отработал; не проверено).
- Диагностика: в лог загрузки пишутся значения `clock-frequencies` узла `arm-io` (32-битные слова).
- Для следующего шага нужен лог вкладки **Kernel** (кнопка Save): там теперь текст самого ядра до паники.
- Ничего не изменено в поведении эмулятора; новая функция `a64_dt_node_prop_words` покрыта тестом.

## 0.4.70 — без всплывающих сообщений при Copy/Save; текст ядра в отчёте остановки

- Убраны всплывающие сообщения «Скопировано» и «Сохранено». Оставлены только сообщения об ошибках («Лог пуст», «Не удалось скопировать/сохранить») — иначе при сбое не было бы видно, что ничего не произошло.
- Отчёт остановки (вкладка Normal) теперь содержит и текст вкладки Kernel (последние 6000 байт, строки с префиксом `[kernel]`), поэтому достаточно сохранить один лог Normal. Тест: `spin.zip` проверяет строки `[panic] kernel text so far` и `[kernel] s`.
- Из лога 0.4.69: `clock-frequencies` узла `arm-io` в DeviceTree целиком нули (96 слов). Вероятно, это поле iBoot заполняет сам (как другие placeholder-значения); ядру для `wdt` нужна частота 24 МГц, равная частоте счётчика. Пока не изменено: сначала нужно увидеть текст ядра, чтобы понять, читает ли провайдер таблицу из DeviceTree вообще (в логе указатель таблицы был NULL).

## 0.4.71 — тише Normal-лог; префикс [stop]; текст ядра не дублируется в Normal

По просьбе пользователя:
- **Откат 0.4.70:** текст ядра больше не копируется в отчёт Normal (он остаётся только на вкладке Kernel; Save сохраняет вкладку).
- **Префикс `[panic]` переименован в `[stop]`**: это отчёт остановки эмулятора, а не паника гостя, и старое имя путало.
- **Убрана итоговая строка** «This is the emulator stopping where it lacks a feature … NOT iOS booting». Первая строка заголовка попытки (про «NOT iOS booting») оставлена.
- **Меньше шума:**
  - отчёт остановки не печатается второй раз после «EMULATOR FAULT» (раньше регистры, обходы таблиц, список функций и исключения шли дважды);
  - строки «Apple register … first read/write» заменены одной строкой `[stop] MODELLED Apple registers the kernel used (...)` с последними значениями; допущение про «ready»-бит остаётся отдельной строкой;
  - строки «unknown SoC block … read/write» заменены первыми обращениями в итоговой строке блоков (`first: r+0x680=0x0 w+0x688=0x1 ...`);
  - убраны логи свойств узлов `aic`, `pmgr`, `wdt`, `arm-io`, список свойств `/chosen`.
- Значения и поведение эмуляции не менялись; тесты обновлены под новый формат (проверяют итоговые строки, а не отдельные строки лога).

## 0.4.72 — вывод из Kernel-лога 0.4.71 и диагностика «clock frequencies not published»

Текст ядра (вкладка Kernel, msgbuf) наконец показал причину остановки на `AppleS5L8960XWatchDogTimer.cpp:55`: строка
`AppleT802xIO::start: clock frequencies not published in device tree`.
Драйвер `arm-io` (AppleT802xIO) считает, что DeviceTree не публикует частоты, поэтому его таблица частот (provider +0xb8) пуста, `wdt` получает 0 и паникует. В IPSW-дереве свойство `clock-frequencies` (384 байта) есть, но целиком из нулей: iBoot, судя по всему, заполняет его при запуске (ASSUMPTION; значения не известны, не выдумываются).

Ядро после этого ещё идёт (AppleCredentialManager, I2C, AES, KeyStore), т.е. останов позже, чем думал раньше; в конце текста ядра: `thread_invoke: preemption_level 1 ... hw_lock_bit(): timed out` — это следствие зацикливания обработчика паники.

Новое: диагностика `a64m_set_diag_string` — при остановке в образе ядра ищется текст, находится код, который на него ссылается (ADRP+ADD), и выводится окно кода вокруг первой ссылки. Включено для «clock frequencies not published in device tree» и «clock-frequencies». Тест с синтетическим образом: текст найден, ссылка найдена, отсутствующий текст сообщается. Поведение эмуляции не менялось.

## 0.4.73 — разбор кода AppleT802xIO::start (лог 0.4.72) и дамп метода, заполняющего таблицу частот

Диагностика 0.4.72 нашла код (llvm-objdump). Вывод:
- `AppleT802xIO::start` (на 0xfffffff00863d438) вызывает метод vtable +0x5b0 с аргументами `(this, 0, 1, 0x100)` и, если он вернул 0, печатает «clock frequencies not published in device tree».
- Этот метод (0xfffffff00814fc38, по аргументам) читает через `this+0x90` свойство `clock-frequencies` и ещё одно (строки 0x…f6b, 0x…f7d) как OSData, при флаге `x2 != 0` выделяет таблицу (`this+0xa8`, счётчик `this+0xa0 = 6`) и заполняет таблицу провайдера, из которой метод 0x814fa84 отдаёт частоту по clock-id (`this+0xb8` — таблица, `+0xc0` — количество, `+0xc4` — первый id).
- Почему он вернул 0 — по дампу 0.4.72 не видно (окно кончилось на 0x814fd14). 0.4.73 выгружает продолжение 0x814fd14..+0x3e0 и начало метода со строками.
- Убран ненужный дамп probe2 (≈25 строк). Поведение эмуляции не менялось (ASSUMPTION: метод и впрямь +0x5b0, вывод по аргументам вызова, не по таблице виртуальных функций).

## 0.4.74
- Лог 0.4.73 разобран: метод `0xfffffff00814fc38` (его вызывает `AppleT802xIO::start`) возвращает 0, если у узла `arm-io` нет **обоих** свойств `clock-frequencies` и `clock-frequencies-nclk`. В IPSW есть только `clock-frequencies` (384 байта, все нули); отсюда «clock frequencies not published in device tree».
- MODELLED: в `arm-io` добавляется `clock-frequencies-nclk` той же длины, заполненное нулями. ASSUMPTION: настоящие значения подставляет iBoot, здесь они неизвестны; частоты и типы часов НЕ выдуманы. Ожидается, что проверка watchdog (частота против таймбазы 24 МГц) всё ещё увидит 0 — дальше нужно понять раскладку таблицы.
- Тест на `a64_dt_node_add_zero_prop`.

## 0.4.75
- Лог 0.4.74: `could not add 'clock-frequencies-nclk' (-1)` — не хватило запаса в буфере DeviceTree (+256 байт, а свойство 384+36). Запас увеличен до 2048 байт. Остальное без изменений (стоп тот же: watchdog `clock speed 0`).

## 0.4.76
- Лог 0.4.75: `clock-frequencies-nclk` добавлен, стоп ушёл с watchdog дальше: теперь `ApplePMGR.cpp:899 "voltage-states5 not found"` (в pmgr нет voltage-states1 и 5; был добавлен только 1).
- MODELLED: все отсутствующие `voltage-states0..15` в узле `pmgr` создаются пустыми (0 байт), чтобы не падать на каждом индексе по одному за запуск. ASSUMPTION: настоящие таблицы даёт iBoot, значения НЕ выдуманы.
- Ядро теперь прошло: `AppleT802xIO::start`/частоты, много драйверов (msgbuf/Kernel tab — смотри там).

## 0.4.77
- Лог 0.4.76: ядро прошло `voltage-states5`; новый стоп — data abort в `0xfffffff0081713a0` (`ldr w8,[x0,x8]`, x0=0, индекс (0−1)·8 = `0x7fffffff8`). Код берёт «последний элемент» таблицы длиной 0: пустые `voltage-states<N>` (MODELLED из 0.4.76) для этого кода не годятся — нужна непустая таблица.
- Диагностика (поведение не меняется): дамп функции `0xfffffff008171290..0xfffffff008171550`, чтобы узнать размер записи и формат. Значения напряжений по-прежнему не выдумываются.

## 0.4.78
- Лог 0.4.77: функция `0xfffffff008171290` — перебор perf-состояний PMGR (строки: `perfDomain->voltageStateCount`, `perfStateMinVoltageStates`, `perfStateFrequenciesMHz`). Обращение `ldr w8,[x0, (count-1)<<3]`: записи таблицы `voltage-states<N>` по 8 байт, пустая таблица даёт индекс −1.
- MODELLED: недостающие `voltage-states<N>` теперь из ОДНОЙ записи в 8 нулевых байт (вместо пустых). Размер записи взят из кода ядра; смысл слов неизвестен, значения не выдуманы (нули).
- Дамп-окно диагностики остаётся.

## 0.4.79
- Диагностика «все недостающие свойства DeviceTree за один запуск» (поведение гостя не меняется). Эмулятор на каждом вызове (BL/BLR/BLRAA) читает строки в x1/x2; если строка похожа на имя свойства (строчные буквы/цифры, `-` `,` `#`, 4..39 символов) и такого имени нет среди свойств DeviceTree, она попадает в список (первые 256, с адресом вызова). Список печатается при стопе: `DeviceTree names probe (HEURISTIC)`.
- Ограничение: это эвристика; могут попасть ключи реестра, не свойства DT. Имена, собранные на лету (`voltage-states%d`), видны только если строка уже построена в момент вызова.
- Тест на пробу (`voltage-states5` один раз, имя из DT и «не имена» не попадают). Версия 0.4.79.

## 0.4.80
- Лог 0.4.79: стоп — снова watchdog (`clock speed 0 does not match absolute time`); это ожидаемо, таблица частот в DeviceTree всё ещё нули. Хвостовые `data abort` — следствие panic-обработчика, не причина.
- Список «имён не из DT» (0.4.79) почти весь шум: строки в x1/x2 — это куски имён, которые ядро копирует (memcpy/strcpy) при разборе самого DT, и значения `compatible`. Проба теперь учитывает только литералы из образа ядра и отбрасывает строки с запятой.
- Диагностика при загрузке (поведение не меняется): одна строка `frequency-like DeviceTree properties` — значения `timebase-frequency`, `clock-frequency`, `bus-frequency`, `fixed-frequency` и др. в `cpu0`/`arm-io`/`device-tree` и первые слова `clock-frequencies`. Нужно, чтобы понять, нули ли это (заполняет iBoot) и можно ли честно вывести частоту, которую требует проверка watchdog (таймбаза 24 МГц).

## 0.4.81
- Лог 0.4.80 (в `Normal` обрезан на проб-списке, хвост со стопом потерян): в IPSW-дереве из частотных свойств есть только `arm-io/usbphy-frequency=0`, `arm-io/soc-generation`, `device-tree/clock-frequency=0` и нулевой `clock-frequencies`. В пробе видно, что ядро ищет `bus-frequency`, `timebase-frequency`, `memory-frequency` — их в DT нет (их заполняет iBoot). Из этого следует, что глобалы частот ядра (gPEClockFrequencyInfo?), из которых `AppleT802xIO` строит таблицу часов, остаются нулями.
- Диагностика (поведение не меняется): дамп 5 глобалов с адреса `0xfffffff00914e128` (`+0x60..+0xd8`) и список свойств узла `cpu0`; список проб урезан до 48 имён и не печатает строки с точкой (раньше он съел остаток лога).
- Фикса пока нет: какое значение требует watchdog и какое свойство его даёт — сначала смотрим данные.

## 0.4.82
- Лог 0.4.81: окно глобалов ядра `0xfffffff00914e128` — `+0x10 = 0x16e3600` и `+0x48 = 0x16e3600` (24 МГц: dec/timebase; ядро взяло их из счётчика), а таблица часов `AppleT802xIO` читает `+0x60/+0x78/+0x90/+0xa8/+0xc0` (похоже на gPEClockFrequencyInfo: bus/cpu/mem/prf/fix, по тройке hz,min,max) — там нули. В `cpu0` нет ни одного `*-frequency` (список свойств в логе), их заполняет iBoot.
- MODELLED (ASSUMPTION): в `cpu0` добавляется `fixed-frequency` = 24000000 (u32). Основание: проверка watchdog требует частоту, равную таймбазе 24 МГц (которую эмулятор сам задаёт), а фиксированный генератор — самый вероятный источник. Если watchdog берёт другой clock, стоп повторится — в окне глобалов будет видно, какие ячейки заполнились. Остальные частоты (bus, memory, peripheral, cpu) не добавлены и не выдуманы.
- `a64_dt_node_add_prop` (свойство с данными) + тест.

## 0.4.83
- Лог 0.4.82: **watchdog пройден** — `fixed-frequency` = 24 МГц сработал (ячейка `+0xc0` глобалов частот стала `0x16e3600`). Новый стоп: `ApplePMGR.cpp:1303` `"%s %x request %s.%d > %d"` — устройство `ECPU0` (id 1) просит у `ECPU` состояние с индексом, большим числа записей. Мои однозаписные нулевые `voltage-states<N>` (0.4.78) для этого не годятся: настоящие таблицы заполняет iBoot (зависят от чипа).
- Диагностика (поведение не меняется): в стоп-отчёт попадают 16 слов за x3 (раньше 8 — нужны два последних числа `%d > %d`), и при загрузке печатаются длина и первые слова настоящих `voltage-states0/2/9` из IPSW (по ним видно формат записей и число состояний).

## 0.4.84
- Лог 0.4.83: настоящие таблицы IPSW: `voltage-states0(24 байта)=0 0 1 0 2 0`, `voltage-states2(32)=0 0 1 0 2 0 3 0` — записи по 8 байт `{индекс состояния, 0}` (напряжения iBoot вписывает сам), т.е. 3 и 4 состояния. Аргументы паники `ApplePMGR.cpp:1303`: `ECPU0`, id `1`, `ECPU`, `%d = 0`, `%d = 0` («request ECPU.0 > 0»).
- Диагностика (поведение не меняется): дамп кода `0xfffffff008172480..0xfffffff008172720` перед вызовом panic в `ApplePMGR::initDriver` — понять, что сравнивается (число состояний, максимальный индекс?). Окно данных частот убрано (частоты разобраны).

## 0.4.85
- Лог 0.4.84 разобран (код `ApplePMGR::initDriver`, `0x8172470..0x8172730`): для каждого устройства из `devices` (запись 0x30 байт: `+1` = запрошенный индекс состояния, `+2` = индекс домена) ядро берёт домен перф-состояний (запись 0xf0) и требует `[домен+8]` (число состояний) **больше** запрошенного индекса; иначе `ApplePMGR.cpp:1303` `"ECPU0 1 request ECPU.0 > 0"`. Число состояний домена ECPU = 0: в IPSW есть пустая (0 байт) `voltage-states<N>`.
- MODELLED: существующие, но ПУСТЫЕ `voltage-states<N>` (0..15) теперь получают одну запись `{0,0}` (8 нулевых байт, как первая запись настоящих таблиц `0 0 / 1 0 / 2 0`). Значения не выдуманы. `a64_dt_node_fill_empty_prop` + тест.
- Настройки: **разрешение экрана** (270×480 / 540×960 по умолчанию / 1080×1920), применяется со следующего Start. Это размер фреймбуфера в `boot_args.Video` (MODELLED; у настоящего iPhone XS 1125×2436). `a64m_set_video_size`, JNI `setVideoSize`, `EmuView.resize`, тест.

## 0.4.86

- Лог 0.4.85: паника ApplePMGR (ECPU) пройдена, ядро дошло до сторожевого таймера (AppleS5L8960XWatchDogTimer.cpp:55). В аргументе паники `0x13016e3600`: младшие 32 бита = 24000000, старшие `0x13` — соседние байты дерева. Ядро читает `fixed-frequency` как 64-битное число.
- Исправление (MODELLED): `fixed-frequency` у cpu0 теперь 8 байт (u64 = 24000000), значение то же, без выдумок.

## 0.4.87

- Лог 0.4.86: сторожевой таймер пройден (`fixed-frequency` теперь 8 байт). Остановка снова `ApplePMGR.cpp:1303`: аргументы паники — домен `ECPU`, запрошено состояние 0, число состояний домена 0 (`[domain+8]`, домены на `this+0xe50`, шаг 0xf0, код дизассемблирован). Сообщения о заливке пустых таблиц в логе нет — значит, гипотеза «пустая `voltage-states<N>`» для ECPU была неверной, и число состояний берётся не оттуда (или таблица не для того индекса).
- Диагностика (ничего не меняет): стоп-отчёт выгружает код `0xfffffff008171290..0x171690` (функция со строками `perfDomain->voltageStateCount`, `perfStateFrequenciesMHz`), чтобы найти, где пишется `[domain+8]`.

## 0.4.88

Разбор дампа кода из лога 0.4.87 (llvm-objdump, функция `0xfffffff0081712xx..`, разбор `voltage-states<N>` одного perf-домена):
- число состояний домена (`[domain+8]`) зависит от байта «типа» домена (`entry+2`): тип 0 — индекс последней записи + 1 (или число записей, если второе слово первой записи ненулевое); тип 1 (ECPU) — число ПОДРЯД идущих записей с ненулевым первым словом (остановка на нуле).
- моя заглушка `{0,0}` для типа 1 даёт 0 состояний → паника `ApplePMGR.cpp:1303` (ECPU, запрошено 0, есть 0). Это объясняет, почему пустые таблицы оказались ни при чём.

Исправление (MODELLED, **PLACEHOLDER**): добавляемые таблицы теперь содержат одну запись `{1,0}`. Единица — НЕ настоящее значение: реальные данные CPU (частоты/напряжения) iBoot берёт из чипа, их нет в IPSW. Заглушка лишь даёт счёт 1. Производная частота этого состояния бессмысленна. Могут появиться следующие паники из-за этого.

## 0.4.89

- Лог 0.4.88: паника ApplePMGR.cpp:1303 пройдена (заглушка `{1,0}` сработала). Новый стоп: `ApplePMGRMTRSensor.cpp:131` — `agingRefDataFromProperty`: «propery mtr_0 missing in devicetree». Это драйвер датчика старения (MTR) из PMGR; нужное свойство в дереве IPSW не найдено.
- Диагностика (ничего не меняет): в boot-лог выводится список узлов и свойств дерева, в имени которых есть `mtr`, `aging` или `sensor`, чтобы увидеть, что в дереве есть на самом деле (имя узла/свойство/длина). Значения НЕ выдумываются: если свойство — калибровочные данные чипа, которые iBoot берёт из фьюзов, подставлять его нельзя без явной пометки.

## 0.4.90

- Лог 0.4.89: стоп тот же (`ApplePMGRMTRSensor.cpp:131`, `mtr_0`). Список обрезался на `tempsensor3`: слово `sensor` дало 85 совпадений и забило 3000 символов до узла pmgr. Видны только `ppm/enable-aging-model`, `tempsensor*`, `isp/*`, `sgx/*`; свойств с `mtr` среди увиденных нет.
- Диагностика (ничего не меняет): фильтр сужен до `mtr` и `aging`.

## 0.4.91

- Лог 0.4.90: в дереве есть узел `arm-io/mtrtempsensor3`, но свойств с `mtr`/`aging` нет (кроме `ppm/enable-aging-model`). Значит `mtr_0` нет в дереве IPSW (его, вероятно, добавляет iBoot из данных чипа). Размер и смысл свойства неизвестны — подставлять вслепую нельзя.
- Диагностика (ничего не меняет): стоп-отчёт выгружает код `0xfffffff008ce1680..0x8ce1800` (функция с паникой `agingRefDataFromProperty`) и `0xfffffff008ce9180..0x8ce9200` (вызывающий), чтобы увидеть, как используются имя и размер свойства.
- Добавлена строка скорости в стоп-отчёт: `[emu] speed: ... миллионов инструкций/с`, чтобы оптимизировать по измерениям, а не вслепую.

## 0.4.92

- Появился headless-запуск без телефона: `.github/workflows/boot.yml` собирает те же C-исходники в консольную программу (`tools/boot_cli.c`), скачивает с публичного сервера Apple тот IPSW, у которого ядро в архиве 16875356 байт (`tools/find_ipsw.py`), и прогоняет попытку загрузки; лог публикуется как релиз `boot-log`. Проверка: запуск воспроизвёл ту же панику (`mtr_0`) и те же 5387 байт вывода гостя, что и телефон. Скорость на раннере: 31.9 млн инструкций/с.
- Разбор дампа кода (0.4.91): `ApplePMGRMTRSensor::agingRefDataFromProperty(имя, "rosc-mtr_0")` ищет свойство `mtr_0` у провайдера, требует `data[0]==1` и длину `(3*data[1]+2)*4` байт.
- Исправление (MODELLED, PLACEHOLDER): узлу `pmgr` добавлено `mtr_0 = {1,0}` (версия 1, ноль записей) — минимум, который проходит проверку ядра; это НЕ данные калибровки чипа. ASSUMPTION: провайдер — `pmgr` (не проверено).

## 0.4.93

- Headless-запуск 0.4.92 показал: ядро приняло `mtr_0` на узле `pmgr` (провайдер — `pmgr`, подтверждено) и спросило `mtr_1`. Теперь добавляются `mtr_0..mtr_15` (каждое `{1,0}`, PLACEHOLDER, не данные калибровки). Запас буфера дерева поднят до 8192 байт.

## 0.4.94

- Headless-запуск 0.4.93: `mtr_0..mtr_15` приняты, новый стоп `AppleT8020PMGR.cpp:568` (`calculateCPUEnergyAccumulatorsTuning`): «ifane-max for freqHz=24000000 not found». Частота 24 МГц — моя `fixed-frequency`; ядро ищет по ней запись таблицы ifane-max (настройка счётчиков энергии CPU).
- Диагностика (ничего не меняет): дамп кода `0xfffffff008ce4f80..0x8ce5120`, поиск в дереве свойств с `ifane`/`energy`/`tuning`.

## 0.4.95

- Headless 0.4.94: в дереве есть `pmgr/ifane-max` (144 байта) и `pmgr/energy-counters` (440). Дамп кода: ядро считает частоту CPU из регистра PLL (бит 21 = включён, множитель `(reg>>4)&0x1ff`, делитель `(reg&0xf)+1`, иначе 24 МГц кварца) и ищет её в таблице `ifane-max` (записи по 16 байт, первое слово — float частоты). Регистр читается из нулевого блока pmgr, потому получилось 24 МГц.
- Диагностика (ничего не меняет): слова `ifane-max` и `energy-counters` в boot-лог; дамп кода `0xfffffff008ce4d00..0x8ce4f80` (откуда берётся регистр).

## 0.4.96

- Диагностика (ничего не меняет): стоп-отчёт показывает последние 24 чтения из неизвестных блоков MMIO (блок+смещение=значение), без опроса контроллера памяти; видно, какой регистр ядро прочитало прямо перед паникой. Нужна для паники `ifane-max for freqHz=…` (частота CPU считается из регистра PLL).

## 0.4.97

- Headless 0.4.96: перед паникой `ifane-max` ядро читает регистры блока pmgr со смещениями 0x40, 0x60 … 0x1e0 (шаг 0x20), затем 0xbc030/0xbc034; все нули.
- Диагностика (ничего не меняет): в списке чтений указан физический адрес блока (`'pmgr'@адрес+смещение`); дамп кода `0xfffffff00817c138..0x8c2f8` (функции, считающие частоту из регистров).

## 0.4.98

- Разбор (headless 0.4.97): `0x817c138` возвращает второе слово записи `voltage-states` (напряжение), частота CPU считается из регистра `+0x40` блоков `pmgr` 0x210e70000 и 0x211e70000 (кластеры); нулевое чтение даёт 24 МГц, которых нет в `pmgr/ifane-max` (504…2496 МГц).
- ЭКСПЕРИМЕНТ (MODELLED, не проверено): эти регистры читаются как `0x200150` (PLL включён, множитель 21, делитель 1): 21 × 24 МГц = 504 МГц, первая запись родной таблицы `ifane-max`. ASSUMPTION: раскладка битов прочитана из кода ядра, кварц 24 МГц. Если следующая паника покажет другое `freqHz`, шкала неверна — по числу уточним.

## 0.4.99

- Headless 0.4.98: PLL-эксперимент сработал (паника `ifane-max` пройдена). Новый стоп `ApplePMGR.cpp:3084` `perfStateToVoltage`: «state < domain->perfStateCount» для PCPU: у заглушки одно состояние.
- Заглушки `voltage-states<N>` теперь из 9 записей `{1,0}` (9 = число строк родной `ifane-max`). PLACEHOLDER, не настоящие напряжения/частоты.

## 0.4.100

- Headless 0.4.99: тот же assert 3084, `state` дошёл до 11: ядро перечисляет состояния CPU по регистрам блоков pmgr 0x210e70000/0x211e70000 (`+0x40`, `+0x60`, … шаг 0x20, по одному на состояние) до повтора первого значения; у меня был ненулевой только первый регистр, остальные — нули (не повторяют первый) → лишние состояния.
- ЭКСПЕРИМЕНТ (MODELLED): регистры `+0x40..+0x120` читаются как PLL-слова (бит 21, множитель 21/31/49/71/89/99/102/104, делитель 1) = 504/744/1176/1704/2136/2376/2448/2496 МГц — строки родной `ifane-max` (2436 пропущена: не кратна 24 МГц); `+0x140` повторяет первое слово как маркер конца. ASSUMPTION: раскладка из кода ядра.

## 0.4.101

- Headless 0.4.100: регистры читаются как заданы (+0x40…+0x120 разные, +0x140 повторяет первое), но цикл не остановился: следом читается +0x160 и assert 3084 с `state`=11. Условие `cmp x24,x23; b.eq 0x8ce5294` — не «выход», как я предположил.
- Диагностика (ничего не меняет): дамп кода `0xfffffff008ce5120..0x8ce52e0` (хвост цикла).

## 0.4.102

- Разбор хвоста цикла (0.4.101): для каждого из двух кластеров ядро читает регистры `+0x40…+0x1e0` (14 штук, шаг 0x20); регистр, равный первому, — неиспользуемый слот (пропуск), любой другой — состояние производительности `индекс-2`. Мои регистры `+0x160…` были нулями (≠ первому) → лишние состояния → assert 3084.
- ЭКСПЕРИМЕНТ (MODELLED): состояний 4 (множители 21/31/49/71 = 504/744/1176/1704 МГц — первые строки родной `ifane-max`; 4 = размер большей реальной таблицы voltage-states2), остальные слоты повторяют первое слово.

## 0.4.103

- Headless 0.4.102: кластер 0 пройден; для кластера 1 (P) паника `ifane-max for freqHz=252000000`: ядро считает множитель × 12 МГц (21 → 252 МГц), а не × 24. Сверка с `ifane-max` только у кластера 1.
- Для блока 0x211e70000 множители удвоены (42/62/98/142 → 504/744/1176/1704 МГц). MODELLED, ASSUMPTION как раньше.

## 0.4.104

- Headless 0.4.103: паника по `ifane-max` пройдена (P-кластер, множитель × 12 МГц подтверждён). Новый стоп: `AppleMTRPolynomT8020.cpp:1203`, `MTRROCalibrate`: REQUIRE `ret == kIOReturnSuccess`; в аргументах `fff0000000000000` (double −inf?). Возможная причина — пустые данные `mtr_N = {1,0}` (ноль записей).
- Диагностика (ничего не меняет): дамп кода `0xfffffff008cdfc80..0x8cdff40`.

## 0.4.105

- Диагностика: дамп кода подгоночной функции MTRROCalibrate (0x8cdf408..0x8cdfc80), чтобы выяснить, какие калибровочные данные ей нужны. Поведение эмуляции не менялось.

## 0.4.106

- Дамп 0.4.105 показал: подгоночная функция MTRROCalibrate распаковывает битовые поля из mtr_N, нулевое поле = ошибка. Пустые {1,0} не годились.
- mtr_0..15 теперь {1, 3, 9 x 0x5a5a5a5a} (44 байта) — PLACEHOLDER, значения бессмысленны, это не настоящая калибровка. Эксперимент: проходит ли подгонка.

## 0.4.107

- Headless-прогон 0.4.106: паника та же, в коэффициентах inf/NaN. Данные mtr_N не причина: подгонка делит на (s1 - s0), т.е. два float-аргумента MTRROCalibrate, вероятно, равны (0). Диагностика: дамп тела MTRROCalibrate, чтобы найти источник этих float. Поведение не менялось.

## 0.4.108

- Разбор дампа 0.4.107: MTRROCalibrate делит на (v1 - v0), оба float приходят от вызывающей функции (адрес возврата 0x8ce4348). Диагностика: дамп этой вызывающей функции, чтобы найти источник float (какое свойство DT/регистр). Поведение не менялось.

## 0.4.109

- Дамп 0.4.108: float-аргументы подгонки и 7 слов данных читаются из регистров pmgr-блока 0x23d200000 (+0xbc088..+0xbc0c4, фьюзы чипа). Они читались как 0 -> деление на ноль (inf/NaN).
- MODELLED/PLACEHOLDER: +0xbc0a8..+0xbc0c0 = 0x5a5a5a5a, +0xbc088/8c/90 дают float 25.0 и 105.0. Это не настоящие значения чипа, эксперимент.

## 0.4.110

- Headless 0.4.109: MTR-подгонка прошла (коэффициенты считаются). Новый стоп: ApplePMGR::waitReg32 (ApplePMGR.cpp:3241) ждёт (reg & 0xf0) == 0xf0 на pmgr 0x23b000000+0x802d8 после запроса состояния 0xf.
- MODELLED/ASSUMPTION: в блоках pmgr записанный регистр читается с битами 7:4 = битам 3:0 (запрошенное -> текущее состояние питания). Реальные задержки/отказы не моделируются. Тест добавлен.
- Исправлено: записи неизвестных блоков разрушенной машины больше не наследуются новой машиной с тем же адресом.

## 0.4.111

- Headless 0.4.110: новый waitReg32 на pmgr 0x23b000000+0x98000: ждёт (reg & 0x10) == 0, получает 0xff. Диагностика: дамп вызывающего кода (0x817c7f8, 0x8ce46dc). Поведение не менялось.

## 0.4.112

- Дамп 0.4.111: код 0x8ce4604 ставит младший ниббл pmgr 0x23b000000+0x98000 в 0xf и ждёт (reg & 0x10) == 0 — бит 4 это флаг занятости, а не «текущее состояние».
- MODELLED/ASSUMPTION: регистр читается как 0x0f. Реальное поведение неизвестно.

## 0.4.113

- Headless 0.4.112: та же функция (две ветки) ждёт бит 4 и на pmgr +0x98004. MODELLED/ASSUMPTION: +0x98004 тоже читается как 0x0f.

## 0.4.114

- Headless 0.4.113: следующий waitReg32 на pmgr +0x80168 (ждёт (reg & 0xf0) == 0xf0, читает 0). Причина в моей же модели: лимит 64 записанных регистров на блок исчерпан (доменов питания больше). Лимит поднят до 512.

## 0.4.115

- Headless 0.4.114: PMGR пройден, ядро дошло до драйверов (DieTemp, DWI, MIPI DSI, AGX, Haptics). Первое исключение: запись 0xC5ACCE55 (ключ CoreSight LAR) в физический 0x210010fb0 — там нет ни RAM, ни устройства; блок отладки CPU. Следующий шаг — найти, чем он описан в DT.
- Диагностика: DT-поиск теперь по coresight / impl-reg / debug / uttdbg / reg-private и показывает первые 6 слов значения. Поведение не менялось.

## 0.4.116

- cpuN/reg-private (0x210010000, 0x210110000, ... 6 ядер) добавлены как неизвестные блоки MMIO (читается 0, запись игнорируется). MODELLED/ASSUMPTION: размера в DT нет, взято 0x10000. Причина: запись ключа CoreSight 0xC5ACCE55 в 0x210010fb0.

## 0.4.117

- Headless 0.4.116: 0x210010fb0 принято, но ядро идёт дальше по блокам отладки: следующая запись в 0x210030fb0 (регистр x8=0x210034000 — рядом ещё один блок). В DT его нет среди reg-private.
- Диагностика: дамп всех свойств узлов cpu0 и cpu-debug-interface (имя, длина, первые слова), чтобы найти, чем описан 0x210030000. Поведение не менялось.

## 0.4.118

- Дамп DT 0.4.117: у cpu0 только reg-private (0x210010000); в cpu-debug-interface нет адреса 0x210030000. Ядро пишет и в base+0x20fb0.
- MODELLED/ASSUMPTION: блоки cpuN/reg-private расширены с 0x10000 до 0x40000 (размера в DT нет). Диагностика: дамп кода функции, пишущей ключ CoreSight (0x7b9d680).

## 0.4.119

- Ядро (0.4.118) дошло до ожидания корневого диска (Waiting on IOMedia Apple_HFS, затем Still waiting for root device) и крутится, драйвера накопителя в логе не видно.
- Диагностика (поведение не менялось): DT-поиск по nvme/nand/ans/sep/storage/nvram и дамп свойств узлов ans, sep, nvme-mmu0, nand — чтобы понять, какой накопитель описывает DT и сколько работы нужно для его модели.

## 0.4.120

- Инструмент: tools/ipsw_report.py (только чтение) перечисляет файлы IPSW и показывает, есть ли у образов Firmware/* блок KBAG (т.е. зашифрованы ли). Нужно, чтобы честно оценить, можно ли вообще использовать прошивку сопроцессора ANS. Расшифровка не выполняется. headless-workflow публикует ipsw.txt.

## 0.4.121

- Отчёт IPSW: в Firmware/ нет прошивки ANS (она не в IPSW). Есть два restore-ramdisk (IMG4, ~107 МБ) и rootfs-DMG 3,3 ГБ. Отчёт теперь разбирает IM4P (тип, размер полезной нагрузки, наличие KBAG = шифрование, сжатие), только чтение.

## 0.4.122

- Отчёт IPSW (0.4.121): прошивки ANS в IPSW нет; два restore-ramdisk (rdsk, ~107 МБ) НЕ зашифрованы (нет KBAG) и не сжаты.
- MODELLED (так делает iBoot при восстановлении): Restore RAM-диск из BuildManifest.plist (RestoreRamDisk) загружается в память между DT и topOfKernelData, адрес и размер пишутся в /chosen/memory-map RAMDisk, в boot-args добавлено rd=md0. RAM гостя растёт на размер диска. Ничего не расшифровывается и не обходится. Цель: дать ядру честный корневой диск вместо ожидания ANS.
- Тесты: a64_dt_patch_u64x2, a64_plist_first_path.

## 0.4.123

- Headless 0.4.122: путь после ключа RestoreRamDisk в BuildManifest указал на Firmware/ane/... (моя ошибка разбора: у записи RestoreRamDisk нет Path сразу после ключа). Отчёт IPSW теперь печатает фрагмент манифеста (без бинарных данных), чтобы разобрать структуру. Только чтение.

## 0.4.124

- Манифест: ключ RestoreRamDisk сначала встречается как строка-политика (Production), настоящая запись — словарь с Info/Path (048-15811-206.dmg для первого описания). Разбор берёт только вхождение со значением-словарём. Тест на этот случай.

## 0.4.125

- Headless 0.4.124: RAM-диск передан (rd=md0, memory-map RAMDisk, 101 МиБ), но ядро всё равно ждёт IOMedia Apple_HFS и пишет Still waiting for root device. Диагностика (поведение не менялось): строки ядра про RAMDisk / md0 / rd= / rootdev / root-matching, чтобы понять, чем именно ядро ищет корень.

## 0.4.126

- Диагностика 0.4.125: в ядре есть mdevadd/RAMDisk/rootdev/root-matching. Теперь ищутся ссылки кода (ADRP+ADD) на строки RAMDisk и root-matching и дампятся окна кода вокруг первых попаданий, чтобы увидеть, как ядро выбирает корень. Поведение не менялось.

## 0.4.127

- Ссылки на RAMDisk (0x80e9dec) и root-matching (0x80e9ba0) лежат в одной функции. Прогон больше не падает, поэтому дамп окна идёт прямо из образа ядра (строки «[stop] code ... kernel image dump»). Поведение не менялось.

## 0.4.128

- Диагностика: строки, на которые ссылается код выбора корня (±0x600 вокруг ссылок на RAMDisk/root-matching) — какие свойства DT и boot-args он читает. Поведение не менялось.

## 0.4.129

- Разбор функции выбора корня (IOFindBSDRoot): читает /chosen/memory-map RAMDisk, зовёт mdevadd(-1, base>>12, size>>12, 0), затем проверяет, что rd начинается с «md»+цифра. Следующий кусок (после 0x80e9fac) дампится, чтобы увидеть, что происходит с md-веткой. Поведение не менялось.

## 0.4.130

- Разбор (0.4.129): ветка md в выборе корня не входит (нет ни «BSD root: md0», ни ошибки про rd=), значит rdBootVar не содержит md0: rd=md0 из boot_args до ядра не доходит. Диагностика: свойства узлов /chosen и memory-map из DT (есть ли свой boot-args, что лежит в memory-map). Поведение не менялось.

## 0.4.131

- DT (0.4.130): в /chosen нет boot-args; memory-map RAMDisk = {0x43308000, 0x651aa00} на месте. Код выбора корня вызывает PE_parse_boot_argn("rd") и не находит rd=md0, хотя -v из той же строки работает. Дамп функции PE_parse_boot_argn (0x810b9d4) и строк, на которые она ссылается. Поведение не менялось.

## 0.4.132

- ИСПРАВЛЕНИЕ (моя ошибка с самого начала): boot_args.CommandLine лежит по смещению 0x6c = 108 (сразу за deviceTreeLength), а не 112. Это видно из PE_parse_boot_argn_internal самого ядра (читает [bootArgs + 0x6c]). До 0.4.131 строка писалась на 4 байта дальше, и ядро ВСЕГДА видело пустую командную строку: ни «-v serial=3 debug=0x14e», ни rd=md0 до него не доходили.
- Теперь передаётся «-v rd=md0» (без rd — «-v»). Прежние serial=3 debug=0x14e не возвращены: они ни разу не действовали, поведение с ними не проверено.

## 0.4.133

- Результат 0.4.132: «BSD root: md0, major 2, minor 0» и «hfs: mounted Peace16A366.arm64eCustomerRamDisk on device b(2, 0)» — корень смонтирован с restore-ramdisk. После этого ядро только печатает TempSensor; за 15 млрд шагов ничего нового.
- Инструмент (headless): в конце прогона печатается состояние CPU (EL, pc, daif, idle, исключения, последние pc), чтобы понять, что делает ядро после монтирования. Приложение не менялось.

## 0.4.134

- Состояние CPU в конце headless-прогона (6 млрд шагов): EL1, pc в цикле из 6 инструкций 0x863fe1c..0x863fe30 (вызов из 0x863fc90), 37780 FIQ-прерываний таймера, idle=0. Диагностика: дамп этого кода и строк, на которые он ссылается. Поведение не менялось.

## 0.4.135

- Код 0.4.134: ядро (_mccEnableCacheMode) пишет режим кэша в контроллер памяти +0x1c00 и в цикле ждёт, пока +0x1c04 (+0x40000 на канал, 4 канала) покажет тот же режим в битах 14:10 и 9:5. Блок читался как 0, из-за чего 6 млрд шагов крутился цикл из 6 инструкций.
- MODELLED/ASSUMPTION: после записи режима v в +0x1c00 регистры +0x1c04 + 0x40000*k читаются как (v<<10)|(v<<5). Реальные задержки и отказы не моделируются. Тест добавлен.

## 0.4.136

- Результат 0.4.135: цикл у контроллера памяти пройден; к 8 млрд шагов CPU в другом месте (pc 0x7a6ea60, прерывания замаскированы, x0=feedfacefeedfacf). Инструмент: headless печатает pc каждые 500 млн шагов (видно, движется ли ядро). Приложение не менялось.

## 0.4.137

- Заметил (по лог-файлам и скриншоту): строка «AppleT8015TempSensor::_timeoutOccurredGated: sensor off» заполняет 1 МиБ лога и не даёт читать остальное.
- Логи (обычный и kernel) теперь схлопывают одинаковые подряд идущие строки гостя: «[last line repeated N more times]» (заметка выводится при смене строки, каждые 256 повторов и в конце прогона). Только отображение логов: ядро и модель не менялись; текст, который ядро рисует в своём framebuffer, на экране по-прежнему идёт как есть. Тест добавлен.
- Результат прогона 0.4.136 (14 млрд шагов): после монтирования ramdisk ядро крутится в области 0x7a08820..0x7a6eaa8 с замаскированными прерываниями, 52 тыс. FIQ таймера, новых сообщений нет.
- Диагностика (поведение не менялось): дамп кода и строк в областях 0x7a08800..0x7a09100 и 0x7a6e9f0..0x7a6eab0, где ядро крутится после монтирования.

## 0.4.138
- Диагностика (только headless, поведение эмуляции не меняется): гистограмма входов в функцию переключения потоков 0x7a08820 по паре (lr, x1) — чтобы понять, какой код и какие потоки бесконечно крутят планировщик после монтирования ramdisk. Включается переменной `XN_HIST_PC`, печатается строками `[hist]` в boot.err.

## 0.4.139
- Исправлена ошибка в диагностике 0.4.138: адрес для гистограммы передавался без старших бит (0xfffffff0…), поэтому счётчик не срабатывал. Эмуляция не менялась.

## 0.4.140
- Диагностика: гистограмма 0.4.139 показала, что ядро ~13 млн раз заходит в 0x7a08820 из idle-цикла планировщика (0x7a6ea60) — то есть крутится idle-поток. Теперь дампится код самой функции простоя (0x7b9d300..0x7b9d700), чтобы увидеть, на чём она ждёт (WFI / регистры Apple). Эмуляция не менялась.

## 0.4.141
- Диагностика: счётчики WFI/WFE (сколько выполнено и сколько реально усыпили CPU) — в `[cpu]` headless-лога; дамп функции 0x7bb21d0, которую вызывает процедура простоя ядра. Идея: ядро 13 млн раз заходит в idle, но пропуск времени (cnt_skip) не растёт — значит, WFI либо не выполняется, либо тут же возвращается. Эмуляция не менялась.

## 0.4.142
- Диагностика: поиск по DeviceTree переключён на узлы/свойства с 'temp', 'tmps', 'thermal', 'tsens' — чтобы найти регистры и прерывания датчика температуры (AppleT8015TempSensor, строка «sensor off» в логе). Эмуляция не менялась.

## 0.4.143
- Диагностика датчика температуры: дамп свойств узлов DT `tempsensor0` и `dietempctl0` и код ядра вокруг строки «sensor off» (ADRP+ADD-ссылки). Нужно, чтобы по коду драйвера понять, какие регистры/прерывание он ждёт, прежде чем эмулировать ответ датчика (MODELLED). Эмуляция не менялась.

## 0.4.144
- Ответ датчика температуры (MODELLED, значения PLACEHOLDER): по коду ядра (0.4.143) «sensor off» печатается, когда бит0 регистра status (+0x10 по DT sensor-offset-status) равен 0. Блоки `tempsensorN` теперь отвечают как работающий датчик: status=1, readBK (+0x20000/+0x20004) = бит31 «valid» | raw, readSum (+0x28) = 4·raw, readCnt (+0x30) = 4. Сырое значение температуры и единицы ВЫДУМАНЫ (raw=0), реальный датчик не эмулируется; это эмуляция устройства на уровне регистров, а не подделка загрузки.
- Диагностика: дамп константы double, которой драйвер переводит значение в температуру (0x706f4a0), чтобы потом выбрать правдоподобное raw.

## 0.4.145
- Исправление 0.4.144: блоки `tempsensorN` лежат ВНУТРИ блока `pmgr` (0x23b288000 в диапазоне 0x23b000000…), поэтому отдельные блоки пропускались и датчик по-прежнему «off». Теперь ответ датчика — окно внутри pmgr-блока (`a64m_unknown_add_tempsensor`, MODELLED/PLACEHOLDER): status(+0x10)=1, readBK(+0x20000/+0x20004)=valid|raw, readSum=4·raw, readCnt=4.
- raw = 0x8c0 (≈35 °C): из кода ядра температура = raw·100·(1/64) (константа double 0x3f90000000000000 найдена дампом 0.4.144). Единица 1/64 °C — ASSUMPTION, значение выдумано.
- Тест: окно датчика в pmgr-блоке добавляется, для неизвестного блока отказ.

## 0.4.146
- Датчик температуры в 0.4.145 всё ещё писал «sensor off»: драйвер проверяет бит0 регистра, чьё смещение лежит в поле структуры +0x130, и какой это из DT-регистров (CTL0-set 0, CTL0-clr 4, CTL1 8, CTL2 0xc, status 0x10) из кода однозначно не видно. ASSUMPTION: все пять читаются как 1 (MODELLED, PLACEHOLDER). Если «off» исчезнет — значит один из них верный.

## 0.4.147
- Диагностика датчика температуры: протокол первых ~90 обращений драйвера к окнам датчиков (чтение/запись, номер окна, смещение от начала окна, значение), строка `[tempsensor]` в boot.err. Нужен, чтобы увидеть, какой регистр драйвер реально читает перед печатью «sensor off». Эмуляция не менялась.

## 0.4.148
- По протоколу 0.4.147: у датчиков РАЗНЫЕ базовые адреса внутри pmgr-блока (шаг 0x4000: +0, +0x4000, +0x8000, +0xc000, +0x10000…), а смещения readBK и т.п. отсчитываются от базы каждого датчика (читалось 0x10000+0x20000). В 0.4.146 окно было только у tempsensor0. Теперь окно создаётся для КАЖДОГО узла `tempsensorN` (MODELLED, значения PLACEHOLDER, как в 0.4.145/146).

## 0.4.149
- Диагностика датчика: вместо первых 90 — кольцо ПОСЛЕДНИХ 60 обращений к окнам датчиков (смещение от начала pmgr-блока), строка `[tempsensor]` в boot.err. У разных датчиков раскладка регистров разная (шаг 0x4000 у одних, 8 байт у других), нужно увидеть, что читает таймаут-код прямо перед «sensor off». Эмуляция не менялась.

## 0.4.150
- Датчик температуры: по кольцу обращений 0.4.149 исправно отвечал только один датчик (смещение 0x8000), остальные читали +0 как 0x88 (запись 0x8 → зеркало pmgr), бит0=0 → «sensor off». У датчиков РАЗНАЯ раскладка регистров (у одних шаг 0x4000, у других 8 байт: +0x468, +0x200, +0x408…). Теперь для каждого узла с `tempsensor` в имени адреса регистров берутся из его собственных DT-свойств `sensor-offset-*` (CTL0-set/clr, CTL1, CTL2, status → 1; readBK/readBK1 → valid|raw; readSum; readCnt). MODELLED, значения PLACEHOLDER.
- Тест на добавление окна/регистра датчика в pmgr-блок.

## 0.4.151
- Диагностика (эмуляция не менялась): после монтирования корня (`hfs: mounted ... md0`) ядро больше ничего не печатает и крутит idle. Дамп кода вокруг первой ссылки на строку `/sbin/launchd` и счётчик входов в эту ссылку (`[hist]` в boot.err) — чтобы узнать, доходит ли ядро до запуска launchd.

## 0.4.152
- Диагностика (эмуляция не менялась): `/sbin/launchd` не достигается (счётчик 0), значит поток, который должен запустить launchd, не стартует. Заново дампится код переключения потоков 0x7a08800–0x7a09100, чтобы найти вход Switch_context и построить гистограмму «какой поток откуда блокируется».

## 0.4.153
- Диагностика (эмуляция не менялась): в boot.err строка `[calls]` — 60 последних впервые вызванных (bl) функций ядра. Показывает, куда ядро добралось в последний раз перед тем, как все потоки заблокировались после монтирования корня.

## 0.4.154
- Диагностика (эмуляция не менялась): по `[calls]` 0.4.153 последние впервые вызванные функции — 0x7f21574, 0x7fbdabc, 0x7a6f650, 0x7bc58e0. Дамп кода первых двух и список строк, на которые ссылаются все четыре, — чтобы понять, что это за функции (bsd_init / планировщик / драйвер).

## 0.4.155
- Диагностика (эмуляция не менялась): boot.err теперь печатает первые синхронные исключения (`[exc]`: шаг, ESR, ELR, FAR) и регистры в момент первого. Между 4000 M и 8000 M шагов появляется data abort (ESR 0x96000047, FAR 0xffffffe010858000) — нужно понять, откуда и не он ли ведёт к панике/остановке после монтирования root.

## 0.4.156
- Диагностика (эмуляция не менялась): первый data abort (шаг ≈1,65 млрд, ESR 0x96000047 = translation fault уровня 3 в EL1, FAR 0xffffffe010858000 = x0) происходит в ядре по ELR 0x7d49b60, вызывающий lr 0x7fb6910. Дамп этих двух участков кода и строки, на которые они ссылаются.

## 0.4.157
- Диагностика (эмуляция не менялась): кольцо последних 64 вызовов `bl`, выполненных при размаскированных прерываниях (I и F = 0) — то есть то, что последним вызывал поток ядра вне обработчиков прерываний и idle. Строка `[bl]` в boot.err; нужна, чтобы увидеть цепочку вызовов потока, заблокировавшегося после монтирования корня.
- Из дампа 0.4.156: первый data abort — запись в объект skywalk (`sk_sys_obj`, «skywalk.mem.pp») по адресу, отображение которого ленивое (translation fault L3), по-видимому, это обычный vm_fault, а не паника.

## 0.4.158
- По `[bl]` 0.4.157: после монтирования корня ядро не «зависло» — какой-то поток драйвера (код ≈0x8eef990–0x8eefb68, вызовы в 0x8180180–0x8180388 и ожидание с таймаутом 0x7a68edc/0x7a964b4) в цикле опрашивает условие и засыпает на таймер. Дамп этого кода и строки, на которые он ссылается, чтобы узнать, какой драйвер и чего ждёт. Эмуляция не менялась.

## 0.4.159
- По дампу 0.4.158 циклический код в `[bl]` — это ApplePPMDroopController (ApplePMGR), периодический таймер драйвера, а не блокировка: ядро живо, просто заблокирован другой поток.
- Диагностика (эмуляция не менялась): цепочка вызывающих (5 уровней, начало функции = ближайший pacibsp — эвристика) для кода, ссылающегося на `/sbin/launchd`, и отметка по карте покрытия, выполнялась ли хоть одна инструкция начала каждой функции (`[chain]` в boot.err). Так видно, на каком уровне вызовов от bsd_init цепочка до запуска launchd обрывается.

## 0.4.160
- По `[chain]` 0.4.159: функция 0x79fd900 (по размеру и положению это bsd_init) выполнялась, а вызов 0x7a399d8 (дальше к `/sbin/launchd`) — нет. Значит поток bsd_init блокируется где-то между началом и сайтом вызова 0x79ffb80.
- Диагностика (эмуляция не менялась): список всех `bl` в этой функции и отметка по карте покрытия, какой из них выполнялся последним (`[bsdinit]` в boot.err) — это и есть место, где bsd_init остановился.

## 0.4.161
- По `[bsdinit]` 0.4.160: в функции 0x79fd900 последний выполненный вызов — `bl 0x7a090a8` (сайт 0x79ffa94), следующий (`bl 0x7a399d8` к launchd) не достигнут. Дамп конца этой функции, самой 0x7a090a8 и строки, на которые они ссылаются. Эмуляция не менялась.

## 0.4.162
- Разбор дампа 0.4.161: цепочка 0.4.159 пошла через ассемблерную заглушку возврата из исключения (там `eret`, нет pacibsp), поэтому «функция 0x79fd900» не bsd_init; запуск launchd идёт через AST_BSD нового потока (`bsd_utaskbootstrap` → `act_set_astbsd`).
- Диагностика (эмуляция не менялась): якорь сменён на строку `hfs: mounted %s on device` — это последнее, что ядро печатает. Цепочка вызывающих до 8 уровней, для КАЖДОЙ выполнявшейся функции цепочки — последний выполненный `bl` и следующие (`[calls-in]` в boot.err). Так найдём, где остановился bsd_init после монтирования.

## 0.4.163
- По `[calls-in]` 0.4.162: строка «mounted %s on device» лежит в расширении (0x8e7e…), его вызывающие — виртуальные вызовы, статический обход по `bl` до bsd_init не дошёл.
- Диагностика (эмуляция не менялась): восстановление цепочек вызовов из записей кадров `[fp, lr]` в ОЗУ гостя (`[stack]` в boot.err, ASSUMPTION: куча ядра линейно отображена, VA = 0xffffffe000000000 + (PA − 0x40000000); сохранённые lr без PAC-подписи). Заблокированный поток bsd_init должен быть виден по цепочке, которая заканчивается в thread_block.

## 0.4.164
- Исправление диагностики 0.4.163: размер ОЗУ подбирался с шагом 64 МБ от 1 ГБ и не находился (`[stack]` пуст). Теперь шаг 16 МБ от 16 МБ. Эмуляция не менялась.

## 0.4.165
- Диагностика: в 0.4.164 цепочек не нашлось. Вероятная причина — сохранённые lr несут PAC-подпись в старших битах (pacibsp); теперь используются только младшие 36 бит, порог глубины 6. Эмуляция не менялась.

## 0.4.166
- Диагностика: смещение VA→PA для сканирования стеков теперь измеряется через MMU гостя (`a64_virt_to_phys` по адресу кучи), а не предполагается (в 0.4.165 цепочек не нашлось). Строка `[stack] measured:`. Эмуляция не менялась.

## 0.4.167
- По `[stack] measured:` 0.4.166 куча ядра НЕ отображена линейно (VA 0xffffffe000a21308 → PA 0x54641308). Сканирование стеков переписано: идём по страницам VA кучи (0xffffffe000000000…+512 МБ), каждую транслируем через MMU гостя, цепочки `[fp, lr]` тоже следуем через трансляцию. Эмуляция не менялась.

## 0.4.168
- Диагностика: цепочки в `[stack]` теперь без лимита 150, дедуплицированы (счётчик `xN`, адреса без префикса 0xfffffff0…). Нужно найти единственную цепочку потока bsd_init/vfs_mountroot, не потеряв её среди одинаковых потоков IOKit. Эмуляция не менялась.

## 0.4.169
- По `[stack]` 0.4.168: в ядре ~140 разных цепочек; почти все — потоки IOKit (workloop 0x806c4c8…, сопоставление 0x800d0c0/0x8014f6c) и таймерные (thread_call). Среди них одна, где функция 0x7babbb8 вызывает блокирующий примитив (0x7a6b580→0x7a6a998→0x7a68fcc→0x79ff9e8), а её вызывающий — старт потока (0x7a08604): кандидат на kernel_bootstrap_thread. Дамп кода вокруг 0x7babbb8 и 0x7ba6ba0 и строки, на которые они ссылаются. Эмуляция не менялась.

## 0.4.170
- 0x7babbb8 из 0.4.169 оказался ml_set_interrupts_enabled-подобной мелочью, а не потоком: слепой разбор по адресам исчерпал себя.
- Диагностика (эмуляция не менялась): проверяю, есть ли в kernelcache таблица символов (LC_SYMTAB). Если есть — сохраняю её и подписываю адреса в `[stack]` именами функций; в лог идёт строка про число символов.

## 0.4.171
- По 0.4.170: в kernelcache НЕТ таблицы символов (LC_SYMTAB absent), имён функций не будет.
- Диагностика (эмуляция не менялась): кольцо последних 96 впервые выполненных инструкций ядра с номером шага (`[new]` в boot.err). Весь периодический код уже покрыт, поэтому «фронт нового кода» показывает, где загрузка двигалась в последний раз перед тишиной.

## 0.4.172
- По `[new]` 0.4.171: новый код ядра перестаёт выполняться на ≈2049 M шагах (из 4000 M) — последняя новая область 0x7f214a0–0x7f219a4, это код `lck_mtx_*` (медленный путь мьютекса). Значит, около 2,05 млрд шагов какой-то поток впервые попал в КОНКУРЕНТНЫЙ захват мьютекса и дальше загрузка не двигается — похоже на взаимоблокировку.
- Диагностика (эмуляция не менялась): регистры x0 (адрес мьютекса), x1, lr в момент первого выполнения последних новых инструкций и дамп кода 0x7f21400–0x7f21a00.

## 0.4.173
- Диагностика: проверена эмуляция CAS/CASP (LSE) — соответствует архитектуре, ошибки нет. В конец лога добавлен `[mtx]` — слово мьютекса, на котором стоит фронтир (0xffffffe000868550), и TPIDR_EL1, чтобы отличить взаимную блокировку от бага эмуляции. Поведение эмулятора не менялось.

## 0.4.174
- Диагностика: дамп структуры потока-владельца мьютекса (`[thr0]`) и текущего потока (`[thr1]`), чтобы увидеть, на чём заблокирован владелец. Поведение эмулятора не менялось.

## 0.4.175
- Диагностика: дамп слов вокруг головы очереди 0xffffffe000868550 и статической очереди 0xfffffff008f94e90 (`[q0..2]`). Поведение эмулятора не менялось.

## 0.4.176
- Диагностика: адреса кода ядра на стеке ядра потока-владельца (`[kstk]`), чтобы увидеть, где он остановился. Поведение эмулятора не менялось.

## 0.4.177
- Диагностика: в `[kstk]` для каждого адреса возврата показана цель предшествующего `bl`, чтобы восстановить цепочку вызовов потока-владельца. Поведение эмулятора не менялось.

## 0.4.178
- Диагностика: строки, на которые ссылается код перед каждым адресом возврата на стеке потока-владельца (`[kstr]`), чтобы назвать функцию, вызвавшую блокирующее ожидание. Поведение эмулятора не менялось.

## 0.4.179
- Диагностика: найдено, что поток-владелец очереди — простаивающий воркер thread_call (штатное ожидание, не взаимная блокировка). Добавлены строки, на которые ссылается код вокруг последнего выполненного bl функции цепочки "/sbin/launchd" (`[rstr]`). Поведение эмулятора не менялось.

## 0.4.180
- Диагностика: функция hfs_mountfs не блокирована (её кадра нет ни на одном стеке). Добавлены строки (`[bstr]`) вокруг адресов возврата заблокированного потока с цепочкой 7a68fcc 8151cbc 80eaa68 7e8e3a8 7a75284 — кандидат на поток bsd_init. Поведение эмулятора не менялось.

## 0.4.181
- Найдено: заблокированный поток цепочки — kernel_bootstrap_thread (bsd_early_init → IOKit/IOBSD → поиск BSD root "BSD root: %s" → вызов, который уходит в thread_block). Добавлены цели bl и строки в них (`[btgt]`). Поведение эмулятора не менялось.

## 0.4.182
- Найдено: поток bsd_init (7e8d100) → функция с "SecureRootName"/"IOPlatformUUID" → виртуальный вызов метода платформы (код с "no-rtc", "interrupt-controller", "master") → блокирующий помощник → thread_block. Добавлен список строк и bl в этой функции (`[wstr]`). Поведение эмулятора не менялось.

## 0.4.183
- Диагностика: окна дампа кода перенесены на функцию 0x8151a80..0x8151d00 (метод платформы с "rtc"/"IORTC"/"SecureRoot", где поток bsd_init блокируется), чтобы дизассемблировать условие ожидания. Поведение эмулятора не менялось.

## 0.4.184
- Найдено дизассемблированием: метод платформы, в котором блокируется bsd_init, крутит цикл `while(!flag){assert_wait; thread_block}` по флагам объекта (+0x10a, +0x10b), которые выставляет колбэк уведомления о ресурсах "IORTC" и "SecureRoot". Добавлен поиск всех ссылок на эти строки в коде ядра (`[sref]`), чтобы найти, кто их публикует. Поведение эмулятора не менялось.

## 0.4.185
- Диагностика (путь 2, без подмены поведения): окна дампа кода — начало метода AppleARMPE (0x8151900..0x8151a80, условия, при которых ожидание "SecureRoot" пропускается) и вызывающий код IOFindBSDRoot (0x80ea9cc..0x80eab10). Поведение эмулятора не менялось.

## 0.4.186
- Разобрано: bsd_init вызывает IOSecureBSDRoot -> callPlatformFunction("SecureRootName") у AppleARMPE; метод ждёт флаг ответа +0x10b. Условие обхода: [obj+0x110] (провайдер) равен NULL. Добавлен обход кадров и дамп объекта платформы (`[fr]`, `[plat]`), чтобы увидеть реальные значения флагов. Поведение эмулятора не менялось.

## 0.4.187
- Диагностика: [plat] из 0.4.186 показал нули (+0x108..0x118), что противоречит ветке ожидания; добавлен сырой дамп слов стека кадра метода (`[sf]`) и значения +0x108/+0x110 для каждого указателя в нём, с признаком "не отображено". Поведение эмулятора не менялось.

## 0.4.188
- Найдено: `this` метода AppleARMPE — куча-объект 0xffffffe0004b56a0 (сохранённый x19), а не статический адрес из 0.4.186. Добавлен дамп его полей (`[this]`: +0..+0x10 и флаги +0x100..+0x120). Поведение эмулятора не менялось.

## 0.4.189
- Найдено: у объекта AppleARMPE +0x10a=1 (запрос выставлен), +0x10b=0 (ответа нет), +0x10c=1, +0x110 ненулевой указатель. bsd_init ждёт ответ. Добавлен поиск всех команд STRB в [x+0x10b]/[x+0x10a] в коде ядра (`[wr10b]`) — кто может выставить флаг ответа. Поведение эмулятора не менялось.

## 0.4.190
- Найдено: флаг ответа +0x10b выставляют команды по адресам 0x8151d80 (тот же метод AppleARMPE), 0x831b7f0, 0x8b8edf0/0x8b8ee08, 0x8dcd7ec. Окна дампа кода перенесены на 0x8151ce0..0x8151e00 и 0x831b780..0x831b820, чтобы понять условие записи. Поведение эмулятора не менялось.

## 0.4.191
- Найдено: флаг ответа +0x10b выставляется в ветке callPlatformFunction("SecureRootCallBack") метода AppleARMPE (0x8151d80). Значит, кто-то должен вызвать этот колбэк после проверки secure root. `[sref]` теперь ищет все ссылки на строку "SecureRootCallBack". Поведение эмулятора не менялось.

## 0.4.192
- Исправлено в диагностике `[sref]`: строка "SecureRootCallBack" длиннее 15 байт и не находилась из-за усечённого сравнения. Поведение эмулятора не менялось.

## 0.4.193
- Пункт 1 (SEP): в логе ядра `AppleSEPKeyStore::start: _sep_enabled = 1`. Диагностика `[sep]` находит эту строку и выводит код вокруг её использования, чтобы увидеть, как вычисляется `_sep_enabled` и чего keystore ждёт от SEP. Поведение эмулятора не менялось.

## 0.4.194
- Исправлено в `[sep]`: поиск по подстроке "_sep_enabled" (формат строки неизвестен) в расширенной области cstring, адрес берётся с начала строки. Поведение эмулятора не менялось.

## 0.4.195
- Найдено по коду: `_sep_enabled = 1` в AppleSEPKeyStore::start ставится безусловно, не из DeviceTree. Keystore ждёт SEP через AppleSEPManager (почтовый ящик). Диагностика: в лог выводятся узлы DT с "sep"/"mailbox" (имя, compatible, адрес, размер) и все свойства узла `sep`, чтобы спроектировать модель почтового ящика SEP. Поведение эмулятора не менялось.

## 0.4.196
- Найдено: SEP описан в DT как узел `sep` (iop,t8020) по физическому адресу 0x242400000, размер 0x10000 (почтовый ящик IOP) и `dart-sep`; оба уже существуют как нулевые блоки. В конец лога выводится список тронутых неизвестных блоков (`[unk]`), чтобы увидеть, обращалось ли ядро к `sep`. Поведение эмулятора не менялось.

## 0.4.197
- Найдено: блок `sep` (0x242400000) за весь запуск не тронут ни разу, то есть AppleSEPManager не доходит до обращения к почтовому ящику. Диагностика `[sepx]`: все строки "AppleSEP…"/"SEPManager"/"sep-…" в ядре и выполнялся ли код, который на них ссылается. Поведение эмулятора не менялось.

## 0.4.198
- Диагностика: окна дампа кода перенесены на 0x8641100..0x8641300 (код вокруг ссылки на "AppleSEPManager", который ни разу не выполнялся), чтобы понять, что должно его запустить. Поведение эмулятора не менялось.

## 0.4.199
- Найдено: ссылка на "AppleSEPManager" по 0x8641194 — вспомогательная функция SEPBICS, а не запуск менеджера. Список `[sepx]` был обрезан на 48 строках; лимит поднят до 600 (по порядку в памяти), чтобы увидеть и собственные строки AppleSEPManager. Поведение эмулятора не менялось.

## 0.4.200
- Найдено: класс AppleSEPManager зарегистрирован (инициализация выполнена), но его методы (endpointsEnabled, debugEP и др.) не вызывались; keystore путь "Can't find AppleSEPManager" не выполнялся. Два заблокированных потока IOKit имеют в стеке адреса 0x8c97584 (рядом со ссылкой на "AppleSEPManager") и 0x89825f8. Диагностика `[bstr]` переключена на строки около этих адресов возврата. Поведение эмулятора не менялось.

## 0.4.201
- Найдено: заблокированные потоки около 0x8c97584 (камера Pearl) и 0x89825f8 (SPMI) к SEP не относятся. Диагностика `[skx]`: выполнялся ли код, ссылающийся на "SecureRoot"/"SecureRootCallBack" (0x83d23f0, 0x83d272c, 0x89876e4, 0x898af7c, 0x88b2f50). Поведение эмулятора не менялось.

## 0.4.202
- Найдено: код keystore, который регистрирует /dev/aes_0 и вызывает платформенную функцию "SecureRoot"/"SecureRootCallBack" (0x83d23f0, 0x83d272c), не выполнялся, хотя AppleSEPKeyStore::start доходит до "_sep_enabled". Диагностика `[ksx]`: выполненные диапазоны инструкций в AppleSEPKeyStore::start (0x898aa80..0x898b500), чтобы найти, где start останавливается. Поведение эмулятора не менялось.

## 0.4.203
- Найдено: AppleSEPKeyStore::start выполняется до 0x898acc0, затем фрагменты 0x898acf8..08 и 0x898af48..4c. Окна дампа кода перенесены на 0x898ac80..0x898ad40 и 0x898af20..0x898afc0, чтобы увидеть, чем start заканчивается. Поведение эмулятора не менялось.

## 0.4.204
- Найдено: AppleSEPKeyStore::start завершается успешно (возвращает 1), но код keystore, регистрирующий "SecureRoot", не выполняется — он, по-видимому, запускается, когда появится AppleSEPManager. Диагностика `[plist]`: личность (Info.plist) AppleSEPManager из prelink-info в памяти, чтобы узнать ключи сопоставления (IONameMatch, IOResourceMatch и т.д.). Поведение эмулятора не менялось.

## 0.4.205
- Найдено: prelink-info (Info.plist) в памяти нет. Ссылка на "AppleSEPManager" рядом с адресом 0x8c97584 принадлежит драйверу камеры Pearl, который сам ждёт появления AppleSEPManager. Диагностика `[bstr]` теперь печатает строки около 14 адресов возврата заблокированных цепочек start() драйверов IOKit, чтобы узнать, какие драйверы стоят и на чём (искомые: DART, PMGR, питание для узла sep). Поведение эмулятора не менялось.

## 0.4.206
- Найдено (0.4.205): в заблокированных start() стоят ApplePMGR (ждёт "function-pmp_control" и "pmgr-dock-fifo-client"), AppleARMCPU ("function-cpu_idle"), AppleInterruptController, AppleBaseband, кнопки/HID. Корневая гипотеза: ApplePMGR ждёт провайдера PMP (сопроцессор управления питанием), от которого каскадом зависят остальные, включая AppleSEPManager. Блок pmp ни разу не тронут. Диагностика: свойства узлов DT `pmp` и `pmgr` в лог. Поведение эмулятора не менялось.

## 0.4.207
- Найдено: узел `pmp` — IOP-сопроцессор (iop-version, reg x3, прерывания, iommu-parent), его блоки не тронуты. Диагностика `[sepx]` переключена на строки с "PMP" в ядре и выполнялся ли код, который на них ссылается — чтобы найти драйвер PMP и увидеть, стартовал ли он. Поведение эмулятора не менялось.

## 0.4.208
- Найдено: драйвер ApplePMP, ApplePMPControlFunction (регистрирует function-pmp_control) и прошивка ApplePMPFirmware/AppleT8020PMPFirmware ни разу не выполнялись; транспорт AppleA7IOPV2PMP/V4PMP — выполнялся (вероятно выбор класса по iop-version). Окна дампа кода перенесены на 0x82df8c0..0x82dfa40 и 0x82dfe40..0x82dffc0 + в лог выводится compatible узла pmp. Поведение эмулятора не менялось.

## 0.4.209
- Найдено: узел pmp имеет compatible `iop,ascwrap-v2` и три диапазона reg (0x23bc00000/0x60000, 0x23b850000/0x200000, 0x23b500000/0x40000). Два потока блокированы в коде 0x8836fcc (ApplePMPFirmware/AppleT8020PMPFirmware) в той же вспомогательной функции ожидания 0x814a91c, что и ApplePMGR::start; возможна циклическая зависимость PMGR ↔ PMP. Диагностика `[bstr]`: строки вокруг адресов возврата этих цепочек. Поведение эмулятора не менялось.

## 0.4.210
- Найдено: заблокированные start() (ApplePMGR, AppleSOCTuner, baseband, кнопки, PMP-прошивка и др.) стоят в общей функции ожидания AppleARMCPU "AppleARMCPU-AllCPU-Initialized" (0x814a91c/0x814a9f0). Эмулятор запускает ОДИН процессор, остальные 5 ядер DT никогда не отмечаются. ASSUMPTION (изменение поведения): в командной строке добавлен штатный boot-arg XNU `cpus=1`, чтобы ядро не ждало несуществующие ядра. Ничего не подделывается; если ядро его не учитывает в AppleARMCPU, эффект будет нулевым и это видно по логу.

## 0.4.211
- РЕЗУЛЬТАТ 0.4.210: с `cpus=1` ядро больше не ждёт остальные ядра — стартуют AIC, SPI, I2C, AES, backlight, USB PHY, DWI, MTR, AGX и др. (первая смена поведения за долгое время). Затем паника ядра: "thread_invoke: preemption_level 1, possible cause: blocking while holding a spinlock, or within interrupt context" и "thread already waiting on 0xffffffe01002c208"; цикл 0x7ba5ec0 — это panic-halt самого ядра. Диагностика `[pfr]`: цепочка кадров от x29 на остановке, чтобы найти, кто блокируется при preemption_level 1. Поведение эмулятора не менялось.

## 0.4.212
- Разобрано по `[pfr]`: цикл 0x7ba5ec0 — повторная паника внутри panic (mutex в обработчике → "thread already waiting"); исходная паника "thread_invoke: preemption_level 1". Добавлен снимок кадра при ПЕРВОМ входе в panic() (0x7a44164): `snap_*` в эмуляторе (одно сравнение pc на инструкцию) и вывод цепочки `[pfirst]`, чтобы увидеть, кто блокировался с preemption_level 1. Поведение гостя не менялось.

## 0.4.213
- НАЙДЕНА ПЕРВОПРИЧИНА паники: первое исключение запуска — запись AppleT8020PlatformErrorHandler::start по физическому адресу 0x200158000 (ESR 0x96000050, внешний abort), где нет устройства: блок 0x200000000 (1 МиБ) заканчивается на 0x200100000, а AMCC non-plane (0x200100000) и DCS (0x200200000) ядро само отображает в логе. Исправление (MODELLED, ASSUMPTION, содержимое PLACEHOLDER): добавлен нулевой блок `amcc` 0x200100000 (1 МиБ) + тест. Остальная цепочка (preemption_level 1, "thread already waiting") — вторичные паники внутри panic().

## 0.4.214
- РЕЗУЛЬТАТ 0.4.213: паника от AMCC устранена; запуск продвинулся до AppleDisplayPMU::start -> драйвер I2C (AppleS5L8940XI2CController) обращается по шине к PMU дисплея и срабатывает assert "RxFIFO doesn't have enough bytes" (_runPolledMode): у нас блок I2C — нулевой, без FIFO. Окна дампа кода: 0x84ed380..0x84ed600 (драйвер I2C) и 0x7a43b60..0x7a43bc0 (инструкция паники, ESR=0x2000000 = UNDEFINED в нашем декодере — проверить, не пробел ли это эмулятора). Поведение эмулятора не менялось.

## 0.4.215
- Найдено: инструкция 0x7a43b98 — `udf #0xdeff`, штатная panic-ловушка XNU (не пробел эмулятора). Драйвер I2C: регистры +0x08 (счётчики FIFO), +0x14 (статус; бит 28 — ошибка, биты 24/27 — завершение; запись 0x08800040 сбрасывает), +0x18 (управление). Окна дампа кода: 0x84ecf00..0x84ed380 и 0x84ed600..0x84ed9c0 (начало транзакции и _runPolledMode) для проектирования модели I2C «пустая шина». Поведение эмулятора не менялось.

## 0.4.216
- Драйвер I2C разобран частично: assert "RxFIFO doesn't have enough bytes" срабатывает, когда транзакция «завершилась без ошибки» ([x19+0x170]==0), а счётчик Rx FIFO ((+0x08>>8)&0xff) не равен ожидаемому; коды ошибок 0xe00002d5 (нет устройства) / 0xe00002d6 (нет ответа); бит 24 регистра +0x14 — признак NACK. Окна дампа расширены до 4 (0x84ecb00..0x84ee200), чтобы получить весь драйвер для модели I2C «пустая шина». Поведение эмулятора не менялось.

## 0.4.217
- Разобран драйвер I2C (опрос статуса +0x14): бит 27 — транзакция завершена, бит 21 — NACK (ошибка 0xe00002e9), бит 20 — данные готовы/успех, биты 23 и 6 — ошибка; запись в +0x14 сбрасывает биты; +0x18 — маска прерываний; +0x04 — данные Rx (бит 8 — FIFO пуст), +0x08 — счётчики FIFO. Диагностика `[i2c]`: последние 80 обращений к блокам i2c (какой регистр запускает транзакцию), чтобы смоделировать «пустую шину» (NACK) по фактам. Поведение эмулятора не менялось.

## 0.4.218
- РЕЗУЛЬТАТ `[i2c]`: драйвер делает инициализацию (+0x1c, +0x14, +0x18, +0x10=0x80000000, тайминги +0x38/+0x30), затем читает +0x14/+0x0c/+0x08 (всё 0) и срабатывает assert. MODELLED (ASSUMPTION, PLACEHOLDER): контроллеры I2C (имя узла DT содержит "i2c") ведут себя как ПУСТАЯ шина: установка бита 31 в +0x10 завершает транзакцию статусом бит 27 (готово) + бит 21 (NACK), биты статуса сбрасываются записью единиц. Значения выведены из кода драйвера, не из документации. Тест добавлен.

## 0.4.219
- ИСПРАВЛЕНО понимание: assert вызывает `_checkBusStatus` (вызывается из AppleAcornDisplayPMU::handleStart "Clearing UVLO flag"), а не RxFIFO. Он требует бит 24 регистра +0x14 = 1 (уровень SCL), иначе "SCL is stuck low". Модель 0.4.218 (NACK по записи в +0x10) отменена как догадка без оснований; вместо неё MODELLED (ASSUMPTION, PLACEHOLDER): статус I2C читается с битом 24 = 1 (шина свободна), события сбрасываются записью единиц. Тест обновлён.

## 0.4.220
- Сборка 0.4.219: паника I2C ушла (бит 24 = уровень SCL сработал), ядро дошло до шага ~1.08 млрд. Новая остановка: `AppleT802xPCIe::configure` — REQUIRE `_phyTunables != nullptr` (AppleT802xPCIe.cpp:135). Только диагностика: вывод PCIe-узлов DeviceTree и их свойств, чтобы найти, из какого свойства драйвер берёт tunables (их обычно кладёт iBoot).

## 0.4.221
- Диагностика 0.4.220: узел `apcie` (apcie,t8020) не имеет свойств с tunables (IPSW-дерево их не содержит). Только диагностика: `[sepx]` теперь ищет строки ядра с "tunable" и показывает, исполнялся ли код, который на них ссылается.

## 0.4.222
- Диагностика 0.4.221: ядро читает свойства `apcie-phy-tunables`, `apcie-phy-ip-tunables`, `apcie-axi2af-tunables` узла `apcie` (код исполнялся); в IPSW-дереве их нет (их кладёт iBoot). MODELLED (ASSUMPTION, PLACEHOLDER): добавляем их ПУСТЫМИ (0 записей; ядро проверяет numTunables*sizeof==длина, 0 допустим). Никакие значения настройки PHY не выдуманы; к шине PCIe ничего не подключено. Тест на добавление пустого свойства.

## 0.4.223
- Сборка 0.4.222: пустые tunables убрали панику PCIe (_phyTunables). Новая остановка: AppleT8020DART.cpp:1918 "page size is incorrectly configured: config 0x4000 actual 0x1" для `dart-ane` (единственное чтение блока: +0x0 = 0). Только диагностика: окна кода вокруг проверки (0x8f05700..0x8f05a00 и др.), чтобы понять, какой регистр DART кодирует размер страницы.

## 0.4.224
- Разбор кода AppleT8020DART (0x8f05980..): проверка читает регистр DART +0x0, берёт биты 27:24 и сравнивает с настроенным сдвигом страницы; печатает actual = 1<<поле. config = 0x4000 (16 КиБ) => поле должно быть 14. MODELLED (ASSUMPTION, PLACEHOLDER): блоки DART (имена узлов "dart*") читают +0x0 = 0x0e000000; остальные биты неизвестны (0). Тест добавлен.

## 0.4.225
- Сборка 0.4.224: модель DART снимает панику page-size; ядро дошло до шага ~1.70 млрд. Новая остановка: assert "spmi timeout" (AppleSPMI, вызван из AppleDialogPMU "pmu-debug"). Только диагностика: окна кода 0x8982900..0x8983600 (проверка тайм-аута и функция вызова).

## 0.4.226
- Дамп 0.4.225: assert "spmi timeout" — код опрашивает регистр SPMI-контроллера через виртуальный метод (чтение регистра 0, ожидание бита), счётчик повторов 10. Блока "spmi" в списке тронутых нет. Только диагностика: узлы DeviceTree с spmi/dwi (имя, compatible, адрес, свойства).

## 0.4.227
- Диагностика 0.4.226: контроллеры SPMI (spmi,t8015) лежат в 0x23d180700 и 0x23d180d00 (по 0x100), но блок с именем spmi не трогался. Добавляем именованный блок `spmi` на страницу 0x23d180000 (0x4000), чтения 0, первые обращения логируются (PLACEHOLDER) — чтобы увидеть реальные обращения драйвера.

## 0.4.228
- Блок `spmi` тронут (26 чтений / 40 записей, последнее +0x700; первые записи в +0x738: 1,1,0x11,0x31,0x71,…). Только диагностика: кольцо последних 80 обращений теперь пишет и блок spmi (раньше только i2c).

## 0.4.229
- Последовательность SPMI: чтение статуса +0x700, запись команды в +0x704 (0x60088f33, 0x11a08f39), чтение ответа +0x708. MODELLED (ASSUMPTION, PLACEHOLDER): блок spmi — статус бит 24 = FIFO ответа пуст; каждая ненулевая команда порождает один ответ = эхо командного слова; чтение +0x708 вынимает его. Реальный формат ответа НЕИЗВЕСТЕН, данные PMU не выдуманы. Тест добавлен.

## 0.4.230
- 0.4.229: эхо-ответ SPMI не помог, паника "spmi timeout" осталась (шаг ~1.66 млрд). Только диагностика: кольцо обращений (300) теперь пишет ТОЛЬКО блок spmi, чтобы видеть полную последовательность (раньше его забивал i2c).

## 0.4.231
- Сборка 0.4.230: вся последовательность SPMI видна: команды 0x6001 8f30 (запись), 0x6001 8f38 (чтение) и др. выполняются, на КАЖДУЮ драйвер вызывает виртуальный метод слота 0x618 («команда завершена?»), он возвращает «не готово» → обработчик 0x8982b18 (слот 0x630) увеличивает счётчик [this+0x18c], и после 10 подряд — assert "spmi timeout". Вероятно метод ждёт прерывание контроллера, которого у нас нет. Только диагностика: поиск слота vtable в живой памяти и дамп кода слота 0x618.

## 0.4.232
- Дамп 0.4.231: слот 0x618 — диспетчер: при w1!=0 вызывается слот 0x620 (ожидание ПРЕРЫВАНИЯ: commandSleep на флаге [this+0x184], таймаут [this+0x180]*1e6 нс, при истечении 0xe00002d6), иначе слот 0x628 (опрос регистров состояния прерываний +0x40/+0x60). Опросов регистров нет => режим по прерыванию. Контроллера прерываний (AIC) с доставкой IRQ у нас нет, поэтому каждая команда SPMI — таймаут, 10 подряд — assert. Только диагностика: поля объекта AppleSPMI (+0x110, +0x168, +0x180, +0x184, +0x18c).

## 0.4.233
- Объект AppleSPMI (0.4.232): +0x110=1, +0x168=0xc0 (номер прерывания 192: бит 0 слова 6), +0x180=0x7d0 (таймаут 2000 мс), +0x184=1 (флаг ожидания остаётся 1), +0x18c=10 (счётчик). Значит драйвер ждёт IRQ №192. Только диагностика: свойства `interrupts`/`reg`/`compatible`/`ipid-mask` узла aic и spmi — для минимальной модели AIC (доставка IRQ), которая понадобится.

## 0.4.234
- DT: прерывания узла spmi начинаются с 192 (0xc0) — ровно номер, которого ждёт AppleSPMI (объект +0x168). MODELLED (ASSUMPTION, PLACEHOLDER): минимальный контроллер прерываний на блоке `aic`: EVENT +0x2004, MASK_SET +0x4100, MASK_CLR +0x4180 (раскладка из ПУБЛИЧНОГО драйвера Asahi Linux для AIC первого поколения, на T8020 не проверена), всё замаскировано при старте, без приоритетов/целей/IPI; выход — вход IRQ процессора. SPMI: команда в +0x704 поднимает IRQ 192 и бит 0 слова статуса +0x778 (сброс записью 1). Тесты добавлены.

## 0.4.235
- Сборка 0.4.234: паника "spmi timeout" осталась; регистр EVENT AIC ни разу не читался, последняя запись AIC — +0x4194 (слово 5 маски), т.е. IRQ 192 (слово 6 → +0x4198) ядро НЕ размаскировало. Только диагностика: список различных записанных смещений AIC с числом записей и ИЛИ значений.

## 0.4.236
- Список записей AIC (0.4.235) переполнился нулевыми записями в +0x3000.. (таблица целевых CPU, 0 = «никуда»); ядро пишет туда 1 для включаемых прерываний (+0x317c.. = №95..). Только диагностика: нулевые записи в +0x3000..0x3fff не учитываются, чтобы увидеть маски (+0x41xx) и ненулевые цели.

## 0.4.237
- Сводка AIC (0.4.236): ядро выставляет цели (+0x3xxx=1) и размаскирует (MASK_CLR +0x4180..) прерывания; из списка SPMI размаскирован ТОЛЬКО №204 (0xcc; слово 6, бит 12), №192 имеет цель, но не размаскирован. MODELLED (ASSUMPTION): команда SPMI поднимает IRQ 204 (раньше 192). Что делает обработчик — покажет кольцо обращений spmi.

## 0.4.238
- Сборка 0.4.237: IRQ 204 для SPMI убрал панику "spmi timeout" (команды SPMI теперь завершаются), стартовали DART ane/isp. Новая остановка (шаг ~1.29 млрд): AppleHxDART REQUIRE "translation not permitted for bypassed SID (%d)" (AppleHxDART, адрес 0x8f03890). Только диагностика: окна кода 0x8f03400..0x8f03900.

## 0.4.239
- Ошибка модели 0.4.237: IRQ 204 принадлежит dart-aop (паника "Max spurious DART/SMMU interrupts", x3[3]="dart-aop"), а не SPMI. Из списка прерываний SPMI ядро размаскировало №234 (0xea; MASK_CLR слово 7 = 0x3000470, бит 10). MODELLED (ASSUMPTION): команда SPMI поднимает IRQ 234.

## 0.4.240
- Сборка 0.4.239: IRQ 234 дошёл до обработчика SPMI (читает +0x720..+0x738 enable и +0x760..+0x778 status). Он увидел enable слова 6 = 0, т.к. мы возвращали 0 — драйвер пишет туда маски (+0x738 = 0xff7f3ff1). MODELLED (ASSUMPTION): слова +0x720..+0x73c читаются как последнее записанное значение. Тест добавлен.

## 0.4.241
- Сборка 0.4.240: SPMI-обработчик теперь работает (статус читается, очищается, команды завершаются). Новая остановка (шаг ~1.26 млрд): data abort на 0xffffffe81b313600, инструкция `ldr d0,[x20,w8,uxtw #3]` при w8=0xffffffff (счётчик 0 минус 1) в цикле над массивом double (значения 1.0) в 0x818adf4, цепочка 0x8190a18 <- 0x8199c38 <- 0x819d9c4 <- 0x806c4c8. Только диагностика: окна кода и строки, на которые ссылаются эти функции.

## 0.4.242
- Дамп 0.4.241: функция с индексом -1 — расчёт политики питания/термо (строки CLPC/ApplePPM/ane_temp_reg): счётчик записей w22=0 (таблицы до 10 double, полином по значениям [sp+0x118]) -> w19-1=-1. Нужно найти, откуда берётся счётчик (скорее всего свойство DeviceTree, которого нет в IPSW-дереве). Только диагностика: окна кода 0x818a200..0x818ac00 с началом функции и строки.

## 0.4.243
- Дамп 0.4.242: функция 0x818a650 — построение таблиц ANE-производительности CLPC ("CLPC: ANE PACC%u: MHz"); числа читаются из хелперов 0x81877bc / 0x8187884 (свойства DeviceTree), счётчик w21=0 -> цикл по -1. Только диагностика: код хелперов и строки, на которые ссылаются обе функции (какие свойства читаются).

## 0.4.244
- Дамп 0.4.243: 0x8187884 — переходник: берёт [this+0x88] и, если он NULL, СРАЗУ возвращается, ничего не записав (счётчик остаётся 0 -> индекс -1 в 0x818adf4). Строки рядом (_provider->_cpuComplexCount) показывают, что +0x88 это провайдер. Только диагностика: слова статических объектов x25/x26 первого исключения (0xfffffff009150cd0 / 0xfffffff00914f4c0).

## 0.4.245
- Дамп 0.4.244: 0xfffffff009150cd0 и 0xfffffff00914f4c0 — это таблицы констант (floats), а не провайдер. Только диагностика: окна кода вызовов 0x818a780..0x818a9c0 (откуда берётся x0/this переходника 0x8187884) и 0x81877bc..0x8187884.

## 0.4.246
- Дамп 0.4.245: счётчик берётся вызовом `d104(obj=[глобал 0xfffffff00914f7c8], 2, ...)` -> переходник 0x8187884 -> [obj+0x88]. Только диагностика: значение глобала и слова объекта (+0x88 = провайдер).

## 0.4.247
- Дамп 0.4.246: объект CLPC существует (0xffffffe0003d8a00), его +0x88 = провайдер 0xffffffe0003bc000 (не NULL). Значит переходник вызвал метод провайдера (0x8181c94: селектор 2/5/8), он вернул 0 состояний. Только диагностика: слова провайдера и код 0x8181c94..0x8182200 со строками.

## 0.4.248
- Дамп 0.4.247: провайдер — ApplePMGR (строки perfDomainID <= kPerfDomainIDMax / nstates <= perfStateCount). Метод 0x8181c94 возвращает число состояний перф-домена: [pmgr+0xe50] = массив доменов по 0xf0 байт, [pmgr+0xe5c+id] = индекс, +8 = число состояний. Для домена ANE оно 0. Только диагностика: дамп этой таблицы.

## 0.4.249
- Дамп 0.4.248: у ApplePMGR 5 перф-доменов (id 1,2,3,5,8 -> индексы 0..4), число состояний 3/9/4/9/2; ANE-запрос идёт с id 8 (2 состояния), т.е. счётчик не 0 на этом шаге; нулевой w22 получается при подсчёте состояний с частотой >= 1 МГц (частота = u32 из таблицы [dom+0x90] * 1e6). Только диагностика: сами таблицы [dom+0x88], [dom+0x90], [dom+0x70] у доменов 0..4.

## 0.4.250
- Дамп 0.4.249: домен id 8 (ANE, индекс 4) имеет 2 состояния, но таблица частот [dom+0x90] = {0,0}, поэтому счётчик состояний с частотой >= 1 МГц = 0 (у доменов 1/3 частоты 0x3e80000). Только диагностика: слова свойств pmgr perf-domains / ifane-max / voltage-states8,9,2 / perf-regs / fane.

## 0.4.251
- Дамп 0.4.250: perf-domains = 5 записей по 28 байт (имена COS/ECPU/DCS?/PCPU/ANE), voltage-states8 у нас — 9 плейсхолдеров {1,0}, но домен ANE получил 2 состояния и частоты {0,0}. Только диагностика: где в ядре строится "voltage-states%d" (ссылки ADRP+ADD, выполнялись ли).

## 0.4.252
- Дамп 0.4.251: "voltage-states%d" используется в одном месте — 0xfffffff008171268 (исполнялось). Только диагностика: окна кода 0x8171000..0x8171e00 и строки в них (код построения таблиц перф-доменов ApplePMGR).

## 0.4.253
- Разбор кода ApplePMGR (0x8171268..0x81715e0): для домена режима 0 частота состояния = word0 / 1e6 МГц ТОЛЬКО если word1 (напряжение) первой записи не 0, иначе 0; число состояний при word1==0 = последний word0 + 1 (у нас {1,0} -> 2 состояния, частоты 0). Отсюда счётчик ANE 0 и индекс -1. PLACEHOLDER (выдуманные значения, НЕ данные iBoot): `voltage-states8` теперь 9 записей {1000000, 1}. Остальные voltage-statesN без изменений.

## 0.4.254
- Сборка 0.4.253: плейсхолдер voltage-states8 убрал падение в таблицах ANE; ядро дошло до шага ~1.68 млрд. Новая остановка: внешний abort (ESR 0x96000010) — чтение 0x23bc0810c (нет ни RAM, ни устройства), ELR 0x82e0f88. Только диагностика: узлы DeviceTree вокруг этого адреса.

## 0.4.255
- Дамп 0.4.254: 0x23bc0810c лежит в узле `pmp` (iop,ascwrap-v2, 0x23bc00000, размер 0x60000), который пересекается с disp0/pmgr и поэтому не получил блок. PLACEHOLDER: именованный блок `pmp` на странице 0x23bc08000 (0x4000), чтения 0, первые обращения логируются; протокол IOP/mailbox НЕ моделируется.

## 0.4.256
- Сборка 0.4.255: блок pmp на 0x23bc08000 принят (запись +0x10c=1 прошла), следующий внешний abort — чтение 0x23bc00044 (область управления CPU обёртки ASC). PLACEHOLDER: блок `pmp0` на 0x23bc00000 (0x4000), чтения 0, обращения логируются; протокол IOP не моделируется.

## 0.4.257
- Сборка 0.4.256: блоки pmp/pmp0 приняты (ядро пишет CPU-control +0x44=0x10), дошли до шага ~1.81 млрд. Новая остановка: внешний abort — чтение 0x269104114, ELR 0x8c0fad4. Только диагностика: узлы DeviceTree рядом с адресом.

## 0.4.258
- Дамп 0.4.257: 0x269104114 лежит в узле `avd` (декодер видео, 0x268000000, размер 0x2000000), перекрывающемся с dart-avd, поэтому блок не создавался. PLACEHOLDER: блоки `avd` (0x268000000..0x269010000) и `avd2` (0x269014000..0x26a000000), чтения 0; декодер не моделируется.

## 0.4.259
- Сборка 0.4.258: блоки avd приняты, шаг ~2.06 млрд; новая остановка: чтение 0x22b800000 без устройства. Чтобы не терять по сборке на каждый пропущенный узел: MODELLED (PLACEHOLDER) «заполнение дыр» — каждый пропущенный /arm-io узел (размер 0, >16 МиБ или перекрывается) режется на куски 64 КиБ (1 МиБ для больших, <=0x40 кусков на узел, <=120 всего) и куски, не пересекающиеся с имеющимися блоками, становятся нулевыми блоками `<имя>~`.

## 0.4.260
- Сборка 0.4.259: заполнение дыр сработало (118 блоков), абортов по физ. адресам нет. Новая остановка: assert в AppleH10CamIn::start (fISPVersion=-1, регистр версии ISP читается 0). Диагностика: выгрузка кода 0x8c50e00..0x8c51e00 для разбора. Поведение не меняется.

## 0.4.261
- Разбор assert AppleH10CamIn::start (по коду ядра): драйвер читает регистр ISP по смещению 0x1800000 (версия 9) либо 0x3000000 и требует (reg>>16)==fISPVersion, где fISPVersion берётся из таблицы ядра [0xfffffff0079ab1e8+0x204]. Диагностика (поведение не меняется): дамп этой таблицы `[isp]` и список DT-узлов isp/cam с физ. адресами.

## 0.4.262
- Сборка 0.4.261: ожидаемая версия ISP из таблицы ядра fISPVersion=8, значит драйвер читает регистр по смещению 0x3000000 и ждёт (reg>>16)==8. DT-узел isp: 0x22a000000, размер первого диапазона 0x1e8d000 (меньше 0x3000000), поэтому добавлена диагностика всех слов свойства reg узла isp. Поведение не меняется.

## 0.4.263
- Сборка 0.4.262: reg узла isp = {0x22a000000,0x1e8d000},{0x23b080000,0xd000}. MODELLED (PLACEHOLDER): регистр версии ISP по физ. адресу isp+0x3000000 читается как 0x80000 ((reg>>16)==fISPVersion==8, как требует код AppleH10CamIn::start). Адрес вне диапазона DT — реальное поведение железа НЕ известно, значение взято из проверки самого ядра.

## 0.4.264
- Сборка 0.4.263 не помогла: по логу гость читает isp~@0x22b800000+0 = isp+0x1800000 (ветка fISPVersion==9), значит версия 9, а не 8 (дамп таблицы показал другую строку; моё предположение было ОШИБОЧНЫМ). Регистр isp+0x1800000 теперь читается как 0x90000 (MODELLED, PLACEHOLDER: значение лишь то, что требует проверка (reg>>16)==9; реальное содержимое чипа неизвестно). Блок 0x22d000000 убран.

## 0.4.265
- Сборка 0.4.264: камера прошла (fISPVersion=9, AppleH10CamIn стартовал). Новая остановка: NULL-разыменование (FAR=0) в коде AVD-драйвера по ELR 0x8c0028c (рядом строки powerDownDeviceInternal / "Failed to power down AVD"), шаг ~1.93 млрд. Диагностика: выгрузка кода 0x8bffa00..0x8c00a00. Поведение не меняется.

## 0.4.266
- Сборка 0.4.265: NULL-разыменование было следствием раннего сбоя AVD: "getAVDVersion(): AVD2 Version is wrong: 00000000", "Not supported AVD2 Revision 0" (драйвер H11, регистр версии AVD читается 0), затем powerDownDeviceInternal обращается к неинициализированному объекту [this+0xa8]. Диагностика: журнал обращений к блокам avd (вместо spmi), чтобы увидеть, какой регистр читается. Поведение не меняется.

## 0.4.267
- Сборка 0.4.266: единственное обращение avd2 перед ошибкой версии — чтение avd2+0xf0114 (физ. 0x269104114) = 0. Диагностика: поиск кода, ссылающегося на строку getAVDVersion, и его выгрузка ([sep] ref ...), чтобы увидеть, какое значение/формат ожидает драйвер. Поведение не меняется.

## 0.4.268
- Сборка 0.4.267: строка getAVDVersion адресуется кодом по 0x8c0fae4 (выполнялся). Диагностика: выгрузка кода 0x8c0f700..0x8c10200, чтобы прочитать логику сравнения версии. Поведение не меняется.

## 0.4.269
- Сборка 0.4.268 (код getAVDVersion прочитан): драйвер читает [0x268000000+0x1104114] и требует (v & 0xffff0000) == 0x20000. MODELLED (PLACEHOLDER): регистр 0x269104114 читается как 0x20000 (младшие 16 бит — ревизия — НЕИЗВЕСТНЫ, 0; возможно драйвер напишет "Not supported AVD2 Revision"). Диагностика: поиск кода по строке "Not supported AVD2" для чтения таблицы ревизий.

## 0.4.270
- Сборка 0.4.269: версия AVD принята (AVDVersion = 00020000), но nAVD2Revision = 0 -> "Not supported AVD2 Revision 0" (код ссылки 0x8bfe840). Диагностика: выгрузка кода 0x8bfe300..0x8bff000, чтобы увидеть, откуда берётся ревизия и какие значения поддерживаются. Поведение не меняется.

## 0.4.271
- Сборка 0.4.270: код AppleAVD::start показал, что ревизия [this+0x100] ставится из байта свойства, прочитанного через IORegistry (значение 1 -> 0xc, 2 -> значение из таблицы, поддерживаются 0xc/0xd), иначе "Not supported AVD2 Revision". Диагностика: дамп строк 0xfffffff0076926b0..0x...800 ([avdstr]) — имя пути/свойства. Поведение не меняется.

## 0.4.272
- Сборка 0.4.271: свойство "avd-version" узла /arm-io/avd; AppleAVD::start по первому байту выбирает ревизию (1 -> 0xc, 2 -> константа ядра по 0xfffffff00716e400, нужны 0xc/0xd). Лог показал "H11/H11a" (ветка 2), но ревизия = 0. Диагностика: значение avd-version в DT и дамп константы ядра ([avdc]). Поведение не меняется.

## 0.4.273
- Сборка 0.4.272: avd-version в DT = 2 (ветка H11/H11a), константа ядра для [this+0x100] = 0xd (поддерживается). Значит ревизия 0 появляется позже: сообщение "Clover AVDVersion = ..., nAVD2Revision = 0" печатается кодом, который, вероятно, выводит ревизию из регистра версии. Диагностика: поиск кода по строке "nAVD2Revision". Поведение не меняется.

## 0.4.274
- Сборка 0.4.273: код по 0x8bfe82c печатает nAVD2Revision = (версия & 1) и отвергает 0. MODELLED (PLACEHOLDER): регистр версии AVD 0x269104114 теперь 0x20001 (старшие 16 бит = 0x2 как требует getAVDVersion, бит0 = то, что требует проверка ревизии; реальное значение железа неизвестно). Диагностика: needle createAPCommObject для следующего шага.

## 0.4.275
- Сборка 0.4.274: AVD прошёл проверки версии/ревизии, но упёрся в RTBuddy (ожидание ответа сопроцессора). Камера (isp) и видеодекодер (avd) не нужны, чтобы дойти до /sbin/launchd, а их драйверы паникуют без ответа железа. MODELLED (ASSUMPTION, НЕ то, что делает настоящий iBoot): из DeviceTree удаляются узлы isp, avd, dart-isp, dart-avd (новая функция a64_dt_node_remove с тестом). Откат — удалить блок 0.4.275 в attempt.c. Цель: добраться до SEP/SecureRootCallBack, на котором стоит bsd_init.

## 0.4.276
- Сборка 0.4.275: узлы isp/avd/dart-isp/dart-avd удалены (лог: removed 4 node(s)), но та же паника RTBuddy теперь от PMP (RTBuddy(PMP): Loaded firmware … затем "No response received"); SIO и AOP стартуют без паники. MODELLED (ASSUMPTION, НЕ то, что делает iBoot): из DT дополнительно удаляются узлы pmp и pmp0 (процессор управления питанием), чтобы ядро не ждало его mailbox. Откат — убрать их из списка блока 0.4.275.

## 0.4.277
- Сборка 0.4.276: паник больше нет (removed isp/avd/dart-isp/dart-avd/pmp), ядро работает до лимита 4 млрд шагов, корень смонтирован (`BSD root: md0`), но застряло на повторных неудачных I2C-обменах драйвера аудио-усилителя AD5860 ("Bus is still jammed", сотни млн шагов на попытку). MODELLED (ASSUMPTION, НЕ то, что делает iBoot): из DT удаляются узлы, у которых имя или compatible содержит `ad5860` (новая функция a64_dt_node_remove_match с тестом). Это не подделка загрузки: ядро просто не видит этот усилитель. Откат — убрать блок 0.4.277.

## 0.4.278
- Сборка 0.4.277: ядро без паник доходит до AppleSEPManager::start (запись 1 в sep+0x810c) и ждёт SEP. Диагностика (поведение не меняется): поиск и покрытие кода по строкам SEPManager / "SEP " / "sep-" ([sepx]) для разбора протокола ожидания SEP.

## 0.4.279
- Сборка 0.4.278: AppleSEPManager::start дошёл до "SEP memory IS coherent" и _setPowerState, но сообщений загрузки SEP ("SEP booting", ping, Tz0 и т.д.) не было; выполнялся код со строкой "wait-sep-pg" (0x8ce3d4c) — ожидание power-gate SEP в PMGR. Горячий цикл ядра на 0x8ceaa20–0x8ceaa40, вероятно, опрос состояния SEP_PS. Диагностика: выгрузка кода 0x8ce7c80..0x8ce8300 ("SEP still running after 5s SEP_PS") и 0x8cea780..0x8ceab00 (горячий цикл). Поведение не меняется.

## 0.4.280
- Сборка 0.4.279 выгрузила старое окно кода (sed попал не в ту строку после сдвига). Исправлено: окна 0x8ce7c80..0x8ce8300 и 0x8cea780..0x8ceab00 теперь действительно в attempt.c. Поведение не меняется.

## 0.4.281
- Сборка 0.4.280: горячий цикл ядра (0x8ceaa20..0x8ceaa44) — опрос регистра 0 объекта x19 через виртуальный readReg32 (+0x700): ждёт бит 2 (tbz w20,#2), затем бит 3 после записи; перед этим ждёт бит 4 регистра 0x28. Это опрос регистра, а не mailbox. Диагностика: регистры x9..x29 в конце, выгрузка кода реализации чтения (0x8483d80..0x8484400), чтобы узнать физический адрес читаемого регистра. Поведение не меняется.

## 0.4.282
- Сборка 0.4.281: метод чтения (0x8483e74) читает [[obj+0x1a8] + reg]; объект цикла x19 = 0xffffffe0003a2d00, прочитанное значение 0. Диагностика ([mmio]): базовый указатель объекта и его физический адрес — какой блок опрашивается. Поведение не меняется.

## 0.4.283
- Сборка 0.4.282: горячий цикл опрашивает регистр 0 по физ. адресу 0x600080000 — диапазон узла apcie (PCIe-контроллер, под ним NVMe флеш-память): ждёт бит 2 (готовность/PLL), чего у нас нет, и крутится без таймаута. Корень у нас RAM-диск md0, флеш не нужна. MODELLED (ASSUMPTION, НЕ то, что делает iBoot): из DT удаляются узлы с apcie в имени или compatible (до 16). Откат — убрать блок 0.4.283.

## 0.4.284
- Прогон 0.4.283 на 12 млрд шагов: корень смонтирован (hfs: mounted Peace16A366.arm64eCustomerRamDisk), после этого за 8 млрд шагов нового вывода нет; ядро крутится в районе 0x7a6ea20 / 0x7a08820 / 0x7a090a4 (daif=0xf, таймерные FIQ идут). Последний вывод: AppleCredentialManager init, AppleMobileApNonce "requires AES accelerator". Диагностика: выгрузка кода 0x7a6e900..0x7a6ef00 и 0x7a08780..0x7a08e80, чтобы понять, что это за цикл (ожидание потока/idle или реальный опрос). Поведение не меняется.

## 0.4.285
- Сборка 0.4.284: код 0x7a6e9e0.. — штатный цикл простоя планировщика (processor idle: проверка runnable-потоков, WFI), т.е. все потоки спят, ядро ждёт событие. Из истории: bsd_init ждёт флаг +0x10b AppleARMPE, который ставит колбэк "SecureRootCallBack" кода keystore (0x83d23f0/0x83d272c); он запускается, когда появляется AppleSEPManager. В 0.4.278 AppleSEPManager::start выполнен — проверяем, дошёл ли keystore. Диагностика: [sepx] по строкам SecureRoot / KeyStore / aes_0 / "no SEP endpoint" (E = выполнялось). Поведение не меняется.

## 0.4.286
- Сборка 0.4.285 ([sepx]): код keystore выполняется (AppleKeyStore starting, _sep_enabled), SecureRootName/SecureRoot у AppleARMPE выполняются (bsd_init ждёт), но код 0x83d23f0 ("SecureRoot") и 0x83d272c ("SecureRootCallBack") — тот, что должен вызвать колбэк, — не выполняется; 0x898af7c/0x89876e4 тоже нет. Диагностика: выгрузка кода 0x83d2100..0x83d2e00, чтобы прочитать условие, при котором этот код запускается. Поведение не меняется.

## 0.4.287
- Разбор дампа 0.4.286: функция 0x83d21c4 (регистратор keystore) вызывает у платформы callPlatformFunction("SecureRoot", this) — но только если проверка `blraa [0xfffffff0078917d0]` вернула не 0; иначе ранний выход (cbz w0 -> 0x83d2600), поэтому "SecureRoot"/"SecureRootCallBack" не выполняются и bsd_init ждёт. Диагностика: значение слота 0xfffffff0078917d0 (указатель на проверку) ([slot]) — чтобы найти саму проверку. Поведение не меняется.

## 0.4.288
- Сборка 0.4.287: слот проверки 0xfffffff0078917d0 = 0xfffffff00806a96c (обычный указатель на код; соседние слоты 0x806a968/974/978 — маленькие функции-заглушки). Диагностика: выгрузка кода 0x806a800..0x806b000, чтобы прочитать эту проверку. Поведение не меняется.

## 0.4.289
- Разбор 0.4.288: проверка по слоту 0xfffffff0078917d0 — это функция 0x806a96c `mov w0,#1; ret`, т.е. всегда возвращает 1; значит условие раннего выхода в 0x83d21c4 НЕ срабатывает. Тогда регистрация SecureRoot не выполняется потому, что сама функция 0x83d21c4 (start какого-то класса keystore, vtable 0x...bf4da0) не вызывается, либо останавливается раньше. Диагностика ([skx]): покрытие точек входа/этапов 0x83d2124, 0x83d2160, 0x83d21c4, 0x83d2210/14, 0x83d22f8, 0x83d23d0, 0x83d23f8, 0x83d26e8. Поведение не меняется.

## 0.4.290
- Сборка 0.4.289: ни вход 0x83d21c4 (start класса keystore, регистрирующего SecureRoot), ни конструктор 0x83d2124 никогда не выполнялись — класс не создан/не заматчен. Диагностика ([meta]): слова OSMetaClass объекта 0xfffffff009156868 и строки, на которые они указывают — имя класса, чтобы найти, чьим провайдером он матчится. Поведение не меняется.

## 0.4.291
- Сборка 0.4.290: OSMetaClass 0xfffffff009156868: +0x10 = суперкласс fffffff00914d428, +0x18 = className OSSymbol ffffffe0005d55e0, classSize 0x90 (имена как строки в этих словах не читались: строка у OSSymbol лежит по +0x10). Диагностика ([meta]): имя класса и цепочка суперклассов до 6 уровней. Поведение не меняется.

## 0.4.292
- Сборка 0.4.291: регистратор SecureRoot — это IOAESAccelerator::start (0x83d21c4; класс IOAESAcceleratorCommand -> IOCommand -> OSObject — пул из 0x20 команд). Его не вызывают: AppleS8000AES::start печатает "registers at phys 0x235008000/0x23d2d0000", "aes-version: 3", "using-acg: no" и дальше не идёт; AppleMobileApNonce из-за этого пишет "requires AES accelerator". Диагностика: [sepx] по строкам "AppleS8000AES"/"AES" (покрытие) — где обрывается start. Поведение не меняется.

## 0.4.293
- Сборка 0.4.292 ([sepx]): AppleS8000AESAccelerator::start выполнил сообщения "registers at phys", "aes-version: 3", "using-acg: no" (0x8a6b870..0x8a6b954), но дальше "using DART Mapper" (0x8a6ba04) и всё остальное не выполнялось — start обрывается/возвращает false между ними без своих сообщений. Диагностика: выгрузка кода 0x8a6b900..0x8a6bb00, чтобы увидеть проверку. Поведение не меняется.

## 0.4.294
- Разбор дампа 0.4.293: "using DART Mapper" печатается лишь если маппер найден — его отсутствие не ошибка, так что отсутствие этого сообщения ничего не доказывает. Дальше start создаёт буфер (0x1000), объект по [0x657a68] и проверяет их (assert-строки 0x8a6ba34..). Диагностика: диапазоны реально выполненных инструкций в AppleS8000AESAccelerator::start (0x8a6b600..0x8a6c000, [ksx]) и выгрузка кода 0x8a6bb00..0x8a6c200 и 0x8a6b600..0x8a6b900. Поведение не меняется.

## 0.4.295
- Диагностика: дамп кода AppleS8000AESAccelerator::start в диапазоне 0x8a6b900..0x8a6bd00 (место остановки после 0x8a6b9ec). Поведение эмулятора не менялось.

## 0.4.296
- Диагностика: AppleS8000AESAccelerator::start останавливается внутри вызова 0x8075e48 (после cast и 0x80775bc). Дамп кода 0x8075e40..0x8076200 и 0x80775b0..0x8077680 + карта выполнения. Поведение не менялось.

## 0.4.297
- Разбор 0.4.296: 0x8075e48 = waitForMatchingService(table, без таймаута); table строится serviceMatching(строка из свойства) (0x80775bc). То есть AES-драйвер вечно ждёт сервис с неизвестным классом. Диагностика: печать строк вокруг 0x7659bd2 (имя свойства).

## 0.4.298
- Свойство "wait-for" (из настоящего DeviceTree в IPSW) задаёт имя сервиса, которого ждёт AES-драйвер. Диагностика: dt_grep теперь ищет "wait-for" и печатает значение строкой.

## 0.4.299
- Значение "wait-for" у узла aes в DT = "AppleSmartIO": AES-драйвер ждёт сервис AppleSmartIO. Диагностика: поиск узлов/свойств smartio в DT и персоналити AppleSmartIO в prelink-info.

## 0.4.300
- В DT нет узла smartio (по имени); персоналити в XML не в памяти. Диагностика: печать контекста первых 8 вхождений строки "SmartIO" в памяти ядра.

## 0.4.301
- Исправлен вывод диагностики [plist] (NUL -> "|"), 14 вхождений "SmartIO".

## 0.4.302
- Найдено: AES-драйвер вечно ждёт сервис AppleSmartIO (свойство wait-for у узла aes из настоящего DT). AppleSmartIO — драйвер на RTBuddy (сопроцессор SIO), для него в эмуляторе нет загрузки прошивки RTKit/ответчика почтового ящика.
- MODELLED/ASSUMPTION: свойство aes/wait-for переименовано в x-wait-for-disabled (`a64_dt_prop_rename`, тесты добавлены). Последствие: AES не ждёт SIO; DMA AES через SIO не работает. Откат: удалить блок 0.4.302 в attempt.c.

## 0.4.303
- Итог 0.4.302: AES-драйвер прошёл ожидание AppleSmartIO и дошёл до `_iv_dma->setMemoryDescriptor(_iv_bounce)` (AppleS8000AES.cpp:197), где IODARTMapper (mapper-sio) вернул 0xe00002bd (NoMemory) из iovmMapMemory → panic.
- Диагностика: поиск строки "Map request failed" в памяти ядра, xref (adrp+add) и дамп кода перед первым xref, чтобы разобрать, откуда берётся NoMemory. Поведение эмулятора не менялось.

## 0.4.304
- Разбор 0.4.303: IODARTMapper::iovmMapMemory (0x850aXXX) возвращает 0xe00002bd, когда виртуальный метод DART (vtable+0x5c0 или +0x5d0, выделение/отображение DVA) возвращает false. Диагностика: свойства узлов dart-sio и sio из DT (vm-base/vm-size и т.п.). Поведение не менялось.

## 0.4.305
- DT dart-sio (настоящий): sids=0x8001, vm-base=0, vm-size=0xfc000000, pio-vm-base=0xfd000000, pio-vm-size=0x2000000, page-size=0x4000, bypass=0x8000, dart-options=0xd.
- Диагностика: карта выполнения iovmMapMemory и слот vtable (+400 qword вокруг), чтобы найти виртуальные методы DART +0x5c0/+0x5d0, возвращающие false. Поведение не менялось.

## 0.4.306
- 0.4.305: в iovmMapMemory выполнен вызов vtable+0x5c0 (0x850b244), вернул false → переход на ошибку NoMemory. Диагностика: запись цели косвенного вызова по заданным PC (`wpc/wtg`), печать `[wcall]`. Поведение не менялось.

## 0.4.307
- 0.4.306: вызов vtable+0x5c0 в iovmMapMemory идёт в 0x850b684 и возвращает false. Диагностика: дамп кода 0x850b680..0x850be80. Поведение не менялось.

## 0.4.308
- Разбор метода DART 0x850b684: ветки с ошибкой — отказ vtable+0x5d0 (выделение DVA, 0x850b9c0) при [obj+0x1a8]==0. Диагностика: карта выполнения 0x850b680..0x850bc00 и цели вызовов +0x5d0/+0x6c8/+0x678. Поведение не менялось.

## 0.4.309
- Разбор 0.4.308: в методе DART 0x850b684 выполнился путь до вызова функции 0x850b600 (+0x168); её результат не 0 и не 1 → переход на ошибку (cmp w0,#1; b.ne). Диагностика: дамп кода 0x850b5c0..0x850b684. Поведение не менялось.

## 0.4.310
- Разбор 0.4.309: функция 0x850b600 проверяет, что физический диапазон буфера лежит в таблице разрешённых диапазонов DART ([obj+0x118] число, [obj+0x120] указатель; пары start/end); пустая таблица или нет попадания → -2/-1 → ошибка маппинга. Диагностика: печать объекта DART и таблицы диапазонов. Поведение не менялось.

## 0.4.311
- Итог 0.4.310: объект DART mapper-sio проверяет физический диапазон буфера по таблице из DT (`sio/map-range` = 0x23d200000, размер 0x100000), но только для адресов < 2^35 (`lsr x1,#35; cbz`). RAM эмулятора была на 0x40000000 (< 2^35), поэтому проверка проваливалась (-2) и маппинг буфера AES возвращал NoMemory.
- ASSUMPTION: база DRAM для попытки загрузки iOS перенесена на 0x800000000 (настоящая DRAM A12; новый макрос `A64M_IOS_RAM_BASE`, `a64m_create_system_at`). Общий `A64M_RAM_BASE` (0x40000000) для демо/тестов не менялся. Это приближает карту памяти к настоящему устройству, но может вскрыть новые места, где ядро/эмулятор рассчитывали на низкий адрес.
