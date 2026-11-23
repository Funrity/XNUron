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

## 0.4.312
- Результат 0.4.311: DRAM на 0x800000000 сработала. SecureRoot зарегистрирован ("BSD root: md0, major 2, minor 0", hfs mounted), ядро дошло до первого использования AES. Новая паника: `AppleS8000AESAccelerator::_enableAES: DPA has not been seeded` (assert из IOAESAccelerator::performAES; вызывающий неизвестен).
- Диагностика: дамп кода _enableAES (0x8a6c100..0x8a6c340) и start (0x8a6bb00..0x8a6c100), чтобы понять, чем засеивается DPA. Поведение не менялось.

## 0.4.313
- Разбор 0.4.312: _enableAES опрашивает регистр состояния AES (aes+0xc), пока в нём не взведены биты 0x3f80 (режим 2: 0x180), и после таймаута паникует «DPA has not been seeded». Регистр читался как 0 (неизвестный блок).
- MODELLED/ASSUMPTION/PLACEHOLDER: регистр aes+0xc читается как 0x3f80 (смысл битов выведен только из кода опроса). Это НЕ реализация AES: данные не шифруются/расшифровываются, ключи не используются; реальные запросы AES по-прежнему работать не будут.

## 0.4.314
- Результат 0.4.313: паники нет, ядро упирается в лимит шагов (4 G) и работает (таймерные прерывания, wfi 538). Последние сообщения: `AppleMobileApNonce::start: Nonce entanglement requires AES accelerator`; корень смонтирован. Куда дальше (launchd) ядро не идёт.
- Диагностика: дамп кода, где ядро проводит время (0x7a6e880..0x7a6eb00, 0x7a08800..0x7a08940), чтобы понять, чего оно ждёт. Поведение не менялось.

## 0.4.315
- Разбор 0.4.314: код ядра в конце — это idle-поток планировщика; новый код последний раз выполнялся на ~1.9 G шагов (команда `IOAESAcceleratorCommand` и ожидание её завершения в IOAESAccelerator::performAES), дальше все потоки спят. Вероятно, команда AES ждёт завершения/прерывания от несуществующего аппаратного движка AES (а он работает через SIO/RTKit DMA).
- Диагностика: трассировка обращений к MMIO блока aes (последние 300) вместо avd, чтобы увидеть протокол драйвера. Поведение не менялось.

## 0.4.316
- Итог 0.4.315: драйвер AES кладёт команду в FIFO aes+0x200 (слова 0x1b910000, 0x24000000, 0,0,0,0, 0x5a000010, 0, 0x23380, 0x41c40, 0x64000000, 0x20, 0x88000001), проверяя перед записью aes+0x24; завершения (прерывание/статус) не получает. Адреса 0x23380/0x41c40 — IOVA в DART sio.
- Диагностика: дамп кода драйвера AES 0x8a6c340..0x8a6dc00 для разбора формата команд и обработчика прерывания. Поведение не менялось.

## 0.4.317
- Разбор формата команд AES (0.4.316) показал, что движок — это FIFO со словами-командами (0x1b910000 ключ?, 0x24000000+IV, 0x5a000010+src/dst IOVA, 0x64000000+длина, 0x88000001) — для честной эмуляции нужны DART-трансляция и прерывание; это отдельная большая задача.
- Прежде чем её делать, проверяем гипотезу, что именно AES блокирует запуск launchd: диагностика [vs] теперь ищет xref строки "/sbin/launchd" (init_program_name) и показывает, выполнялись ли эти места (E). Поведение не менялось.

## 0.4.318
- Итог 0.4.317: код, использующий "/sbin/launchd" (0x7ed1db0, 0x7ed1de4), НЕ выполнялся: load_init_program/bsd_utaskbootstrap не достигнуты. Значит, bsd_init (или поток, который его вызывает) ещё где-то ждёт после монтирования корня.
- Диагностика: карта выполнения 0x7ec8000..0x7ed2000 (bsd_init) и дамп кода 0x7ed1b00..0x7ed1f00. Поведение не менялось.

## 0.4.319
- Итог 0.4.318: в области bsd_init выполнялось 0x7ec8c7c..0x7ec94a4, дальше новый код не идёт (последний исполненный диапазон кончается на 0x7ec94a4).
- Диагностика: дамп кода 0x7ec9140..0x7ec9680 (место остановки). Поведение не менялось.

## 0.4.320
- Итог 0.4.319: участок 0x7ec8c7c..0x7ec94a4 оказался функцией VFS (блокировки), не bsd_init; по нему место ожидания не найти.
- Диагностика [stk]: эвристический поиск цепочек frame-pointer в 16-КиБ ядерных стеках (0xffffffe01a000000..0xffffffe01c000000), чтобы увидеть стеки заблокированных потоков (кто и где спит). Поведение не менялось.

## 0.4.321
- Итог 0.4.320: найдено ~40 стеков; большинство — потоки старта драйверов IOKit (806c4c8/806c29c/806b550), многие спят в 814a9f0/814a91c (ожидание сервиса); один поток — командный поток AES (83d2d9c performAES/83d33b8); потоку bootstrap, вероятно, принадлежит стек 0x1b5c0000 (806d7b0/806d9dc/8067ae0).
- Диагностика [stks]: строки, на которые ссылается код перед каждым адресом возврата из цепочек, чтобы назвать функции. Поведение не менялось.

## 0.4.322
- Итог 0.4.321: десятки потоков start() драйверов IOKit (HID, baseband, apcie, MultiFunction, …) спят в 814a9f0/814a91c на ресурсе "AppleARMCPU-AllCPU-Initialized" — его никто не опубликовал. Остальные ожидания: AES (performAES; ждёт движок), SEP (OS Active), PMGR/CLPC и т.д.
- Диагностика: xref строки "AppleARMCPU-AllCPU-Initialized" (кто публикует/ждёт) и (E)-покрытие. Поведение не менялось.

## 0.4.323
- Итог 0.4.322: строка "AppleARMCPU-AllCPU-Initialized" имеет единственный xref (0x814a624, выполнялся) — это код ожидающей стороны.
- Диагностика: дамп кода 0x814a540..0x814aa40, чтобы понять, кто и когда публикует ресурс. Поведение не менялось.

## 0.4.324
- Разбор 0.4.323: xref 0x814a624 — статический конструктор, создающий OSSymbol "AppleARMCPU-AllCPU-Initialized" в глобале 0xfffffff00914e638. Ожидающие и публикующий используют этот глобал.
- Диагностика [gsym]: все места кода, читающие/пишущие глобал 0x914e638, с признаком выполнения (E). Поведение не менялось.

## 0.4.325
- Итог 0.4.324: глобал 0x914e638 читают 0x81497d8 и 0x819d8f0 (оба выполнялись) и пишет конструктор 0x814a634.
- Диагностика: дамп кода 0x8149740..0x8149a00, 0x819d860..0x819d980, 0x814a880..0x814aa40. Поведение не менялось.

## 0.4.326
- Разбор 0.4.325: функция 0x81497xx — публикатор: если глобальный счётчик (0xfffffff0091bac38) равен [this+0xec] (число CPU), вызывается IOService::publishResource("AppleARMCPU-AllCPU-Initialized", kTrue) (0x8149740+0x94). Значит, ресурс не публикуется, пока AppleARMCPU::initCPU не отработает для ВСЕХ CPU, о которых знает драйвер; с cpus=1 запущен только cpu0.
- Диагностика: значение счётчика [cpucnt], дамп кода 0x8149500..0x8149740 и 0x8149a00..0x8149c00 (откуда берётся [this+0xec]). Поведение не менялось.

## 0.4.327
- Итог 0.4.326: счётчик инициализированных CPU = 1. Ожидаемое число — [AppleARMCPU+0xec].
- Диагностика: перехват this AppleARMCPU в initCPU (0x8149510) и печать слов объекта +0xc0..+0x130 ([dobj]), чтобы увидеть ожидаемое число CPU. Поведение не менялось.

## 0.4.328
- Итог 0.4.327: ожидаемое число CPU [AppleARMCPU+0xec] = 1 = счётчику, т.е. ресурс "AppleARMCPU-AllCPU-Initialized" публикуется (проверка прошла); ждёт его только ApplePPM. Прежний вывод 0.4.321 о том, что на нём спят десятки драйверов, ОШИБОЧЕН: строки в [stks] лишь соседствуют с кодом 0x814a9f0/0x814a91c.
- Диагностика: дамп 0x814a700..0x814aa40 — что на самом деле ждут потоки в 0x814a9f0/0x814a91c. Поведение не менялось.

## 0.4.329
- Диагностика `[fnwait]`: для потоков IOKit, спящих в 0x814a91c, печатается ключ свойства (строка) и nub, которых они ждут. Только диагностика, поведение эмулятора не менялось.

## 0.4.330
- Диагностика (ничего не меняет): в DT ищутся свойства function-vbus_voltage / reset / bb_on / external_standby / dock_parent / vci / pcie_port_control / micbias-enable / dispidle_ctrl / button_volup, печатаются узел и первые слова (phandle провайдера, имя функции). Цель: понять, какой провайдер не публикует функции (чинить или моделировать).

## 0.4.331
- Диагностика 0.4.330 показала: потоки IOKit ждут platform-функции (function-reset, bb_on, vci, vbus_voltage, dock_parent, external_standby, pcie_port_control, micbias-enable, dispidle_ctrl, button_volup), провайдеры которых (GPIO, SMC, PMU, порт аксессуаров на реальных шинах) здесь не стартуют. Помощник 0x814a91c при отсутствии свойства возвращает NULL сразу (дизассемблер: cbz x20 -> выход), а не ждёт.
- MODELLED/ASSUMPTION (не то, что делает iBoot): эти свойства переименованы в `x-off-<имя>` во всех узлах DT. Последствия: драйверы получают "функции нет", оборудование НЕ управляется (сброс линий, напряжения и т.д.). Обратимо: удалить блок в attempt.c.
- `a64_dt_prop_rename` теперь принимает узел "*" (переименовать во всех узлах, возвращает их число). Тест добавлен.
- Диагностика: окно кода 0x814aa40..0x814ac00 (продолжение помощника ожидания).

## 0.4.332
- Лог 0.4.331: после скрытия функций потоки пошли дальше, но AppleM68Buttons::handleStart паникует ("failed to instantiate function-button_volup"): эта функция обязательна. `function-button_volup` больше НЕ скрывается (её поток продолжит ждать провайдера). Добавлен `function-bb_rst` (новый ожидающий). Остальные скрытые функции MODELLED/ASSUMPTION как в 0.4.331.
- Замечено отдельно: при панике обработчик паники получает data abort по адресу рядом с основанием стека (guard page, FAR=0xffffffe01b37fc58) и зацикливается — разберу, если паника повторится.

## 0.4.333
- Лог 0.4.332: гость дошёл дальше (6463 байта вывода), но AppleSOCTuner::start паникует (AppleSOCTuner.cpp:97 REQUIRE `_dispIdleCtrlFunction`): function-dispidle_ctrl обязательна, больше НЕ скрывается. Новые ожидающие функции: function-pmp_control, function-acc_pwr (пока не трогаю). Скрытые остальные: MODELLED/ASSUMPTION, как раньше.

## 0.4.334
- Лог 0.4.333: большой скачок. Гость вывел 18 616 байт: стартуют PPM, AES (с DART-маппером), I2C, SPMI-PMU, RTBuddy(SIO/SMC/GFX/ANS2), ANS2 NVMe probe, дошёл до bsd_init (proc-kqhashlock/knhashlock). Затем 3 трансляционных ошибки по ядерным VA (0xffffffe0107f8000 и т.п.; ядро их пережило) и data abort по NULL (ELR 0x82bc414, FAR 0), после чего паника-петля.
- Диагностика: снимок регистров/цепочки/слов теперь делается на ПЕРВОМ исключении, которое НЕ является трансляционной ошибкой ядерного VA (раньше строго на первом), чтобы разобрать NULL-deref 0x82bc414.

## 0.4.335
- Лог 0.4.334: падение на NULL — функция 0x82bc3xx вызывает виртуальный метод (vtable+0x538 объекта 0xffffffe0008fcc00), получает NULL и без проверки разыменовывает (ldr x8,[x0] → vtable+0x110). Вызвана из 0x82ba8fc ← 0x82c4960 ← старт драйвера IOKit. Какой это драйвер, пока не ясно; x3=-11. Диагностика: окна кода 0x82bc2c0..0x82bc440, 0x82ba840..0x82ba910, 0x82c48c0..0x82c4970 (строки/xref).

## 0.4.336
- Разбор окна 0.4.335: функция 0x82bc344 (метод драйвера start) вызывает this->vtable[0x568] (режим); при значении 1 вызывает vtable[0x538], получает NULL и разыменовывает. Строки, которые она использует, лежат около 0xfffffff00750a03b / 0x750a8e6 / 0x750a948. Диагностика `[avdstr]` теперь печатает 0x750a000..0x750aa40, чтобы узнать имя драйвера.

## 0.4.337
- Лог 0.4.336: упавший код — IOAccessoryManager::start в режиме downstream (рядом строка "downstream manager has no upstream manager"): vtable[0x538] (upstream manager) возвращает NULL и разыменовывается. Причина: я скрыл `function-dock_parent` (провайдер 'accP'). Теперь `function-dock_parent` НЕ скрывается (потоки iap/portmicro/smc-charger будут ждать провайдера, как до 0.4.331). Остальные скрытые функции: MODELLED/ASSUMPTION.

## Репозиторий: подготовка к публичности (без смены версии)
- Новые README.md (English) и README.ru.md (Русский) в стиле Material You Expressive, баннер `docs/banner.svg` (оригинальный, светлая/тёмная тема).
- README честно описывает статус: ядро iOS 12 доходит до простоя планировщика, root смонтирован, `/sbin/launchd` не запущен; PAC смоделирован без подписей.
- `docs/FEASIBILITY.md`: добавлена пометка об устаревших разделах.
- Workflows: сборка APK только для main/PR и не для правок документации; после релиза хранятся 5 последних сборок (остальные релизы удаляются); таймаут; тексты на двух языках. Boot-workflow: очередь запусков.
- Проверено: нет секретов, ключей, прошивок и крупных бинарников в дереве; лицензия проекта не выбрана (решение автора).

## 0.4.338
- Предупреждение о неподдерживаемой версии: из BuildManifest.plist читаются ProductVersion/ProductBuildVersion; если это не iOS 12.x (или в манифесте нет iPhone11,4), в журнал пишется строка `[UNSUPPORTED] …`, в приложении она красная и жирная, плюс Toast. Попытка загрузки при этом ВСЁ РАВНО запускается. Тесты на разбор версии добавлены (`a64_plist_string_value`, `a64_ios_version_supported`).
- README (EN/RU): раздел «Поддерживаемые прошивки» — только iOS 12.x на iPhone11,4.
- build.yml: после публикации хранится только последний релиз APK.

## 0.4.339
- Лог 0.4.338 (публичный репозиторий, Actions работают): версия из манифеста — iOS 12.0, build 16A366 (это и есть проверяемая сборка). Код, ссылающийся на «/sbin/launchd» в [chain] (0x8e7e004), оказался драйвером/политикой, а не load_init_program; настоящий load_init_program по-прежнему не исполнен (xref 0x7ed1db0/0x7ed1de4). Потоки BSD (dlil, flowadv, fork-код 0x7f04b5c) спят штатно.
- Диагностика: исполненные диапазоны 0x7ed0800–0x7ed2000 и окна кода 0x7ed1700–0x7ed2000 (вокруг load_init_program), чтобы увидеть, докуда доходит bsd_utaskbootstrap / что до него.

## 0.4.340
- Лог 0.4.339: диапазоны 0x7ed0800–0x7ed2000 (load_init_program) НЕ исполнялись; bsd_init до туда не дошёл. Из стеков потоков единственный глубокий поток без маркера старта потока IOKit — 0x1b400000: performAES (83d2d9c/83d33b8) под commandSleep, вызван из цепочки старта AppleMobileApNonce (…8064124/80641e0/863d87c). Гипотеза: загрузочный поток синхронно ждёт ответ несуществующего движка AES.
- MODELLED/ASSUMPTION (не то, что делает iBoot): узел DT с compatible 'aes,s8000' удаляется — «AES-акселератора нет». Обратимо. Реальная модель движка (FIFO, DART, UID = PLACEHOLDER) — отдельная задача на потом. Если гипотеза неверна, это будет видно по логу (поток продолжит ждать/появятся другие паники).

## 0.4.341
- Лог 0.4.340: узел aes удалён (AppleMobileApNonce: «requires AES accelerator»), но load_init_program по-прежнему не исполнен — гипотеза про AES НЕ подтвердилась как единственная причина (блок остаётся в DT-правке, помечен MODELLED, обратим).
- Теперь без потока AES виден поток 0x1b3c0000 с цепочкой 7ba7904 ← 803aff0 ← 800d280 («Classes», реестр OSMetaClass) ← 805bc5c ← 805bd48 ← 8067848 ← 806d9dc ← 806d7b0: он спит на rw-блокировке. Диагностика `[lockw]`: сырые слова стека вокруг кадра 800d280 и разыменование указателей на данные ядра, чтобы найти блокировку и её состояние (держит ли её кто-то, или это ошибка эмуляции эксклюзивных операций).

## 0.4.342
- Лог 0.4.341: 800d280 оказался общим помощником блокировки (mutex), а не «Classes»: им пользуются десятки потоков. Группа потоков (1b3c/1b43/1b44/1b45/1b47/1b61…) спит на ОДНОЙ и той же глобальной блокировке 0xfffffff00914c588 (цепочка 7ba7904 ← 803aff0 ← 800d280 ← 805bc5c ← 805bd48 ← 8067848 ← 806d9dc ← 806d7b0). Нужно узнать, кто её держит.
- Диагностика `[lockw]`: исправлено сравнение старших бит (раньше ни один указатель не разыменовывался), фильтр оставляет только кадры семейства 803aff0, печатаются 4 слова по каждому указателю (в т.ч. сама блокировка 0x914c588 и её владелец).

## 0.4.343
- Лог 0.4.342: заблокированная блокировка 0x914c588 — объект (vtable 0x77e5b70), на нём спят ~11 потоков драйверов (AppleMCA, AppleFAN, IOReportHub…): они ждут ресурс/сервис через IOService, а не «держателя блокировки». Это общий класс ожиданий драйверов, он не обязательно блокирует bsd_init.
- Чтобы увидеть, кто вызывает настоящий load_init_program (xref в 0x7ed0000..0x7ee0000), цепочка вызывающих [chain] теперь строится от этой ссылки, а не от первой найденной (0x8e7ef14, это драйвер). Дамп кода [stop] и hist_pc тоже берутся от неё.

## 0.4.344
- Лог 0.4.343: поиск пары adrp+add не находит ссылку load_init_program (адреса не соседние), поэтому 0.4.343 ничего не изменил. Диагностика [chain]/hist/дамп кода теперь строится от известной ссылки 0xfffffff007ed1db0 (жёстко, ASSUMPTION: только для этой сборки iOS 12.0 16A366, только диагностика).

## 0.4.345
- Лог 0.4.344: цепочка вызывающих от load_init_program: 7ed1ca8 (load_init_program) ← 7e8e870 (вызов в 7e8ea28; не исполнялась) ← 7f03128 (7f034f4; не исполнялась) ← 7a399d8 (7a39a78; не исполнялась) ← 79fd900 (X, вызов в 79ffb80; но «функция» 79fd900 растянута эвристикой на ~0x2280 байт — вероятно ошибка поиска pacibsp). Задача: понять, кто вызывает 7a399d8 и докуда доходит загрузочный поток.
- Диагностика: окна кода 7a39980..7a39b00, 7f03100..7f03500, 79ffb00..79ffc00; исполненные диапазоны 79fd900..79ffc00.

## 0.4.346
- Лог 0.4.345: эвристическая цепочка вызывающих выше load_init_program ненадёжна (7a399d8 оказался обработчиком AST, а не вызывающим). После mountroot (hfs смонтирован) ядро доходит до стационарного цикла таймерного потока, новый код больше не выполняется после ~1738 M шагов. Загрузочный поток в списке стеков не виден.
- Диагностика `[stk]`: диапазон сканирования стеков расширен вниз до 0xffffffe000000000 (раньше с 0xffffffe01a000000, и лимит 40 строк мог скрыть стек первого потока, выделенного раньше остальных), лимит строк 90.

## 0.4.347
- Лог 0.4.346 нашёл ЗАГРУЗОЧНЫЙ ПОТОК (стек 0x10690000): 7a75284 (kernel_bootstrap_thread: строки phys_carveout_mb, kdp_init, bootprofile_init) → 7e8e3a8 (bsd_init: «acct», «IOBSD», com.apple.netsrc) → 80eaa68 (IOFindBSDRoot: «BSD root: %s») → 8151cbc → блокировка (7a68fcc). «BSD root: md0» уже напечатано, корень смонтирован, но bsd_init дальше не идёт.
- Гипотеза (ПРОВЕРЯЕТСЯ, не доказана): 8151cbc — ожидание «тишины» реестра IOKit (все start() драйверов завершены); около 20 потоков запуска драйверов висят (AppleMCA, IODMAController, M68Buttons/function-*, SIO/RTKit, Reset-функции и т.д.), поэтому ожидание не кончается.
- Диагностика: окна кода 8151b80..8151d00, 80ea980..80eaa80, 7e8e300..7e8e400 — прочитать, чего именно ждёт 8151cbc.

## 0.4.348
- Дизассемблирование окна 8151b80..8151d00 (лог 0.4.347): функция, в которой спит загрузочный поток, — рукопожатие через флаги в объекте: ждёт, пока [x19+0x10a]==0 (занято), вызывает проверку (8b7660/8b76f0 над объектом [x19+0x110], результат в [x19+0x10c]), ставит [x19+0x10a]=1 и будит, затем ЖДЁТ флаг [x19+0x10b] (assert_wait + thread_block в цикле, возврат 8151cbc). Возвращает 0xe00002c1-1 при неуспехе. Это не «тишина реестра» — предыдущая гипотеза (0.4.347) НЕ подтвердилась.
- Диагностика `[strb]`: все STRB с imm 0x10a/0x10b/0x10c в тексте ядра с эвристическим началом функции — выяснить, кто пишет [0x10b] (кто должен ответить загрузочному потоку).

## 0.4.349
- Лог 0.4.348 ([strb]): флаг [obj+0x10b], которого ждёт загрузочный поток, пишется в 0x8151d80 — в ТОЙ ЖЕ функции (начало ≈8151b04). Вызовы из первого окна (8b7660/8b76f0) — это strlen/strncmp (7a091e0/7a09270): сравнивается имя с [obj+0x110]. Остальной код функции (после 8151d00) нужно прочитать, чтобы понять, какое событие ставит [0x10b].
- Диагностика: окна кода 8151b00..8151b80, 8151d00..8151e40, 8151e40..8151f80.

## 0.4.350
- Лог 0.4.349 (код 8151d00..): функция — обработчик callPlatformFunction на объекте с флагами [obj+0x10a/0x10b/0x10c]. Три ветки: (1) «установка»: сравнить имя, записать результат в [0x10c], поставить [0x10a]=1 и разбудить, затем ждать [0x10b] (в ней спит загрузочный поток, вызванный из IOFindBSDRoot); (2) «получение»: ждать [0x10a], вернуть [0x10c]; (3) ветка в 8151d3c: вызвать x21->vtable[0x3a8](символ@0x7987391, 0, [0x10c]), затем [0x10b]=1 и разбудить — именно она «отвечает» загрузочному потоку. Кто и каким символом вызывает ветку (3)? 
- Диагностика `[avdstr]`: строки 0x7987200..0x7987600 (имена платформенных функций, которыми диспетчеризует эта функция).

## 0.4.351
- Лог 0.4.350: напечатан двоичный мусор — адрес строки я посчитал неверно (adrp: страница = страница PC + imm, а не из смещения окна). Правильно: 0x8151d54 → страница 0x8151000 − 0xc7a000 = 0x7ed7000, строка 0x7ed7391. `[avdstr]` теперь печатает 0x7ed7200..0x7ed7600.

## 0.4.352
- Лог 0.4.351: строки по адресу 0x7ed7391 не нашлись (там не cstring) — ссылка считалась неверно/непрозрачно. Вместо статического чтения ставлю динамический наблюдатель на два косвенных вызова обработчика (0x8151d24 и 0x8151d70; ветка, которая «отвечает» загрузочному потоку): печатаются число вызовов, цель, x0 и x1 как строка (`[wsym]`). Если число вызовов 0 — ветка ответа не вызывалась вообще.

## 0.4.353
- Лог 0.4.352 ([wsym]): ветка 0x8151d70, которая «отвечает» загрузочному потоку (ставит [obj+0x10b]=1), за весь прогон выполнена 0 раз; другая ветка (0x8151d24) — 3 раза (цель 0x80bd89c). Т.е. ответа загрузочному потоку никто не присылает.
- Исправлена арифметика adrp: 0x8151000 − 0xc7a000 = 0x74d7000 (в 0.4.351 я ошибся в вычитании: 0x7ed7000). Строки: 0x74d7391 (символ ветки ответа) и 0x74d3be1 (символ ветки «установить»). `[avdstr]` печатает 0x74d7300..0x74d7440 и 0x74d3b80..0x74d3c80.

## 0.4.354
- Лог 0.4.353: ветка 0x8151d70 вызывается по символу **«SecureRootCallBack»** (строка 0x74d7391); ссылается на него ещё и код 0x83d272c — модуль с сообщением «Creating Special Keys Twice» (хранилище ключей SEP, тот же модуль, где performAES). Цепочка в реальном устройстве: bsd_init → IOSecureBSDRoot → AppleARMPE::callPlatformFunction("SecureRootName") → PE ждёт, пока хранилище ключей/SEP ответит через «SecureRootCallBack» → bsd_init идёт дальше. SEP и AES у нас нет, ответ не приходит, поэтому загрузочный поток спит навсегда.
- MODELLED/ASSUMPTION (НЕ то, что делает настоящее устройство): хук в pc 0x8151c9c (первая проверка флага [obj+0x10b]) отвечает вместо SEP единственным честным исходом: ставит [obj+0x10b]=1 и [obj+0x10c]=0 → функция возвращает kIOReturnNotPrivileged, т.е. «корень НЕ проверен как защищённый». Проверка secure-root НЕ проходится, криптография не подделывается; ядро продолжает как для незащищённого корня. Адрес специфичен для iOS 12.0 16A366. Обратимо: удалить блок в attempt.c. Реальный ответ SEP (ключи, UID) недоступен — это отдельная (большая) задача.
- Тест хука в tests/test.c (флаги выставляются один раз; без хука память не трогается).

## 0.4.355
- Приложение вылетало (нативный SIGSEGV, адрес 0x66) при выборе некоторых IPSW. Теперь создание попытки обёрнуто ловушкой SIGSEGV/SIGBUS: вместо падения показывается сообщение «NATIVE CRASH … Last boot step: …» с последней строкой лога. Это не скрывает ошибку, а локализует её.
- Дамп исключения теперь захватывает первое EC=0 (паническая ловушка ядра), а не первое «не трансляционное» исключение.

## 0.4.356
- ИСПРАВЛЕН вылет приложения при выборе IPSW (SIGSEGV, адрес 0x66): в диагностике `dt_has_kw` массив ключевых слов имел 8 элементов, а цикл шёл до 10 — чтение за границей массива, на Android давало мусорный указатель. Теперь размер берётся через sizeof. Ловушка из 0.4.355 остаётся.

## 0.4.357
- Диагностика: паника ядра идёт из load_init_program через Assert(file,line,expr) (lr 0x7ed1ef8). Добавлен захват x0..x3 при первом входе в 0x7a43ddc и печать строк `[cap]`, чтобы увидеть сам текст утверждения/ошибки exec /sbin/launchd.

## 0.4.358
- Диагностика: при входе в Assert (0x7a43ddc) сохраняется история PC; печатаются переходы (`[caph]`), чтобы увидеть, какой путь в exec вернул EPERM.

## 0.4.359
- Путь EPERM найден по истории PC: load_init_program (0x7ed17f4) вызывает 0x7a55204, который проходит vnode-проверки (0x7ab741c..0x7ab7b38, 0x7ba87d8). Добавлены окна кода вокруг 0x7ba8780, 0x7ab7a80, 0x7a55480, 0x7ed1800.

## 0.4.360
- Диагностика: кольцо последних RET (pc, x0) перед Assert; `[caps]` показывает цепочку возвратов, по которой EPERM (x0=1) поднялся до load_init_program.

## 0.4.361
- Окна кода load_init_program (0x7ed1700..0x7ed1f80): откуда берётся errno=1 в panic "Process 1 exec of %s failed".

## 0.4.362
- ИСПРАВЛЕНИЕ вывода: errno паники «Process 1 exec failed» по [caps] = 6 (ENXIO), а не 1 (прежний вывод был прочитан из неверного регистра Assert). [caps] печатает все возвраты с x0<0x100 из последних 512 RET.

## 0.4.363
- Цепочка возвратов 6 (ENXIO): 0x8e7c160 -> 0x7bdfcb8 -> 0x7bde444 -> ... -> 0x7ed004c -> execve. Окна кода и строки (adrp+add) вокруг этих функций.

## 0.4.364
- Строки (adrp+add) по четырём окнам по 4 КиБ вокруг 0x8e7b000/0x8e7c000/0x8e86000/0x8e87000, чтобы опознать расширение (файловую систему), вернувшее ENXIO.

## 0.4.365
- Расширение — HFS (строки hfs_lookup/hfs_inactive). Первый возврат 6 в цепочке: 0x7bc79d8 (ядро), затем HFS 0x8e52fb4... Окна кода и строки вокруг 0x7bc79d8 и 0x8e52fb4.

## 0.4.366
- Найдено: 0x7bc79d8 = buf_biowait, возвращает b_error=6 (ENXIO) для чтений HFS с md0. Образ RAM-диска (0x651aa00 байт) не кратен 4 КиБ, ядро считает размер в страницах -> хвост выходит за конец. MODELLED/ASSUMPTION: размер в /chosen/memory-map RAMDisk округляется вверх до 4 КиБ (хвост — нули). Реальное поведение iBoot неизвестно.

## 0.4.367
- Эксперимент 0.4.366 (округление размера RAMDisk) ничего не изменил (число шагов идентично) и убран. HFS при этом смонтирован успешно. Диагностика `[e6]`: кто записывает 6 в b_error ([bp+84]).

## 0.4.368
- Диагностика `[e6s]`/`[e6c]`: сеттер b_error (`str w1,[x0,#84]`) и его вызовы с `mov w1,#6` (ENXIO).

## 0.4.369
- Захват первого вызова сеттера b_error (0x7bc5890) со значением 6: lr, bp и последние переходы (`[e6h]`).

## 0.4.370
- Сеттер b_error (0x7bc5890) с 6 не вызывался: ловится первое прямое 32-битное сохранение 6 в [xN,#84] в тексте ядра (`[e6h]`: pc сохранения, lr, переходы).

## 0.4.371
- Первое сохранение b_error=6 найдено: pc 0x7b0744c (lr 0x7b07444). Окна кода и строки вокруг 0x7b07380.

## 0.4.372
- 0x7b0744c оказался ложным срабатыванием (счётчик vm_fault_copy в стеке [sp,#84]). Фильтр: база не sp/fp.

## 0.4.373
- Ловушка b_error=6 включается только после входа в load_init_program; окна кода и строки вокруг 0x809ceb0 (первая находка, возможно не связана с диском).

## 0.4.374
- Первое прямое сохранение b_error=6 после входа в load_init_program: pc 0x7bba9fc (вызов из 0x7c2f984). Окна кода и строки вокруг.

## 0.4.375
- 0x7bba954 = mdevstrategy (RAM-диск md): возвращает ENXIO, если запись md-таблицы не инициализирована (флаг mdInited) или номер устройства > 15. Дамп буфера и записи таблицы (`[e6b]`, `[e6d]`) при первом таком вызове.

## 0.4.376
- Дамп 0.4.375: запись md-таблицы для md0 (0x912e490) вся нулевая, хотя корень с md0 смонтирован. Следим за сохранениями в её size/flags, чтобы увидеть, кто ставит и кто сбрасывает mdInited.

## 0.4.377
- Найдено: md0 добавляется (размер 0x651a, потом флаг 1), а затем флаг обнуляется кодом IOFindBSDRoot (0x80eaad4). Окна кода и строки вокруг 0x80eaa00.

## 0.4.378
- Причина ENXIO найдена. Код 0x80ea9cc — IOSecureBSDRoot: при ответе SecureRootName «не подтверждено» (наш MODELLED-ответ из 0.4.354, kIOReturnNotPrivileged) ядро вызывает цикл «удалить все memory-устройства» и стирает md0 — RAM-диск, служащий корнем. Поэтому чтения md0 в exec /sbin/launchd возвращали ENXIO.
- MODELLED/ASSUMPTION: эмулятор пропускает этот цикл (переход 0x80eaaac → 0x80eaaf0) и сохраняет md0. Корень по-прежнему НЕ объявляется защищённым, проверки не проходятся, криптография не подделывается.
- Тесты: переход (jmp_pc/jmp_to) и его отсутствие.

## 0.4.379
- После 0.4.378 паники exec нет; новая первая паника идёт по пути завершения процесса (exit1 → panic): процесс 1 (launchd) стартовал и вышел/упал. Диагностика: текст первой паники по x0 (`[pfirst] x0 as text`), чтобы увидеть причину выхода.

## 0.4.380
- Диагностика `[ptext]`: поиск текста панического лога ядра ("panic(cpu N caller ...)") в RAM гостя, чтобы прочитать настоящее сообщение первой паники.

## 0.4.381
- Результат 0.4.380: настоящая паника — «initproc exited -- exit reason namespace 2 subcode 0xb» = процесс 1 (launchd) завершён сигналом SIGSEGV. То есть /sbin/launchd действительно запускался в EL0 и упал.
- Диагностика `[el0]`: счётчик и номера первых системных вызовов (SVC) пользовательского процесса и первые aborts из EL0 (esr/elr/far/sp).

## 0.4.382
- Результат 0.4.381: в EL0 исполняется dyld/загрузчик (адреса 0x1050d…, ~1362 системных вызова: stat64, open, pread, fcntl, mmap, close по кругу — загрузка библиотек из RAM-диска). Процесс 1 дошёл до этого и потом упал с SIGSEGV.
- Диагностика: последние 64 системных вызова и последние 8 abort-ов EL0 (чтобы увидеть, на чём он упал).

## 0.4.383
- Результат 0.4.382: последний abort процесса 1 — трансляционная ошибка (esr 0x92000006) по 0x1027347b0 из кода 0x10509f3a8, после чего SIGSEGV; перед этим dyld стабильно мапил/демапил библиотеки (mmap/munmap/pread). Добавлен дамп всех регистров и 64 байт кода последнего abort-а EL0 (`[el0r]`) для разбора на хосте.

## 0.4.384
- Разбор 0.4.383: последний abort — `ldr x0,[x8,#16]` с x8=0x1027347a0 (уровень-2 трансляционная ошибка, то есть таблица страниц пуста), затем SIGSEGV. Вероятно, регион, куда ссылается dyld, был демаплен/не отображён. В лог добавлены аргументы (x0,x1,x2) и возвращаемое значение каждого из последних 64 системных вызовов (mmap/munmap и т.д.).

## 0.4.385
- Разбор 0.4.384: последний вызов процесса — mach vm_allocate (-10, размер 0x4000) вернул 0, затем abort по 0x1027347b0 (внутри такой страницы) и SIGSEGV. В лог добавлено значение `*x1` после возврата из системного вызова (для vm_allocate это выданный адрес), чтобы проверить, совпадает ли он с адресом, по которому падает процесс.

## 0.4.386
- Разбор 0.4.385: mach vm_allocate вернул 0x105e04000 — это НЕ адрес падения (0x1027347b0). Значит, процесс обращается к уже существовавшему объекту. Журнал системных вызовов расширен до 1024; строка `[vmops]` печатает все операции с памятью (mmap/munmap/mprotect/vm_allocate/deallocate/protect/map) с аргументами и результатами, чтобы проверить, покрывает ли карта адрес 0x1027347a0.

## 0.4.387
- Разбор 0.4.386: dyld запросил vm_allocate (FIXED, тег dylib) на 0x1026b4000 размером 0x944000 — ядро вернуло KERN_NO_SPACE (3); следующий вызов (ANYWHERE) дал 0x10279c000, библиотека отображена туда. Процесс падает на адресе 0x1027347b0 = базовый адрес 0x1026b4000 + 0x807a0, то есть на не сдвинутом (unrebased) указателе: либо rebase не выполнен, либо первый отказ был ложным. Журнал системных вызовов увеличен до 4096, чтобы увидеть ВСЕ ранее отображённые регионы и проверить, был ли отказ NO_SPACE законным.

## 0.4.388
- Разбор 0.4.387: оба отказа KERN_NO_SPACE (0x104cd8000 и 0x1026b4000, размер 0x944000) укладываются в поведение dyld «попробуй соседний адрес, иначе любой»; первое могло быть законным (там лежат main/dyld). Падение же происходит на несдвинутом указателе.
- Диагностика: цепочка кадров и стек пользовательского процесса при последнем abort (`[el0r]`), список путей, которые процесс stat()/open() (`[paths]`) — чтобы увидеть, какие образы он грузит и в каком модуле падает.

## 0.4.389
- Разбор 0.4.388: процесс 1 — это сам launchd (читал com.apple.xpc.launchd.limits.plist, /dev/console, SystemVersion.plist, LaunchDaemons), потом он перезапустился (exec) и упал во втором запуске. Мой прежний анализ карты памяти смешивал два запуска. Теперь в `[vmops]` попадают вызовы exec/posix_spawn/execve/exit, чтобы считать карту памяти раздельно для каждого запуска.

## 0.4.390
- Разбор 0.4.389: вызов 244 (posix_spawn) — launchd породил дочерний процесс, поэтому журнал системных вызовов смешивал ДВА процесса. В записи журнала добавлен адрес таблицы страниц (TTBR0) — `<ttbr>` перед каждым вызовом, и адресное пространство последнего abort-а. Так карта памяти считается отдельно для launchd (PID 1) и ребёнка.

## 0.4.391
- Разбор 0.4.390: все 451 операции оказались с одним значением базы TTBR0 (физическая страница корневой таблицы переиспользуется после exec), последний вызов — posix_spawn(xpcproxy). Теперь к метке адресного пространства добавлен ASID (биты 63:48 TTBR0), чтобы различать процессы.

## 0.4.392
- Результат 0.4.391: два адресных пространства — ASID 4 (launchd/PID 1: читал LaunchDaemons, запустил xpcproxy) и ASID 7 (дочерний процесс, грузит библиотеки). Карта PID 1 занимает 0x104ca4000..0x105e08000 и НЕ содержит 0x1027347a0 (это диапазон дочернего процесса 0x102698000..0x103238000), хотя последний abort записан с TTBR0 PID 1. Подозрение: смешение пользовательского состояния/TTBR0 между процессами при переключении контекста.
- Диагностика: ASID у последних abort-ов и системных вызовов; tpidr_el0/tpidrro_el0/tpidr_el1/sp на последнем abort.

## 0.4.393
- Результат 0.4.392: последний abort принадлежит PID 1 (ASID 4); потомок (ASID 7) к этому моменту уже прекратил выполнение. PID 1 обращается к 0x1027347a0, которого нет в карте его вызовов mmap/vm_allocate — возможно, регион выдан ядром иначе (например, OOL-память Mach-сообщения).
- Диагностика `[vmmap]`: ищем в RAM записи vm_map_entry процессов по паре (start,end) и печатаем карту адресов из ядра, чтобы увидеть, есть ли 0x1027347a0 в карте PID 1.

## 0.4.394
- Результат 0.4.393: карта PID 1 (234 записи) начинается с 0x104c50000 и НЕ содержит 0x1027347a0; этот адрес есть только в карте дочернего процесса (смещение 0xb87a0 от его базы 0x10267c000 соответствует странице, реально отображённой у PID 1 по смещению от 0x104c50000). Значит, PID 1 прочитал указатель, перебазированный dyld-ом ДОЧЕРНЕГО процесса — страница __DATA была общей у двух процессов.
- Причина (в эмуляторе): права доступа AP не проверялись, поэтому copy-on-write страницы (ядро отображает их только для чтения и ждёт permission fault) записывались прямо в общую физическую страницу.
- MODELLED: запись по странице TTBR0 с AP[2]=1 даёт permission fault (DFSC 0x0C|уровень, WnR), страницы половины TTBR1 (ядро, APRR) по-прежнему не проверяются. Чтения и исполнение права не проверяют.
- Тест: tests/asm/mmu.S №11 (блок RO: чтение проходит, запись → ESR 0x0D, WnR, память не изменена).

## 0.4.395
- Результат длинного прогона 0.4.394 (40 млрд шагов, без паники): PID 1 жив, порождает потомков (ASID 6, 7, 8, 9). В конце процесс ASID 8 повторяет одну и ту же ошибку записи (translation fault, far 0x108c60000, регистры не меняются) — страница не отображается после обработки ядром; параллельно ядро крутится в опросе драйвера (kext, цикл вокруг 0xfffffff008eef9xx).
- Диагностика `[el0w]`: обход таблиц страниц для адреса последнего EL0-fault под его собственным TTBR0 — виден ли там дескриптор (и какой).

## 0.4.396
- Результат 0.4.395: у процесса ASID 8 дескриптор страницы 0x108c60000 в его таблицах НЕВАЛИДЕН (L3[792]=0) — ядро после fault не установило отображение, и повторы идут каждые ~84 млн шагов (поток в ядре ждёт). Подозрение: нехватка свободных страниц — ~500 МиБ из 768+101 МиБ уходят в secluded-пул (см. 0.4.18).
- ASSUMPTION: гостю даётся больше ОЗУ (пробуем 2048, 1536, 1024, 768, 640 МиБ; у настоящего XS 4 ГиБ). Память хоста выделяется лениво. Если причина не в этом, прогон покажет то же самое, и гипотеза отпадёт.

## 0.4.397
- Результат 0.4.396: гипотеза про нехватку ОЗУ НЕ подтвердилась (при 2 ГиБ тот же зависший fault у ASID 8, дескриптор страницы невалиден). ОЗУ-изменение 0.4.396 оставлено как безвредное (ASSUMPTION).
- Диагностика `[exc-calls]`: список впервые вызванных (bl) функций ядра с момента последнего EL0 data abort — путь обработки зависшего fault (vm_fault и далее), чтобы понять, где ядро теряет отображение.

## 0.4.398
- Исправление сборки 0.4.397: в arm64.c не хватало `#include <stdlib.h>` (calloc), CI-компилятор отклонил сборку. Содержимое диагностики `[exc-calls]` то же.

## 0.4.399
- Результат 0.4.398: в [exc-calls] 391 функция ядра (без символов; путь vm_fault и далее), но сам зависший поток надо смотреть по стеку.
- Диагностика `[stk]`: сканирование стеков ядра расширено до 0xffffffe040000000 (при 2 ГиБ ОЗУ стеки потоков лежат выше прежнего предела 0xffffffe01c000000), порог глубины цепочки 3.

## 0.4.400
- Результат 0.4.399: заблокированных потоков ядра ~90, все ждут (assert_wait/thread_block), по стеку не видно, какой из них — поток зависшего fault.
- Диагностика: структура потока (tpidr_el1 последнего EL0-abort) читается, и стек, на который она указывает, помечается `[stk-last-el0-thread]` — виден путь ожидания именно этого потока.

## 0.4.401
- Результат 0.4.400: ни один стек не помечен как стек потока последнего EL0-abort (в первых 0x600 байтах структуры потока нет указателя на стек, либо поток не заблокирован и не имеет стека).
- Диагностика `[thx]`: непустые слова структуры потока (tpidr_el1) — чтобы увидеть состояние потока (поля ожидания/стека) вручную.

## 0.4.402
- Результат 0.4.401: у потока зависшего fault в структуре нет указателя на стек ядра (поле продолжения +0x80/+0x2f8 заполнено) — он блокируется «с продолжением» либо вообще не заблокирован, а просто редко получает процессор.
- Диагностика `[el0w3]`: записи L3-таблицы вокруг адреса зависшего fault (±6) — видно, отображены ли соседние страницы и не обнулено ли ядром именно это отображение.

## 0.4.403
- Результат 0.4.402: в L3-таблице процесса ASID 8 страницы 786–791 отображены (RW для EL0), 792 и дальше — нет: память трогается подряд, и именно на странице 792 fault не доводится до отображения.
- Диагностика: список первых вызовов `[exc-calls]` теперь только для потока, получившего abort (раньше — все потоки), плюс `[exc-ring]` — последние 256 вызовов (место_вызова>цель) этого потока.

## 0.4.404
- Результат 0.4.403: поток зависшего fault в конце крутится в цикле: вызов 0x7b859b4 из 0x7b84618 повторяется десятки раз подряд (рядом 0x7b863a0, 0x7b83e60) — это код в диапазоне 0x7b82000–0x7b86400, а не ожидание. Значит, поток активен и застрял в цикле внутри обработки fault.
- Диагностика: окна дампа кода (`[stop] code`) направлены на 0x7b84580–0x7b84680, 0x7b859b0–0x7b85ab0, 0x7b863a0–0x7b86440, 0x7b83e60–0x7b83f20, чтобы дизассемблировать цикл.

## 0.4.405
- Разбор 0.4.404: 0x7b859b4 — диспетчер по глубине пикселя (switch по глобалу, NEON-раскладка 4-битных данных), цикл в 0x7b84580.. вызывает его с символом 0x20 (пробел) для каждого столбца — это код консоли ядра (video_console.c: отрисовка/очистка знакомест). То есть 84-млн-шаговый период между повторами fault — это отрисовка консоли, а не ожидание ресурса. Почему fault повторяется, пока неизвестно.
- Диагностика `[exc-ret]`: 256 последних вызовов (bl) потока непосредственно перед его ПОСЛЕДНИМ возвратом в EL0 — хвост обработчика fault (vm_fault и его результат).

## 0.4.406
- Результат 0.4.405: цикл консоли — это ядро рисует то, что пользовательский процесс пишет в /dev/console (отсюда ~84 млн шагов на одну запись). В журнале syscalls процесса виден write(1, …, 36 байт) с текстом, начинающимся «Waiting » — launchd (или его потомок) пишет сообщение «Waiting …» и дальше не продвигается.
- Диагностика `[con-write]`: последние 32 записи пользовательских процессов в fd 1/2 (до 128 байт, текст как есть) — сообщения launchd целиком.

## 0.4.407
- ВАЖНЫЙ результат 0.4.406 (консоль пользовательского процесса): запущенный из RAM-диска PID 1 — это образ **восстановления (Restore ramdisk)**: `restored exited normally with status 1 - rebooting`, `CHECKPOINT NOTICE: NVRAM access is not currently available`, и бесконечно `waiting for matching IOKit service: {IOProviderClass = IOResources, IOResourceMatch = IONVRAM}`. То есть launchd и restored работают, но ждут сервис IONVRAM, которого в ядре нет.
- MODELLED/PLACEHOLDER: в /chosen добавляется `nvram-proxy-data` — пустой (нулевой) образ 8 КиБ, как у ни разу не программированной NVRAM (iBoot передал бы настоящий образ устройства). Никакого содержимого не выдумываем. Цель — проверить, публикует ли ядро IONVRAM после этого.

## 0.4.408
- Результат 0.4.407: пустой `nvram-proxy-data` в /chosen сам по себе IONVRAM не публикует (консоль запущенного restored та же: «waiting for matching IOKit service … IONVRAM»). Свойство оставлено (безвредно, MODELLED/PLACEHOLDER).
- Диагностика (ничего не меняет): печать строк ядра, содержащих «nvram» (классы/свойства), с файловыми смещениями — чтобы понять, какой драйвер должен опубликовать IONVRAM на этом ядре и чего ему не хватает.

## 0.4.409
- Исправление: в 0.4.408 диагностика строк «nvram» не попала в код (сбой вставки при подготовке сборки), хотя запись в CHANGELOG была. Теперь она действительно добавлена (описание 0.4.408 остаётся в силе).

## 0.4.410
- Результат 0.4.409 (строки ядра с «nvram»): в ядре есть `IODTNVRAM`, `IONVRAMController`, `nvram-proxy-data`, `AppleARMCHRPNVRAM`, а также `IONVMeFamily` («nvme: Unable to create NVRAM device», «Set NVRAM NSID», «Failed to restore NVRAM shadow») и `AppleSEPARTStorage`/`AppleMobileApNonce`, которые сами ждут NVRAM. Вероятно, на A12 NVRAM лежит на NAND через контроллер ANS (NVMe) — аппаратуры, которой в эмуляторе нет; пустой proxy-образ (0.4.407) сервис не поднял.
- Диагностика (ничего не меняет): печать строк рядом с именами контроллеров NVRAM (окна 0x4d2080–0x4d2300 и 0x4cfe00–0x4d0000), чтобы увидеть, по каким именам/свойствам их подбирает ядро.

## 0.4.411
- Решение пользователя: делаем MODELLED-заглушку публикации IOResource «IONVRAM» (настоящий источник — ANS2 NVMe + сопроцессор ANS с RTKit, которых у эмулятора нет; это будет честно помечено MODELLED и не считаться загрузкой).
- Шаг 1 (диагностика, ничего не меняет): в ядре ищется строка `_publishIOResource: ` (AppleCredentialManager) и её ADRP+ADD-ссылки, выводится код вокруг них — оттуда видно, какой функцией ядро публикует IOResource (IOService::publishResource).

## 0.4.412
- Результат 0.4.411: строки `_publishIOResource: ` в ядре нет (адрес 0; это, видимо, `__func__` внутри формата). Диагностика теперь ищет строку-ключ `AppleUSBRestrictedModeTimeout`, которую та же функция публикует через IOService::publishResource, и печатает код вокруг её ADRP+ADD-ссылок.

## 0.4.413
- Результат 0.4.412: ключ `AppleUSBRestrictedModeTimeout` используется в функции 0xfffffff008ae8878 (xref 0x...88e4): `bl #528` → 0xfffffff008ae8afc получает (x1 = ключ, x2 = число) — вероятно обёртка, которая создаёт OSNumber и вызывает IOService::publishResource. Диагностика теперь печатает код до pc+0x400, чтобы разобрать эту обёртку.

## 0.4.414
- Разбор 0.4.413: обёртка 0xfffffff008ae8afc(ключ x1, число x2) создаёт OSNumber и вызывает `IOService::publishResource(const char *, OSObject *)` (0xfffffff008075860).
- MODELLED (решение пользователя, НЕ настоящая публикация): сразу после того как AppleCredentialManager публикует свой ресурс (инструкция 0xfffffff008ae88f0), эмулятор один раз вызывает ту же обёртку с ключом `IONVRAM` и значением 1, чтобы restored перестал ждать. За ресурсом ничего нет (ANS2 NVMe и сопроцессор ANS не эмулируются, IODTNVRAM-контроллера и хранилища нет). Адреса — ASSUMPTION для именно этого kernelcache (16A366). Лог явно называет это MODELLED.
- Новый механизм процессора `ch_pc/ch_target/ch_x1/ch_x2` (одноразовый вызов функции ядра), тест №7 в tests/test.c.

## 0.4.415
- ВАЖНОЕ ИСПРАВЛЕНИЕ К 0.4.414: MODELLED-заглушка `IONVRAM` в том прогоне НЕ была установлена (поиск строки в ядре с ошибкой на единицу: «string 'IONVRAM' was not found»), так что она ни на что не влияла. Тем не менее restored в том же прогоне перестал ждать IONVRAM («unable to set restored-exit-status»; запись в NVRAM не удалась) и перешёл к «Searching for NAND service» / «Waiting for NAND service to publish» (повторяется). Вероятная причина — пустой `nvram-proxy-data` (0.4.407) и/или таймаут ожидания у restored; точно не установлено.
- Заглушка из 0.4.414 удалена как ненужная; механизм одноразового вызова (`ch_pc…`, тест №7) оставлен в процессоре, но не используется.
- Итог: restored ждёт сервис NAND (ANS2 NVMe + сопроцессор ANS, не эмулируются). Это естественный предел без модели NAND-контроллера.

## 0.4.416
- Решение: не останавливаться на «NAND service» — пробуем честно смоделировать контроллер ANS2 NVMe поэтапно по трассе обращений драйвера. RTBuddy(ANS2) стартует в логе (0.4.415, строка «RTBuddy(ANS2): start»), то есть сопроцессор ANS управляется через ASC-обёртку (iop,ascwrap-v2) и почтовый ящик RTKit.
- Шаг 1 (диагностика, ничего не меняет): список узлов DT с совместимостью iop/ascwrap/sart/ans/nvme (имя, compat, физический адрес, размер) — чтобы узнать, где лежат регистры ASC и SART для ANS.

## 0.4.417
- Результат 0.4.416: в DT найдены окна ANS (`ans`, iop,ascwrap-v2): 0x277400000+0xc000, 0x277050000+0x4000, 0x27ad20044+4, 0x27acc0000+0x60000, 0x279000000+0x18000, 0x27ab90000+0x8000, и `sart-ans` (sart,t8020) 0x27ac50000+0x8000. Назначение окон пока ДОГАДКА по аналогии с другими ASC-узлами (aop/sio/smc/gfx-asc имеют пару …400000 и …050000); по прежней трассе pmp CPU_CONTROL читается по смещению +0x44 первого окна, а не второго.
- Диагностика (ничего не меняет): трасса `[ans-trace]` — первые 700 различных подряд обращений (чтение/запись, физический адрес+смещение=значение@PC; повторы опроса считаются, а не пишутся) ко всем блокам с «ans» в имени, плюс свойство `interrupts` узлов `ans` и `sart-ans`.
- Эмулятор по-прежнему ничего не отвечает от имени ANS; ответы будут добавляться только по этой трассе.

## 0.4.418
- Результат 0.4.417: трасса `[ans-trace]` ПУСТА — драйверы ANS/SART за 3 млрд шагов к своим регистрам не обращались (ни 'ans', ни 'sart-ans' среди 53 затронутых блоков, ни aop/sio/smc). Значит, пуск ASC для ANS не там, где «RTBuddy(ANS2): start»: драйвер NVMe-контроллера, видимо, ещё не дошёл до start() (заблокирован раньше). Свойства узла `ans`: interrupts (20 байт = 5 прерываний), nvme-interrupt-idx, nvme-coastguard, namespaces(96), iommu-parent, power-gates/clock-gates — их драйвер включает перед пуском.
- Диагностика: вместо ключа «AppleUSBRestrictedModeTimeout» ищется ссылка на строку «Found (ANS2) provider, returning score» — адрес probe() AppleANS2NVMeController в ядре; по нему можно искать заблокированные потоки этого драйвера среди `[stk]`.

## 0.4.419
- Результат 0.4.418: строка «Found (ANS2) provider, returning score» в ядре не найдена (адрес 0) — формат, видимо, «Found (%s) provider, …». Теперь ищется «provider, returning score» (до 6 ссылок, окно 160 байт до начала строки), дамп по 0x80 байт у каждой ссылки.

## 0.4.420
- Результат 0.4.419: probe() драйверов ANS2 находится около 0xfffffff008cbf874. Среди заблокированных потоков ядра два стоят в цепочке 0x8d24c58 → 0x79ff9e8 → (ожидание) и 0x8d25f70 → 0x79ff9e8 → … → thread_block (0x7a6b580): функции из того же блока кода (0x8d24…0x8d25…), похоже драйвер ждёт (замок/событие с ожиданием).
- Диагностика: окно «строки, на которые ссылается код» направлено на страницы 0x8d24000, 0x8d25000, 0x8d26000 и 0x8cbf000 — по именам функций в строках журнала понять, чей это код и чего он ждёт.

## 0.4.421
- Результат 0.4.420 (строки кода у probe, 0x8cbf000): здесь `AppleANS2NVMeController::start`, `WaitDeviceActive()`, `AppleEmbeddedNVMeNVRAM::sync()` — то есть это и есть драйвер NAND/NVRAM («IODeviceTree:/defaults» → «dual-spi-nand», «product-name» → «FastSim», «burnin-size», **«ANS2Endpoint1»**, поля fSlaveProcessor, fAKFIOPNub (IOP-nub RTBuddy), fARMIODevice, fBaseRegisterMap). Буфер строк был полон и дальше 0x8cbffe0 не показал.
- Один из заблокированных потоков (стек …8ca0b50 → 8075e84 → 806cb60 → 7a5a3e0 → ожидание) — из блока кода 0x8ca0… того же расширения, вызывает ожидание службы (рядом с IOService::publishResource).
- Диагностика: окна строк теперь 0x8ca0000, 0x8d24000, 0x8d25000, 0x8d26000, буфер увеличен до 12000 байт.

## 0.4.422
- Результат 0.4.421: заблокированный поток из 0x8ca0… — это `NVMeSEPNotifier::WaitForNVMe()`, он ждёт службу «AppleEmbeddedNVMeController» (потребитель, а не сам драйвер). Сам `AppleANS2NVMeController::start` пока не наблюдался (нет обращений к MMIO ans).
- Диагностика `[cov-ans]`: какие инструкции в коде драйвера 0x8cbf000..0x8cc3000 реально выполнялись (по покрытию), — видно, доходит ли start() и где останавливается.

## 0.4.423
- Результат 0.4.422 (покрытие): `AppleANS2NVMeController::start` ВЫПОЛНЯЕТСЯ: пройдены участки 0x8cbf6b4…0x8cbfc88 (до создания «ANS2Endpoint1», строка у 0xbfc68), и дальше выполнение этой функции не продолжается — последняя выполненная инструкция 0x8cbfc88, т.е. вызов внутри застрял/не вернулся. Параллельно выполнялись маленькие участки 0x8cc0980… (другая функция того же класса).
- Диагностика: дамп кода 0x8cbfb80–0x8cbfd00 и 0x8cc0980–0x8cc0b00, чтобы разобрать, какой вызов стоит на 0x8cbfc88.

## 0.4.424
- КЛЮЧЕВОЙ результат 0.4.423 (разбор кода): в `AppleANS2NVMeController::start` на 0x8cbfc68…0x8cbfc88 создаётся словарь поиска по имени «ANS2Endpoint1» и вызывается `IOService::waitForMatchingService(dict, timeout = -1)` — ожидание БЕЗ ТАЙМАУТА службы «ANS2Endpoint1». Эту службу создаёт RTBuddy, когда сопроцессор ANS загрузился и по RTKit-протоколу (EPMAP) сообщил об эндпойнте 1. Поэтому NVMe-драйвер (а с ним NAND/IONVRAM для restored) ждёт реальной загрузки ANS: нужна модель ASC-почтового ящика и RTKit-рукопожатия. Подсказка про RTKit была верной.
- Но к MMIO ans RTBuddy пока не обращается (трасса пуста), т.е. пуск ASC ещё не начался. Диагностика: ссылки на строку «RTBuddy(%s): start(%p) - (…)» (где RTBuddy::start) — следующий шаг: покрытие этого кода, чтобы увидеть, где RTBuddy останавливается до пуска сопроцессора.

## 0.4.425
- Результат 0.4.424: код RTBuddy::start (строка «RTBuddy(%s): start») ссылается из 0xfffffff0082e85ec и 0xfffffff0082e86e4.
- Диагностика: `[cov-ans]` теперь для двух областей — драйвер ANS2 (0x8cbf000…) и RTBuddy (0x82e8000…0x82ec000); дамп кода 0x82e8400–0x82e8800 (4 окна по 0x100 байт). Цель: увидеть, до какого места RTBuddy::start доходит и почему не пускает сопроцессор ANS.

## 0.4.426
- Результат 0.4.425: RTBuddy::start выполняется далеко (покрытие смешивает все экземпляры RTBuddy: aop, smc, sio, gfx, ans), но по покрытию нельзя отделить экземпляр ANS. Кто и когда публикует службу «ANS2Endpoint1» — неизвестно.
- Диагностика (вместо списка «nvram»): строки ядра со словом «endpoint» (форматы имён эндпойнтов RTKit/IOP, классы Endpoint) — чтобы понять, какой код создаёт «ANS2Endpoint<N>».

## 0.4.427
- Результат 0.4.426: строки «endpoint» не назвали издателя «ANS2Endpoint1». Зато в стеке заблокированного потока виден AppleA7IOP::start (драйвер IOP-копроцессора), застрявший в IOFilterInterruptEventSource → IOEventSource (адреса возврата 80a2b1c, 80a1ea0, 809d0cc, 80bc488, 80639a0, 8063c44, 7a5a3e0). Пока не доказано, что это именно ANS.
- Диагностика: дамп кода вокруг этих адресов, чтобы выяснить, на каком ожидании (замок/воркцикл) блокируется поток.

## 0.4.428
- Дамп 0.4.427: AppleA7IOP::start спит в коде 0x80bc400 (IOWorkLoop/IOService): `casa` по глобальному замку [0x…91ba7c8]; если замок занят, поток уходит в sleep с событием [0x…91ba7d0]. Значит поток ждёт замок, который держит другой поток.
- Диагностика: печать слова замка, его владельца (структура потока) — чтобы найти, кто держит замок и чего он ждёт.

## 0.4.429
- Результат 0.4.428: замок [0x…91ba7c8] сейчас свободен (слово 0) — AppleA7IOP::start не в дедлоке, а ждёт публикации службы (цикл ожидания на условной переменной в функции 0x80bc408).
- Диагностика (только чтение): запись x0/x1/lr при первых 16 входах в 0x80bc408 и эвристический вывод строк из объекта-словаря совпадения (3 уровня указателей), чтобы увидеть, какую службу ждёт драйвер.

## 0.4.430
- Результат 0.4.429: оба зафиксированных входа в функцию ожидания службы ждут «IOInterruptController00000014» (phandle 0x14 = узел aic: у sio interrupt-parent=0x14). Записей ядра в регистры AIC нет (`[aicw]` пуст) — драйвер контроллера прерываний не стартует/не регистрируется, поэтому AppleA7IOP (и, вероятно, ANS) ждёт его.
- Диагностика: поиск в ядре строки «IOInterruptController%08X», дамп кода вокруг её ссылок — найти, кто регистрирует контроллер и при каком условии.

## 0.4.431
- Исправление диагностики 0.4.430: ссылки находились с окном ±160 байт и показали соседние строки (создание ключей OSSymbol), а не «IOInterruptController%08X». Теперь: строка должна начинаться после NUL, цель ADRP+ADD — точное совпадение.

## 0.4.432
- Ошибка в 0.4.431: опечатка в строке формата (лишняя кавычка) — сборка не компилировалась, а я запушил коммит, не заметив, что тесты упали. Исправлено; теперь `run_all.sh` проходит.

## 0.4.433
- Результат 0.4.432: «IOInterruptController%08X» используется в одной функции ядра (0x8058a74: snprintf имени по phandle → OSSymbol); значит, регистрирующий AIC драйвер вызывает ту же функцию. Записей в регистры AIC нет.
- Диагностика: вместо узла dart-sio печатаются свойства узла `aic` (значения compatible, phandle) — чтобы выяснить, какой класс ядра должен его запускать.

## 0.4.434
- Результат 0.4.433: узел `aic`: compatible «aic,1», aic-version 2, #main-cpus 6, phandle 0x14, interrupt-cells 1, регистры 0x23b100000 (0x8000) и 0x23b108000 (0x1000).
- Диагностика: строки ядра, содержащие «aic,» или «AppleInterrupt…» — найти класс драйвера AIC и его личность соответствия.

## 0.4.435
- Результат 0.4.434: в ядре есть класс AppleInterruptController (AIC-44.200.1). Его start проверяет длину DT-свойства ipid-mask (у нас 64 байта), печатает _aicVersion/_aicBaseAddress/число IRQ/«Num Shared Timestamps» и регистрирует контроллер. Записей в регистры AIC в логе нет.
- Диагностика `[aic-info]`: число чтений/записей блока aic и первые обращения (смещения, значения) — дошёл ли AIC start до регистров и что он прочитал.

## 0.4.436
- Результат 0.4.435: `[aic-info] reads=0 writes=0` — ядро не сделало НИ ОДНОГО обращения к блоку aic за весь прогон. Значит, AppleInterruptController::start либо не вызывается, либо останавливается до чтения регистров (поэтому «IOInterruptController00000014» не регистрируется).
- Диагностика: ссылка на строку «AppleInterruptController::start: _aicVersion…» и дамп кода рядом (адрес start/регистров) — затем покрытие этого диапазона.

## 0.4.437
- Результат 0.4.436: код AppleInterruptController::start найден рядом с 0x8b7e7d4 (мапинг памяти провайдера, чтение регистров AIC через vtable[1448], печать _aicVersion…).
- Диагностика: покрытие (`[cov-ans]`) теперь для диапазона 0x8b7e000–0x8b7f800 (вместо RTBuddy) — исполняется ли start и где останавливается.

## 0.4.438
- ВАЖНАЯ ПОПРАВКА: выводы 0.4.428–0.4.436 (про ожидание IOInterruptController00000014 и «AIC не стартует») сделаны по коротким прогонам в 500M шагов и были ошибочны: на прогоне в 3G шагов AIC работает (72 чтения, 752 записи, AppleInterruptController::start выполняется). Настоящая проблема прежняя: ANS2 start ждёт «ANS2Endpoint1», к окнам ANS обращений нет. Дальше только длинные прогоны.
- Диагностика: полный дамп кода AppleANS2NVMeController::start (0x8cbf680–0x8cbfd00), чтобы прочитать, что он делает до ожидания и кто должен создать RTBuddy.

## 0.4.439
- Результат 0.4.438: AppleANS2NVMeController::start сам RTBuddy НЕ создаёт: он мапит регистры (индексы 3 и др.), читает свойства и ждёт службу «ANS2Endpoint1» (nameMatching + waitForMatchingService, таймаут -1). Создавать её должен другой драйвер на узле `ans` — AppleA7IOP/RTBuddy (iop,ascwrap-v2).
- Диагностика: дамп кода 0x82db200–0x82db800 (AppleA7IOP::start), чтобы найти вход функции и поставить на него наблюдатель.

## 0.4.440
- Результат 0.4.439: вход AppleA7IOP::start = 0xfffffff0082db540 (x0 = this, x1 = provider; сначала super::start, затем разбор DT-свойств «role» и др.).
- Диагностика: наблюдатель переставлен с 0x80bc408 на 0x82db540 — записываются первые 16 вызовов (this/provider/lr) и печатаются строки объекта provider, чтобы увидеть, для каких узлов (aop, sio, smc, ans…) вызывался A7IOP::start. Прогон 3G шагов.

## 0.4.441
- Результат 0.4.440: у 0x82db540 за прогон только 6 вызовов, и x1 — не провайдер, а статические объекты в __DATA (внутри — имена классов AppleA7IOPV4, AppleASCWrapV2, AppleT8020MemCacheController…, т.е. это OSMetaClass). Значит либо моя идентификация 0x82db540 как start неверна (вероятнее — вызов из кода метакласса), либо это другой путь. Сомнительно; вывод не делаю.
- Диагностика: покрытие `[cov-ans]` теперь для 0x82db000–0x82dba40 — какие куски этой функции действительно исполнялись.

## 0.4.442
- Результат 0.4.441: AppleA7IOP::start исполняется до конца, обращений к окнам ANS нет — запуск копроцессора, вероятно, идёт позже (питание). Поток ApplePMGR::start (816be24→8178864→814a91c) и ещё один (81613c0→8075e84) ждут в одной и той же функции (возврат 806cb60 → сон 7a5a3e0): ожидание службы.
- Диагностика: дамп кода 0x806c900–0x806cc00 (функция ожидания) и мест вызова в PMGR.

## 0.4.443
- Результат 0.4.442: ApplePMGR::start (814a9ec) вызывает функцию ожидания 0x806c958 с таймаутом -1 (захват мьютекса, сон до пробуждения) — ждёт объект, созданный по соответствию (814a9d8). Что именно — неизвестно.
- Диагностика: наблюдатель на 0x806c958 пишет кольцо из последних 16 вызовов (x0 = ожидаемый объект, lr) и печатает строки объекта; корень дерева теперь x0. Прогон 3G.

## 0.4.444
- Результат 0.4.443: дерево строк читалось после освобождения памяти (мусор), но вызов из PMGR (lr=814a9f0) показал «IOPropertyMatch/IOPropertyExistsMatch» — PMGR ждёт службу по свойству.
- Диагностика: снимок дерева строк (до 4 уровней) объекта ожидания в момент первого вызова из PMGR; чтение без побочных эффектов (без aborts).

## 0.4.445
- Результат 0.4.444 (3G-прогон): ApplePMGR::start ждёт (таймаут -1) службу с существующим свойством «IOFunctionParent00000016» — родителя платформенных функций с phandle 0x16. Пока она не опубликована, PMGR не завершает start.
- Диагностика: список DT-узлов с AAPL,phandle = 0x14 (контроль: aic) и 0x16 — чтобы узнать, какой узел/драйвер должен опубликовать эту службу.

## 0.4.446
- Результат 0.4.445: phandle 0x16 — это сам узел `pmgr` (pmgr1,t8020), phandle 0x14 — `aic`. Значит, клиент PMGR (поток 816be24, «pmgr-dock-fifo-client») ждёт публикации самого драйвера PMGR («IOFunctionParent00000016»); PMGR, в свою очередь, не завершил start.
- Диагностика: второй снимок — для ожидания с lr=0x8075e84 (поток 81613c0, вероятно сам PMGR::start): что ждёт он.

## 0.4.447
- Результат 0.4.446: второй ждущий поток (81613c0) ждёт IOResources «IONVRAM» (IOResourceMatch). Ресурс публикуется реальным NVRAM-стеком (на A12 — через ANS2/NAND), а мы его не эмулируем. Это замыкает круг: PMGR-ветка ← IONVRAM ← ANS ← питание ← PMGR. Вывод 0.4.415 «заглушка не нужна» был неверен (она тогда ещё и не устанавливалась).
- MODELLED (решение пользователя из AskUserQuestion, НЕ реальное): одноразовый вызов kernel-обёртки publishResource с ключом «IONVRAM» возвращён — теперь с исправленным поиском строки (0.4.414: лишняя проверка байта после NUL). В отчёт добавлен `[ionvram-standin] fired=`: сработал ли вызов. За ресурсом нет хранилища NVRAM.

## 0.4.448
- Ошибка 0.4.447: заглушка IONVRAM искала строку в образе ядра уже ПОСЛЕ `free(kern)` (use-after-free): локально тест проходил, на CI процесс падал по segfault, сборка APK и прогон не выполнились. Блок перенесён ДО освобождения ядра. Проверено под AddressSanitizer (ASAN) — ошибок нет.
- Заодно найдено ASAN-ом и исправлено: диагностика `interrupts` читала неинициализированный буфер, если свойство не найдено.
- Отдельно: в 0.4.447 публикация лога упала с HTTP 400 (сбой GitHub при загрузке в релиз) — повтор.

## 0.4.449
- Результат 0.4.448: заглушка IONVRAM сработала (`fired=1`), CI зелёный, но набор заблокированных потоков не изменился (89 стеков, те же 806cb60/8075e84). Среди ждущих: ApplePMGR-клиент (816be24), RTBuddy (82e94ec←82e8a54: поток 3bb60000), 8ca0b50, 8384740. Ожидаемые ресурсы, кроме IONVRAM, неизвестны. `[ans-trace]` по-прежнему пуст.
- Диагностика: снимки строк для ВСЕХ ожиданий ресурса (lr=0x8075e84, до 24 штук), чтобы увидеть имена ресурсов (`[wait-resN]`).

## 0.4.450
- Результат 0.4.449 (имена ожидаемых служб, 3G-прогон): RTBuddy (поток 82e94ec) ждёт службу «RTBuddyFirmwareService» (IOPropertyExistsMatch); ещё ждут: AppleEmbeddedNVMeController (NVMeSEPNotifier), IODTNVRAM (класс платформенного устройства), IOCoreSurfaceRoot/AGXFirmware…RTBuddy (GPU). Т.е. RTBuddy ждёт службу, поставляющую прошивку/данные IOP.
- Диагностика: строки ядра, начинающиеся с «RTBuddyF…», «RTBuddyIO…», «firmware…», «AppleANS2…» — найти, какой класс/личность должна публиковать RTBuddyFirmwareService.

## 0.4.451
- Результат 0.4.450: в ядре RTBuddy грузит прошивку IOP через пользовательский клиент (RTBuddyFirmwareService, право «com.apple.RTBuddy.load-firmware», AppleA7IOP::startCPU(IOSlaveFirmware*), _handlePreloadFirmware). RTBuddy ждёт, пока userspace-демон (в iOS — rtbuddyd) откроет эту службу. Значит, вопрос не в «железе ANS», а в том, запущен ли такой демон в нашем PID 1 (restore ramdisk) и до чего он дошёл.
- Диагностика: `[paths]` теперь 95 байт на путь (было 39), без *.dylib, плюс пути posix_spawn/execve — видно, какие демоны запускает launchd.
- Исправлено (найдено тестом JNI): `runDemo` держал структуру CPU (теперь > 1 МБ из-за диагностических колец) на стеке потока — переполнение стека JVM (на Android тоже опасно). Теперь выделяется в куче.

## 0.4.452
- Результат 0.4.451 (3G): launchd ramdisk запускает только xpcproxy → restored_external (плюс HorizonSetup, syslogd, ReportCrash, PurpleReverseProxy); rtbuddyd в списке НЕТ. Среди ядерных строк — RTBuddyFirmwareBundle, firmware_service, _handlePreloadFirmware, «Selected firmware does not match the target IOP», AppleFirmwareDecodeImage4 (прошивка IOP может поставляться kext-ом/пакетом внутри ядра, как у AGXFirmwareKextG11PRTBuddy). Что именно ждёт «RTBuddyFirmwareService» и относится ли оно к ANS — не доказано.
- Диагностика: контексты «FirmwareService» в образе ядра (prelink-plist/строки, ±170 байт) — найти, какие kext/личности публикуют эту службу.

## 0.4.453
- Результат 0.4.452: «RTBuddyFirmwareService» — это IOMatchCategory kext-личностей (например, AppleSPUFirmwareService-AOP / -SPU: IOProviderClass RTBuddyService, IOPropertyMatch role=AOP/SPU) внутри ядра. То есть прошивку IOP поставляют драйверы внутри кернелкэша по роли; пользовательский демон не обязателен. Ждущая копия RTBuddy (res18) — вероятно, AOP/SPU, а не ANS (не доказано).
- Диагностика: все личности с IOMatchCategory RTBuddyFirmwareService (имя/role/провайдер) — есть ли среди них роль ANS.

## 0.4.454
- Результат 0.4.453: личностей с IOMatchCategory RTBuddyFirmwareService всего четыре: AppleSPUFirmwareService для ролей AOP, SPU, SPU2 и AppleT8020PMPFirmware (роль PMP). Роли ANS среди них НЕТ — значит ожидание «RTBuddyFirmwareService» относится к AOP/SPU/PMP, а не к ANS (прошивку ANS, как у реального железа, ядро, по-видимому, не загружает).
- Диагностика: личности в prelink-plist, упоминающие «iop,ascwrap», узел `ans`, AppleANS2NVMeController, RTBuddyService — какой класс/провайдер должен создать RTBuddy и «ANS2Endpoint1».

## 0.4.455
- Результат 0.4.454 (+3G-прогон): в prelink-plist AppleASCWrapV2 (iop,ascwrap-v2) → AppleA7IOPNub → RTBuddyV2 (iop-nub,rtbuddy-v2) → AppleANS2NVMeController (IOProviderClass=RTBuddyV2). Значит RTBuddyV2(ANS) существует, а запуск ASC позже идёт, вероятно, через питание. В 3G-прогоне ждёт один поток (816be24→8178864→814a91c): клиент «pmgr-dock-fifo-client self pmgr» ждёт свойство «IOFunctionParent00000016» (phandle 0x16 = сам pmgr). Кто выставляет это свойство — неизвестно.
- Диагностика: ссылки на строку «IOFunctionParent%08X» (кто её создаёт/выставляет) + дамп кода рядом.

## 0.4.456
- Результат 0.4.455: строка «IOFunctionParent%08X» имеет одну ссылку (0x814a8a8, функция 0x814a870 — построитель имени по phandle). Функция 0x814a944 — ожидание: берёт свойство «function-…» (OSData ≥ 8 байт), строит соответствие по имени и ждёт службу бессрочно (806c958, таймаут -1). Кто ВЫСТАВЛЯЕТ это свойство службе — по строке не видно (то же построение имени).
- Диагностика: вызывающие (BL) функций 0x814a870, 0x814a8ec, 0x814a944 — найти установщик свойства.

## 0.4.457
- Результат 0.4.456: функция 0x814a7a0 — УСТАНОВЩИК: читает у nub свойство (phandle), строит имя «IOFunctionParent%08X» и делает `service->setProperty(name, …)`. Значит свойство выставляется, когда этот код вызывается для ApplePMGR (phandle 0x16).
- Диагностика: вызывающие (BL и B) 0x814a7a0 — кто и когда должен его вызвать (вероятно, ApplePMGR::start/registerService), и почему этого не происходит.

## 0.4.458
- Результат 0.4.457: установщик 0x814a7a0 вызывается из 8 мест (0x8151988, 0x815a3dc, 0x815ab80, 0x815cc34, 0x8161cec, 0x81672ac, 0x8168960, 0x816bdc8). Последний лежит прямо перед адресом возврата 0x816be24 ждущего потока (функция, вызывающая ожидание), т.е. ту же функцию, где поток застрял.
- Диагностика: дамп кода 0x816bb00–0x816c100, чтобы увидеть порядок: сначала установка собственного IOFunctionParent или ожидание, и что стоит между.

## 0.4.459
- Результат 0.4.458 (дамп ApplePMGR::start): 0x816bdc8 вызывает установщик 0x814a7a0(nub, this) ДО виртуального вызова (+1384, 0x816be20), внутри которого поток ждёт «IOFunctionParent00000016». Установщик молча возвращает ошибку (0xe00002bc), если у nub нет свойства-phandle или оно не OSData. Свойство-ожидание на самом PMGR должно было быть выставлено этим вызовом.
- Диагностика: наблюдатель на 0x814a7a0 — число вызовов и снимок строк nub для вызова из ApplePMGR::start (lr 0x816bdcc): есть ли у nub phandle-свойство.

## 0.4.460
- Результат 0.4.459: установщик вызывался 44 раза; у nub первого вызова из ApplePMGR::start видны ключи IOName/IOLocation/function-clock_gate/bridge-reg-index (phandle-свойство по снимку не подтверждено и не опровергнуто: снимок показывает лишь часть ключей).
- Диагностика: второй наблюдатель на 0x816bdcc (после вызова установщика) пишет его возвращаемое значение x0 (0 = успех, 0xe00002bc = плохой аргумент).

## 0.4.461
- Результат 0.4.460: установщик в ApplePMGR::start вернул 0 (успех) — свойство IOFunctionParent00000016 выставлено на ApplePMGR (this=0xffffffe00044c000). Значит поток ждёт службу с этим свойством, но не находит её: либо PMGR ещё не «registered» (matching видит только зарегистрированные службы, а регистрация обычно в конце start — тупик самому на себя), либо ожидание не пробуждается.
- Диагностика `[pmgr-obj]`: слова объекта ApplePMGR в конце прогона (флаги __state, kIOServiceRegisteredState=0x2) — зарегистрирован ли он.

## 0.4.462
- Результат 0.4.461: у ApplePMGR (this=0xffffffe00044c000) в конце прогона состояние __state[0]=0x1e (registered 0x2, matched 0x4, firstPublish 0x8, firstMatch 0x10) — PMGR зарегистрирован и «matched», свойство IOFunctionParent00000016 выставлено, а поток всё ещё спит в ожидании. Подозрение: потерянное пробуждение (wakeup) или ошибка планировщика/блокировки в эмуляции, а не отсутствие железа.
- Диагностика `[pmgr-waitobj]`: слова объекта ожидания (x0 вызова 0x806c958 из PMGR-клиента, lr 0x814a9f0) в конце прогона — выставлен ли в нём результат при спящем потоке. Наблюдатель 0x806c958 возвращён.

## 0.4.463
- Результат 0.4.462: `[pmgr-waitobj]` не напечатан — кольцо последних 16 вызовов уже вытеснило нужный вызов (lr 0x814a9f0). Теперь адрес x0 первого такого вызова запоминается отдельно (`wp_pm_x0`).

## 0.4.464
- ПОПРАВКА к 0.4.459–0.4.462: застрявший поток — AppleARMCPU::start (рядом строки «Primary%ld», «AppleARMCPU-AllCPU-Initialized»), а не ApplePMGR::start; объект с __state 0x1e и nub с function-clock_gate — это CPU-драйвер. Функция 0x806c958 — waitForMatchingService(dict, timeout): она ищет службы с kIOServiceMatchedState. AppleARMCPU ждёт, пока драйвер ApplePMGR (phandle 0x16) получит IOFunctionParent00000016 и станет «matched»; сам PMGR, видимо, не стартует/не завершает start.
- Диагностика: строки ядра «ApplePMGR::…»/«pmgr…» — найти код ApplePMGR::start.

## 0.4.465
- ОТМЕНА поправки 0.4.464: она была неверной. В логе 1500M адрес возврата 0x816be24 лежит внутри ApplePMGR::start (рядом строки "ApplePMGR::start" и "_regMaps[kRegMapPMGR].memMap"), сеттер 0x816bdcc — там же. Значит застрял именно ApplePMGR::start в waitForMatchingService("IOFunctionParent00000016"); выводы 0.4.459–0.4.462 верны. Причина ожидания пока не найдена (самоблокировка на kIOServiceMatchedState или потерянное пробуждение).

## 0.4.466
- Найдена вероятная причина зависания ApplePMGR::start: цепочка стека waitForMatchingService ← ожидание функции-родителя ← метод около «pmgr-dock-fifo-client» ← ApplePMGR::start. Узел 'pmp' удалён нашим же DT-фиксом (0.4.276, RTBuddy PMP паниковал «No response received»), поэтому провайдер function-pmp_control никогда не появляется. Это следствие нашего обхода, а не ошибка эмуляции (гипотеза, ещё не проверена прогоном).
- ЭКСПЕРИМЕНТ (MODELLED, ASSUMPTION): свойство 'function-pmp_control' переименовано в 'x-off-pmp_control' (как другие function-* в 0.4.331), чтобы PMGR не ждал отсутствующий PMP. Реальная PMP-прошивка/почтовый ящик не моделируются.

## 0.4.467
- Прогон 2000M с переименованным function-pmp_control (0.4.466) НЕ помог: стек ApplePMGR::start всё так же ждёт (… 806cb60 814a9f0 814a91c 8178864 816be24). Гипотеза про 'pmp' не подтвердилась; переименование убрано (откат, чтобы не оставлять ничем не подтверждённый фикс).
- Диагностика (ничего не меняет): печать первых 6 вызовов waitForFunctionParent (0x814a8f0): провайдер, x1 и строка имени функции — чтобы узнать, чьего function-родителя ждёт PMGR.

## 0.4.468
- Лог 0.4.467 записал первые 6 вызовов waitForFunctionParent — все не из PMGR (cpu_idle, bb_rst, set_dbl_click_ms, button_volup, device_reset). Теперь фильтр: только вызовы с адресом возврата 0x8178864 (из ApplePMGR::start), чтобы увидеть имя функции, которую ждёт PMGR.

## 0.4.469
- ПОДТВЕРЖДЕНО диагностикой 0.4.468: ApplePMGR::start (возврат 0x8178864) вызывает waitForFunctionParent для "function-pmp_control" на PMGR-узле — ждёт провайдера (узел 'pmp', удалённый нашим DT-фиксом 0.4.276).
- ОШИБКА 0.4.466: правка попала не в тот массив (диагностический dt_has_kw), поэтому переименование не применялось и «неудача» 0.4.466 ничего не доказывала; в 0.4.467 откатил именно её. Теперь 'function-pmp_control' добавлен в настоящий список скрываемых функций (MODELLED, ASSUMPTION, как другие function-* из 0.4.331); PMP-прошивка не моделируется.

## 0.4.470
- Прогон 0.4.469 (2000M): ApplePMGR::start больше не висит (function-pmp_control скрыт, MODELLED). Следующая остановка: ApplePMGR::initPMP() паникует «ApplePMGR.cpp:2336 REQUIRE failed: _params[i].value == value» (возврат 0x8178894) — ядро сверяет прочитанное значение с записанным; наши заглушки PMP читаются как 0. Вторичные эффекты после паники: исключения udf, затем ошибка данных на стеке.
- Диагностика: окна дампа кода сдвинуты на 0x8178600..0x8178c00 (тело initPMP), чтобы увидеть, какой регистр перечитывается.

## 0.4.471
- Разбор кода initPMP (0x8178824..): паника «ApplePMGR.cpp:2336 REQUIRE failed» — это проверка, что waitForFunctionParent("function-pmp_control") вернул НЕ NULL; строка «_params[i].value == value» в логе была соседней строкой, а не проверкой. Значит PMGR действительно требует службу PMP; скрытие свойства (0.4.466/469) — ошибочный обход, откатан.
- Откат: узлы 'pmp'/'pmp0' больше не удаляются из DT (удаление 0.4.276 убрано), function-pmp_control не скрывается. RTBuddy PMP снова запустится и будет ждать ответа ASC — это и есть задача для честной MODELLED-модели ASC/RTKit.
- Трассировка MMIO (ans-trace) теперь включает и блоки 'pmp' (0x23bc00000 / 0x23bc08000), чтобы увидеть реальные обращения драйвера.

## 0.4.472
- Лог 0.4.471: ядро копирует код (инструкции ARM, напр. 0x14000091, mrs …) в блок 'pmp' на 0x23b500000 — это загрузка прошивки сопроцессора PMP в его SRAM (pc 0x82e81f0); трасса из 700 записей целиком ушла на это копирование, обращения к регистрам ASC (0x23bc00000/0x23bc08000) не попали.
- Диагностика: трасса ans/pmp теперь хранит не более 48 записей на блок (1200 всего), чтобы видеть управляющие регистры ASC.

## 0.4.473
- Прогон 0.4.472 впервые показал управляющую последовательность для PMP (все значения — из трассы, не придуманы): w 0x23b850000+0 = 0x23b500001 (адрес SRAM прошивки | 1: вероятно SART/окно) → r/w 0x23bc08000+0x10c = 1 → w 0x23bc00000+0x44 = 0x10 (в Asahi-документации CPU_CONTROL=0x44, бит RUN=0x10) — ядро запускает сопроцессор и после этого ждёт сообщение (HELLO) через почтовый ящик/прерывание. Код: pc 0x82e0580 / 0x82e0708 / 0x82e0f88.
- Диагностика: дамп кода 0x82e0400..0x82e1100 (RTBuddy start / mailbox), чтобы прочитать реальные смещения регистров ящика и обработчик сообщений из самого ядра.

## 0.4.474
- Из дампа 0.4.473: класс AppleA7IOP/ASC-обёртки читает регистр как [база+off] (0x82e0f84), 0x44 — CPU_CONTROL (бит 0x10 RUN), 0x810c/0x8108 — read-modify-write бит 0 (маски прерываний ящика, назначение ПРЕДПОЛОЖИТЕЛЬНО), +0xc04=0x10 и +0xc14=1 пишутся одной функцией (0x82e0f54). Смещения send/recv ящика в этом диапазоне не найдены.
- Диагностика: дамп соседних областей кода 0x82dfc00..0x82e0400 и 0x82e1100..0x82e1f00 — найти реальные регистры send/recv ящика и обработчик прерывания.

## 0.4.475
- Прочитано из кода ядра (дамп 0.4.474), не придумано: приём из ящика = чтение 16 байт с +0x8830 (I2A_RECV), отправка = запись 16 байт на +0x8800 (A2I_SEND) от базы ASC 0x23bc00000; регистр +0x8108: бит 17 («пусто» A2I), биты 18/19 (0xc0000) — ошибки; +0x810c: те же ошибки + поля 0xf00000, бит 0 включает/выключает (RMW). Цикл приёма читает слово [msg+12] & 0xf00000 и повторяется, пока оно равно 0x100000. Раскладка совпадает с Asahi ASC-ящиком. DT 'pmp': 3 окна (ASC 0x23bc00000/0x60000, 0x23b850000/0x200000, SRAM 0x23b500000/0x40000), 4 прерывания, iop-version, role.
- Диагностика: дамп RTBuddy-кода 0x82db300..0x82dc700 — формат HELLO/EPMAP и проверки версии протокола из самого ядра.

## 0.4.476
- Дамп 0.4.475 (0x82db300..0x82dc700) — это RTBuddy-код общих буферов/очередей; декодирования сообщений ep0 (тип/версия HELLO) в нём нет.
- Диагностика: список строк ядра, начинающихся с RTBuddy/RTKit/ASCWrap/AppleA7IOP (имена методов и сообщения об ошибках с «version»/«hello»/«endpoint»), чтобы найти обработчик ep0 в самом ядре, а не гадать про версию протокола.

## 0.4.477
- Строки ядра (0.4.476): в kernelcache лежат образы прошивок RTKit («RTKit_iOS-973.200.93.release», «RTKSTACKRTKit_iOS-973.200.76.release») — сопроцессорные ОС встроены в ядро; драйвер AppleA7IOP-110.200.10 имеет методы startCPU, ringDoorbell, setDoorbellAction. Формат HELLO по строкам не определился.
- Диагностика: значения DT interrupts / iop-version / role для узлов 'pmp' и 'ans'; список ВСЕХ номеров прерываний, которые ядро реально размаскировало в AIC (мы узнаем, какие из 4 прерываний ящика слушает драйвер) — без этого модель ящика гадала бы номер IRQ.

## 0.4.478
- Данные 0.4.477 (из DT и из поведения ядра): pmp interrupts = 0x12d 0x12c 0x12f 0x12e, iop-version = 1, role = «PMP»; ans interrupts = 0x16d 0x16c 0x16f 0x16e 0x173, iop-version 1, role «ANS2»; из этих номеров ядро размаскировало только третьи (0x12f у pmp). 
- Из кода ядра (0.4.474): регистр +0x810c имеет бит 0 enable, бит 17 empty, поле 0xf00000 = число записей; приём читает RECV1 (+0x8838) и по полю в его старшей половине решает, был ли элемент последним.
- НОВОЕ (MODELLED, ASSUMPTION, PLACEHOLDER): сторона IOP ящика ASC для PMP — НЕ настоящая прошивка (она не исполняется). При записи RUN (0x23bc00044 бит 4) в очередь ставится HELLO RTKit ep0 (min=max=1 — значение ЗАГЛУШКА, реальная версия протокола неизвестна), после HELLO_ACK — EPMAP {ep0}; все сообщения (I = модель→ядро, A = ядро→модель) печатаются в строке [asc-PMP]. Цель — увидеть, как ядро реально отвечает, и по ответам исправлять модель, а не гадать.

## 0.4.479
- РЕАЛЬНЫЙ ответ ядра на модель PMP (лог 0.4.478, [asc-PMP]): на наш HELLO (min=max=1) ядро ответило сообщением типа 2 с младшими 32 битами 0x0badffff — шаблон «версия не подходит» (в формате Asahi: max=0x0bad, min=0xffff). Это подтвердило, что формат сообщений ep0 совпадает с публичной документацией (тип в битах 59:52), но версия 1 для этого ядра недопустима. Наш EPMAP после такого ответа не должен был уходить — исправлено.
- Теперь HELLO объявляет ШИРОКИЙ диапазон min=1 max=0xffff (MODELLED): ядро само выберет свою версию, она будет видна в [asc-PMP]; значение не придумывается. Если ответ снова 0x0badffff — модель остановится (stage 3) и это тоже видно в логе.

## 0.4.480
- Прогон 0.4.479: на HELLO с диапазоном версий 1..0xffff ядро снова ответило 0x0badffff. Значит дело не в конкретном номере версии: либо наш формат HELLO (позиции полей/тип) не тот, который ждёт этот RTBuddy (iOS 12 старше документированного Asahi), либо ядро ждёт ещё что-то. Модель остановилась на stage 3 (как задумано). Выводов о причине пока нет.
- Диагностика: адреса инструкций MOVK w,#0xbad,lsl#16 в ядре — это место, где ядро формирует шаблон 0x0badffff; следующим прогоном дампнуть код вокруг и прочитать условие сам.

## 0.4.481
- Найдено: единственная инструкция MOVK #0xbad,lsl#16 в ядре — 0xfffffff007c2945c. Диагностика: дамп кода 0x7c29200..0x7c29700, чтобы прочитать условие, при котором ядро отвечает 0x0badffff.

## 0.4.482
- ОШИБКА 0.4.480: поиск MOVK брал только 32-битную форму (Wd); найденный 0x7c2945c оказался не тем (в дампе там обычная инструкция LDR) — вывод «единственное место» был неверен. Сообщение ядра 0x002000000badffff — 64-битная константа, значит нужна форма Xd. Теперь ищутся обе формы (маска 0x7fe00000).

## 0.4.483
- Правильное место: MOVK #0xbad,lsl#16 в 0xfffffff0082f4814 (рядом с кодом RTBuddy/A7IOP; второе совпадение 0x7c2945c — ложное). Диагностика: дамп кода 0x82f4580..0x82f4a00, чтобы прочитать условие ответа 0x0badffff.

## 0.4.484
- ПРОЧИТАНО ИЗ КОДА ЯДРА (дамп 0x82f4580..0x82f4a00, обработчик HELLO 0x82f46c0): ядро берёт из сообщения биты 31:16 (ubfx x21,x20,#16,#16), сравнивает с 10 (cmp w21,#10); при равенстве отвечает HELLO_ACK с версиями 2..10 (0x0020_0000_000a_0002 | флаги<<32), иначе строит ответ-ошибку 0x0020_0000_0bad_ffff (movk в 0x82f4814) и пишет сообщение об ошибке. Перед проверкой ещё есть условие на состояние объекта (поле +216 равно 3 или 0x8000).
- Исправление модели: HELLO теперь с max_ver = 10 (min = 10 — ЗАГЛУШКА, ядро её здесь не сравнивает). Раньше мы слали 1 и 0xffff — поэтому получали 0x0badffff. Это значение взято из кода ядра, не придумано.

## 0.4.485
- Результат 0.4.484 ([asc-PMP], реальные ответы ядра): HELLO (max=10) ПРИНЯТ — ядро ответило ACK 0x00200000000a0002 (версии 2..10), после нашего EPMAP {ep0} ответило 0x0080000000000001 (тип 8, битовая карта 1). Дальше ядро молчит: ApplePMGR всё ещё ждёт провайдера function-pmp_control, т.е. служба PMP ждёт следующего шага рукопожатия.
- Диагностика: дамп 0x82f4a00..0x82f5700 — диспетчер сообщений ep0 в ядре (какие типы и в каком порядке ждёт драйвер после EPMAP).

## 0.4.486
- Дамп 0x82f4a00.. (0.4.485): код — автомат питания IOP (состояния объекта 3 / 6 / 0x8000 / 0x8001, значения 0x20 и тип ответа 7); общего диспетчера ep0 в этом куске нет, поэтому дальше действую по наблюдению.
- Модель PMP (MODELLED, PLACEHOLDER): EPMAP теперь объявляет ep0 + стандартные системные эндпоинты 1..4 (crashlog, syslog, debug, ioreport по публичной документации Asahi; настоящий список прошивки PMP НЕИЗВЕСТЕН) — с одним ep0 ядро ответило DONE и замолчало. На запрос питания (тип 6) модель отвечает типом 7 с тем же состоянием (формат Asahi). Все сообщения по-прежнему в [asc-PMP].

## 0.4.487
- Реальные ответы ядра (0.4.486, [asc-PMP]): после EPMAP {ep0..4} ядро послало StartEP (тип 5) для эндпоинтов 1, 2, 3, 4 (0x0050_0001_0000_0002 …), одно сообщение на ep3 (msg0 0x0308000000000000), затем ответ на EPMAP (0x…0001) — и замолчало; PMGR всё ещё ждёт PMP.
- ЭКСПЕРИМЕНТ (MODELLED, PLACEHOLDER): после ответа ядра на EPMAP модель сообщает «питание IOP = ON (0x20)» самопроизвольным сообщением типа 7 (код питания в ядре 0x82f4b18 принимает состояние 0x20). Проверяем, ждёт ли ядро именно этого.

## 0.4.488
- Реальные ответы ядра (0.4.487): на «IOP power ON» (тип 7, 0x20) ядро прислало 0x00b0_0000_0000_0020 (тип 0xb = AP power state, 0x20). В стеках блокировок видны RTBuddy::_setPowerStateGuardEntryGated, _messageHandler ("invalid management message %llx received"), _incomingMsgAction — драйвер ждёт завершения смены питания, PMGR всё ещё ждёт PMP.
- Диагностика: дамп 0x82f3d00..0x82f4580 — _messageHandler/_incomingMsgAction (какие типы сообщений ep0 он принимает и что делает с каждым).

## 0.4.489
- Прочитано из кода ядра (дамп 0x82f3d00..): диспетчер входящих сообщений ep0 (_messageHandler, 0x82f3e64): тип = биты 55:52 (ubfx x22,x19,#52,#4), таблица переходов на типы 1 (HELLO → 0x82f46c0), 4 (→ 0x82f4a20), 7 (ack питания IOP → 0x82f4b18), 8 (EPMAP → ~0x82f4850), 0xb (→ 0x82f3f20: сохраняет младшие 32 бита в объект+232 и будит ожидающий поток — это то, на чём стоит _setPowerStateGuardEntryGated). Остальные типы → «invalid management message».
- Модель (MODELLED, ASSUMPTION): на AP_PWR_STATE (тип 0xb от ядра) модель отвечает тем же типом 0xb с тем же состоянием.

## 0.4.490
- ОШИБКА 0.4.487/0.4.489 (моя): в machine.c незакрытый комментарий `/* … PLACEHOLDER` проглатывал следующую строку, поэтому ответ на AP_PWR_STATE (0xb) из 0.4.489 фактически НЕ работал (в логе 0.4.489 после A:0xb не было ответа I:). Комментарий закрыт; новый тест (0.4.489) именно это и поймал. Выводы по 0.4.489 о том, что ядро «молчит» после 0xb, недействительны — проверяем заново.

## 0.4.491
- Лог 0.4.490 ([asc-PMP]): ответ на AP_PWR_STATE (I:0xb) теперь уходит (подтверждено), но ядро после него новых сообщений не шлёт. В стеках блокировок поток RTBuddy::start (82e8a54) ждёт ресурс через 0x8075e84 из кода около 0x82e94ec (рядом строки «IOStateReporter», «_createReporters»), а PMGR всё ещё ждёт PMP.
- Диагностика: дамп кода 0x82e9380..0x82e9560 — что именно ждёт RTBuddy::start (имя ресурса / службы).

## 0.4.492
- Дамп 0x82e9380.. (0.4.491): код 0x82e94a0 — RTBuddy ищет службу-прошивку: x0 = результат функции 0x82e4110(имя из объекта+192), затем ждёт её через 0x8075e48(x0, 0), результат проверяется OSDynamicCast, при NULL — REQUIRE (строка 275). Какая именно служба — из дампа не видно.
- Диагностика: словарь-аргумент вызова 0x8075e48 из 0x82e94e8 печатается в [wait-resN] (уже существующий снимок строк, 4 уровня указателей).
- Уточнение к 0.4.490: сообщение в плист-диагностике показывает, что персоналити RTBuddyFirmwareService есть только у AppleSPU (AOP/SPU/SPU2), PMP среди них НЕТ — прежнее утверждение «и PMP» было ошибочным.

## 0.4.493
- Лог 0.4.492: RTBuddy::start ждёт службу с IOProviderClass=«RTBuddyFirmwareService» и IOPropertyMatch. Персоналити этого класса есть только у AppleSPU (AOP/SPU/SPU2); код 0x82e94d0 ждёт её только при ненулевом признаке в объекте (+338), поэтому этот поток, скорее всего, относится к AOP/SPU, а НЕ к PMP — к PMP это ожидание не привязано (вывод по структуре кода, не проверено прогоном).
- Диагностика: список строк ядра ApplePMP*/AppleARMPMP*/PMP*/IOPMP* — найти драйвер, который публикует провайдера function-pmp_control (его ждёт ApplePMGR::start).

## 0.4.494
- Лог 0.4.493: в ядре НЕТ строк ApplePMP*/AppleARMPMP*/IOPMP* (только IOPMPowerSource и т.п.), значит драйвер PMP называется иначе.
- Диагностика: значение DT «compatible» (и device_type) узлов 'pmp' и 'ans' в виде слов — по нему найдём персоналити драйвера в plist ядра.
- ПОПРАВКА к записи 0.4.494: сам патч диагностики в первый раз НЕ применился (проверка assert упала, а я всё равно закоммитил и запустил прогон), поэтому прогон 0.4.494 без «compatible» недействителен. Патч применён в 0.4.495.

## 0.4.496
- Лог 0.4.495: DT-узлы 'pmp' и 'ans' имеют compatible = «iop,ascwrap-…» (обрезано выводом; в начале «iop,ascwrap-»), device_type «pmp» / «ans» — один и тот же класс ASCWrap, различаются только именем/ролью; драйвер, публикующий function-pmp_control, не найден по строкам «ApplePMP».
- Диагностика: контексты plist-персоналити со строками <string>pmp<, <string>PMP<, <string>pmp0<, <string>AppleARMPMGR (какие драйверы матчатся на узел pmp).

## 0.4.497
- НАЙДЕНО в plist ядра (лог 0.4.496): драйвер «AppleT8020PMPFirmware» (IOMatchCategory RTBuddyFirmwareService, IOPropertyMatch role=PMP, IOProviderClass=RTBuddy, com.apple.driver.AppleT8020PMPFirmware). Значит RTBuddy::start для PMP действительно ждёт этот драйвер (то самое ожидание RTBuddyFirmwareService из 0.4.492) — и PMP-ветка блокируется на том, что он не стартует/не публикуется. Исправляю свои выводы 0.4.493 («скорее AOP/SPU»): поток может быть и PMP.
- Диагностика: строки ядра AppleT8020* (код драйвера прошивки PMP).

## 0.4.498
- Лог 0.4.497: список строк AppleT8020* упёрся в лимит вывода (первые — AppleT8020PlatformErrorHandler), строки AppleT8020PMPFirmware не попали. Сузил фильтр до «AppleT8020PMP».

## 0.4.499
- Лог 0.4.498: у AppleT8020PMPFirmware в ядре есть только имя класса и plist-персоналити (и kext, вероятно, несёт образ прошивки PMP — раньше мы видели копирование кода в SRAM 0x23b500000, то есть прошивку ядро PMP-ю уже отдавало). Значит ожидание RTBuddyFirmwareService в потоке 0x82e94ec могло относиться к другому IOP (AOP/SPU), а PMP-RTBuddy сейчас не виден в стеках блокировок. Прежний вывод «PMP ждёт драйвер прошивки» (0.4.497) — НЕ доказан.
- Диагностика: слова DT function-pmp_control (узел pmgr), AAPL,phandle узлов pmp и pmgr — проверить, на какой узел реально указывает функция.

## 0.4.500
- ВАЖНО (лог 0.4.499, из DT): function-pmp_control у pmgr = <0x3b, 'PMPC'…> — провайдер имеет phandle 0x3b, а узел 'pmp' имеет phandle 0x3a. То есть PMGR ждёт НЕ узел 'pmp' (0x3a), а какой-то другой узел 0x3b (вероятно, дочерний/соседний, напр. PMP-контроллер). Мои выводы 0.4.466–0.4.498 о том, что провайдер — это узел 'pmp', могли быть неточны.
- Диагностика: имена узлов с phandle 0x3b и 0x3a.

## 0.4.501
- НАЙДЕНО (лог 0.4.500, DT): провайдер function-pmp_control — узел 'iop-pmp-nub' (phandle 0x3b, compatible «iop-nub,rtbuddy-v2», родитель 'pmp' phandle 0x3a). То есть PMGR ждёт, пока завершится start у RTBuddyV2 для PMP — именно он публикует IOFunctionParent0000003b. Значит блокировка в PMP-RTBuddy::start (поток 0x82e94ec, ожидание RTBuddyFirmwareService с role=…).
- Диагностика: нормальный рекурсивный разбор OSDictionary для вызова ожидания (ключ=значение, вложенные словари), чтобы увидеть, чьё role ждёт этот поток (PMP или другой IOP).

## 0.4.502
- ОТКАТ 0.4.501: рекурсивный разбор OSDictionary в диагностике вернул «?» — предположенная мной раскладка словаря была неверной; результата по роли IOP он не дал. Возвращён прежний универсальный снимок. Вывод «провайдер = iop-pmp-nub» из 0.4.500 остаётся в силе (прочитан из DT).

## 0.4.503
- Разбор кода (0.4.501 лог): RTBuddy::start (0x82e94a0..) при this+338==0 вызывает 0x82e4110(this+192) — построение словаря — и ждёт службу через 0x8075e48 с таймаутом 0; без результата — REQUIRE (строка 275). Диагностика: дамп кода 0x82e4110 и 0x82f1de8, чтобы увидеть какие ключи (role и т. д.) ждёт поток.

## 0.4.504
- 0x82e4110 = построитель словаря: serviceMatching(строка @0x75118c8) + ключ @0x75118df со значением this+192 (role). Диагностика: дамп данных 0x7511880–0x7511980 (читается как ASCII), чтобы узнать класс и ключ.

## 0.4.505
- РЕЗУЛЬТАТ 0.4.504 (строки ядра @0x75118c8): RTBuddy::start ждёт службу класса «RTBuddyFirmwareService» с ключом «Role». Для PMP она найдена (AppleT8020PMPFirmware: Found firmware, RTBuddy(PMP): Loaded firmware) — значит гипотеза «PMP ждёт прошивку» была неверна; единственный поток, всё ещё стоящий на 0x82e94ec, принадлежит другому IOP (роль не определена: кандидаты SMC/GFX/ANS2/SIO/AOP).
- PMGR всё ещё ждёт IOFunctionParent0000003b (iop-pmp-nub). Диагностика: поиск в plist ядра личностей, матчащихся на «iop-nub,rtbuddy-v2» / «iop-pmp-nub» / RTBuddyV2.

## 0.4.505 — результат
- plist: «iop-nub,rtbuddy-v2» → класс RTBuddyV2 (провайдер AppleA7IOPNub); «iop-nub,rtbuddy» → RTBuddy. AppleANS2NVMeController имеет IOProviderClass=RTBuddyV2 — т.е. и NVMe ждёт старта RTBuddyV2 узла ANS. Значит PMGR ждёт IOFunctionParent, публикуемый RTBuddyV2-нодой 'iop-pmp-nub'; почему её start не завершается — ещё не выяснено.

## 0.4.506
- Диагностика (ничего не меняет): в RTBuddy::start перед ожиданием службы (pc 0x82e94dc) снимается поле this+192 (role) → в логе [wait-resN] x0=<this>: «…строки role…». Цель — узнать, какой IOP (SMC/GFX/ANS2/SIO/AOP/PMP) стоит на 0x82e94ec.

## 0.4.506 — результат
- Перед ожиданием службы дошёл только один RTBuddy::start — role="SMC". Значит зависший на 0x82e94ec поток — это SMC (ждёт RTBuddyFirmwareService с Role=SMC), а не PMP. У PMP RTBuddy::start это ожидание не проходит вовсе (this+338 ≠ 0) и завершается. SMC — отдельная проблема (нужна служба прошивки SMC), к PMGR не относится.
- PMGR по-прежнему ждёт IOFunctionParent0000003b; потока RTBuddyV2 узла iop-pmp-nub в стеках нет — возможно, его start завершился без публикации, либо он вообще не создан.

## 0.4.507
- Диагностика (ничего не меняет): лог вызовов 0x814a7a0 (установщик function-свойства/IOFunctionParent): [fsetN] x0,x1,x2,lr, строка x1 — чтобы увидеть, кто и для чего вообще регистрирует IOFunctionParent и звали ли для 0x3b.

## 0.4.508
- ОТКАТ ВЫВОДА 0.4.507: 0x814a7a0 — не «установщик IOFunctionParent», а общий вызов (x1 — произвольные объекты, x2 — флаг); лог [fset] бесполезен, вывод по нему не делаю.
- Диагностика: поиск BL-вызовов имени-конструктора 0x814a870 и ожидателя 0x814a8f0 по всему коду ядра — найти, кто ещё работает с IOFunctionParent (в частности издателя).

## 0.4.509
- ПОПРАВКА к 0.4.508: поиск BL показал, что построитель имени 0x814a870 вызывается из САМОЙ 0x814a7a0 (0x814a804) и ещё из 0x814ab70. Значит 0x814a7a0 — как раз та функция, что работает с «IOFunctionParent%08X» (скорее всего публикует/ставит его), а мой откат в 0.4.508 был ошибкой. Вызовы [fset] из 0.4.507 — это реальные публикации других узлов (lr в 0x815198c, 0x8167…, 0x82d8388, 0x830f0d8, 0x8312a58, 0x8c138d8…); лог был обрезан 16 записями.
- Диагностика: дамп кода 0x814a780–0x814a8f0, 0x814ab40–0x814abc0 и 0x82d8340–0x82d83c0 (вызов из зоны A7IOP/RTBuddy), чтобы понять сигнатуру и кто публикует для PMP.

## 0.4.510
- Разбор кода: 0x814a7a0(x0=узел-провайдер, x1=служба) читает из x0 свойство (phandle), строит имя IOFunctionParent%08X и вызывает x1->setProperty(имя, …). 0x82d8358 (метод A7IOP/RTBuddy) вызывает её как (nub=arg, this=RTBuddy) после сохранения this[3312]. waitForFunctionParent ждёт службу с этим свойством.
- Диагностика: [fsetN] теперь только для вызовов из зоны A7IOP (lr 0x82c0000–0x8330000), s1 = role (this+192) — видно, для каких IOP публикация была, а для PMP — нет.

## 0.4.511
- Результат 0.4.510: из зоны A7IOP звали 0x814a7a0 один раз с lr 0x82d8388 (и 10 раз из 0x830f0d8, 1 раз 0x8312a58); role (this+192) пуст — смещение не то. Диагностика: теперь печатаются все строковые поля объекта this (смещения 0..0x400, off=строка), чтобы узнать, чьё это RTBuddy.

## 0.4.512
- Результат 0.4.511: x1 в 0x814a7a0 — не RTBuddy, а обычные службы (датчики температуры PMGR, ACC и т.п. по строкам объекта); для fset0 (lr 0x82d8388) строк нет, кто это — неясно. Зона 0x830f0d8 — драйвер датчиков PMGR, не A7IOP. Моё предположение «x1=RTBuddy» было неверным.
- Диагностика: [fnameN] — реальные имена, которые строит 0x814a870 (IOFunctionParent%08X), чтобы увидеть, для каких phandle свойство вообще публиковалось (искомое 0000003b).

## 0.4.513
- Результат 0.4.512: все [fname] = «?» — моя ошибка: хук стоял на 0x814a8bc, до инструкции `add x0, sp, #23`, так что x0 ещё не указывал на буфер. Хук перенесён на 0x814a8c0.

## 0.4.514
- Результат 0.4.513: имена IOFunctionParent%08X строятся (uppercase hex) для phandle 0x01, 0x13, 0x14, 0x16(×9), 0x1A, 0x1C(×9), 0x1D, 0x20, 0x38, 0x39, 0x4E, 0x9C, 0xC7–0xCE, 0xD0, 0xDA, 0xDD — но среди первых 40 записей нет 0x3B (искомый, PMP) и 0x3A. Список мог быть обрезан лимитом 40. Лимит поднят до 200.

## 0.4.515
- Результат 0.4.514: 0x3B строится один раз (лимит 200, записей 100) — но неясно, это публикация (из 0x814a804) или построение в ожидателе/другом месте (0x814ab70). К имени теперь дописывается адрес возврата (@808 = публикатор 0x814a7a0, @b74 = второй вызывающий), чтобы отличить публикацию от ожидания.

## 0.4.516
- Результат 0.4.515: суффиксы адреса возврата не появились — моя ошибка: имя IOFunctionParent%08X занимает 24 символа, а буфер 32 не оставлял места (проверка l+8<32). Буфер увеличен до 40.

## 0.4.517
- Результат 0.4.516: суффикс у всех «@4a8bc» — моя ошибка: в точке хука x30 уже перезаписан внутренним вызовом, это адрес внутри самой 0x814a870. Теперь читается сохранённый LR из кадра ([x29+8]).

## 0.4.518
- ВАЖНЫЙ результат 0.4.517: имя IOFunctionParent0000003B строится только из второго вызывающего (0x814ab74 — сторона ожидания), а публикатор (0x814a808) построил имена для 0x38, 0x39, 0x3C, 0x14, 0xCE и др. (44 публикации), но НЕ для 0x3B. То есть RTBuddyV2-узла iop-pmp-nub публикация не достигла, при этом для соседних узлов она работает.
- Диагностика: DT-узлы для phandle 0x37, 0x38, 0x39, 0x3C, 0x3D, 0x34 — чтобы понять, какие IOP-узлы уже опубликовали и чем PMP отличается.

## 0.4.519
- Результат 0.4.518: phandle, для которых публикатор отработал — это 0x37 aop-gpio, 0x38 nub-gpio, 0x39 smc-gpio, 0x3C dart-pmp, 0x34 dart-aop (GPIO и DART). Узлов IOP-нуба (rtbuddy-v2) среди опубликованных нет вообще: 0x3B = iop-pmp-nub; 0x3A = pmp (ascwrap-v2). Значит публикатор для IOP-нубов — отдельный путь (вызов из 0x82d8388 в зоне A7IOP был один раз), и он для PMP не сработал.
- Диагностика: строковые поля теперь читаются у x0 (узел), а не x1, для вызовов 0x814a7a0 из зоны A7IOP — чтобы узнать, чей это узел (вызов lr 0x82d8388).

## 0.4.520
- Результат 0.4.519: единственный вызов публикатора из зоны A7IOP (lr 0x82d8388) — для узла класса AppleARMCPU/IOPlatformDevice (CPU), не для IOP. То есть публикация IOFunctionParent для IOP-нубов (RTBuddyV2) не происходит ни разу; поток RTBuddy(PMP)::start в стеках отсутствует (завершился). Поправка к 0.4.510–0.4.511: lr 0x82d8388 — не RTBuddy.
- Диагностика: дамп кода RTBuddy::start (0x82e8800–0x82e8e00), чтобы увидеть, что идёт после _createReporters и где возможен возврат false / публикация.

## 0.4.521
- Результат 0.4.520: в RTBuddy::start (0x82e8a6c…) вызов _createReporters (0x82e94a0) стоит в самом конце (после него return true) — зависший SMC-поток блокируется именно в нём; LC_SYMTAB в ядре ОТСУТСТВУЕТ (моя прежняя запись о наличии символов была неверной).
- Диагностика: поиск личностей, у которых провайдер RTBuddyV2 / RTBuddy / RTBuddyService, и строки «PMPC» в plist ядра — кто реализует function PMPC для iop-pmp-nub и должен опубликовать IOFunctionParent.

## 0.4.522
- Результат 0.4.521: личности с провайдером RTBuddyV2 — только AppleANS2NVMeController и AppleANS2CGNVMeController; клиентов RTBuddy/RTBuddyService — AppleT8020PMPFirmware/AppleSPU*. Но в строках ядра есть класс ApplePMP (ApplePMP-98.200.34, «ApplePMPControlFunction», «ApplePMPUserClient») — вероятный издатель function PMPC / IOFunctionParent для iop-pmp-nub.
- Диагностика: поиск личности ApplePMP в plist (ключ, IOProviderClass, IONameMatch).

## 0.4.523 — ПЕРВОЕ ИЗМЕНЕНИЕ ПОВЕДЕНИЯ (EXPERIMENT / ASSUMPTION / PLACEHOLDER)
- Результат 0.4.522: личность ApplePMP матчится на IOProviderClass=RTBuddyEndpointService, IONameMatch «PMPEndpoint1» (второй класс — «PMPEndpoint2»). Это узлы-эндпоинты, которые RTBuddyV2 должен создать из EPMAP от IOP; пока такого узла нет, ApplePMP не стартует и IOFunctionParent для iop-pmp-nub не публикуется (аналогично драйвер NVMe ждёт «ANS2Endpoint1»).
- Эмулятор (MODELLED IOP-сторона, прошивка НЕ исполняется): EPMAP теперь двумя группами — группа 0 (ep0..4, без LAST) и после ответа ядра группа 1 (base 1, LAST, биты 0..1 = эндпоинты 0x20, 0x21). Номера эндпоинтов — ПРЕДПОЛОЖЕНИЕ (нумерация «Endpoint1/2» → 0x20/0x21 не подтверждена), реальный список PMP-прошивки неизвестен.
- Тест: ответ AP на EPMAP даёт вторую группу (base1, LAST, 0x3).

## 0.4.524
- РЕЗУЛЬТАТ эксперимента 0.4.523 (лог 1500M): kernel panic-путь REQUIRE «kRtbIopStatusWaitingForVersion == getIopStatus» (RTBuddyManagementEndpoint, область _handleHello 0x82f48e0) сработал с аргументом x1 = 0x88000100000003 — это МОЯ вторая EPMAP-группа (base 1, LAST, 0x3). Т.е. ядро при её обработке (в потоке mgmt-эндпоинта) упёрлось в REQUIRE состояния; ответ на вторую группу не пришёл. Одновременно появился новый поток, блокированный в RTBuddy::createEndpoint (0x82eb5c8) — значит ядро действительно начало создавать эндпоинт(ы) из моей карты. Причина панического REQUIRE пока не ясна (формат EPMAP group 1 может быть неверным, либо состояние сбрасывается).
- Диагностика: дамп кода 0x82f3e00–0x82f4000 и 0x82f4600–0x82f4c00 (диспетчер mgmt-эндпоинта и обработчики HELLO/EPMAP), чтобы прочитать условие REQUIRE и формат EPMAP.

## 0.4.525 — ИСПРАВЛЕНИЕ эксперимента 0.4.523 (моя ошибка)
- Прочитан обработчик EPMAP ядра (0x82f4858, вызывается диспетчером по типу 8): он принимает ОДНО сообщение, у которого младшие 52 бита — битовая карта эндпоинтов (бит 0 обязан быть 1, REQUIRE строка 0x222), статус IOP обязан быть 4 (REQUIRE строка 0x21a); флаг [obj+272] «карта уже получена»; на каждый установленный бит k≥1 вызывается включение эндпоинта k; затем ответ 0x0080_0000_0000_0000 | карта и статус 5. Понятия base/LAST (как в Asahi) здесь нет: мой бит 51 «LAST» из 0.4.478 на самом деле включал эндпоинт 51, а ВТОРОЕ сообщение EPMAP из 0.4.523 попало в REQUIRE(status==4) и вызвало panic. Строки «_handleHello» в [stks]/[pfr] — ложная привязка по ближайшим строкам.
- Эмулятор: теперь ОДНО сообщение EPMAP: ep0..4 и 32, 33 (0x20, 0x21 — ПРЕДПОЛОЖЕНИЕ для PMPEndpoint1/2, PLACEHOLDER; бит 51 больше не ставится). Тест обновлён.

## 0.4.526
- Результат 0.4.525: единственное EPMAP (ep0..4 + 0x20/0x21) принято без паники, ответ ядра тот же (0x80…01), но узлов PMPEndpoint1/2 по-прежнему нет, новых потоков нет; нумерация 0x20/0x21 не помогла (не подтверждена).
- Диагностика: дамп кода функции включения эндпоинта (0x82ead6c, вызывается на каждый бит EPMAP) и RTBuddy::createEndpoint (0x82eb400–0x82eb700), чтобы увидеть, как образуется имя «<Role>Endpoint<N>» и какие условия нужны для регистрации узла.

## 0.4.527
- Разбор 0x82ead6c (включение эндпоинта k по биту EPMAP): таблица переходов для k=0..4 (ep1 → создаётся объект, сохраняется в this+384; ep3, ep4 свои пути), k==31 — особый путь (имя собирается из role + строка 0x7516484), любой другой k (в т.ч. мои 32/33) → общий вызов 0x82ebee4(this,k). Значит эндпоинт 31 (0x1f), а не 0x20, вероятно «прикладной» для имени вида PMPEndpointN — ПРЕДПОЛОЖЕНИЕ, проверяю.
- Диагностика: дамп 0x82ebe00–0x82ec100 (общий путь), строк 0x7516400–0x7516700, начала 0x82ea980.

## 0.4.528
- Результат 0.4.527: функция 0x82ebee4 — выделение буфера (IOBufferMemoryDescriptor) для эндпоинта, а не создание именованного узла; поток на 0x82eb5c8 — ожидание статуса IOP==7 в цикле с таймаутом (не createEndpoint; прежняя подпись по строкам была ложной) — в 0.4.525 этот поток исчез, т.е. старт PMP завершается.
- Диагностика: поиск в ядре строк с «Endpoint» (формат имени «%sEndpoint%u»), чтобы найти код, создающий узлы RTBuddyEndpointService.

## 0.4.529
- Результат 0.4.528: в ядре есть строки RTBuddy::createEndpoint, RTBuddyEndpointService, шаблон «EndpointNN» (файл 0x51293f) — имя узла эндпоинта, по-видимому, строится из роли и номера по шаблону «EndpointNN».
- Диагностика: поиск ADRP+ADD ссылок на «EndpointNN» и «RTBuddyEndpointService» и дамп кода вокруг первой ссылки каждой строки (существующий искатель 0.4.126 переключён на эти строки).

## 0.4.530
- Результат 0.4.529: «EndpointNN» используется в коде по адресу 0x82edd60, «RTBuddyEndpointService» — в 0x82ee154 (по одной ссылке; сами дампы кода искатель не вывел).
- Диагностика: дамп кода 0x82edc80–0x82ee280 (создание узла-эндпоинта и его имени).

## 0.4.531 — изменение поведения (ASSUMPTION, выведено из кода ядра)
- Прочитан построитель имени узла-эндпоинта (0x82edd1c): строка «EndpointNN» → «<role>Endpoint<цифры>», где число = (k − 5): эндпоинт k=6 даёт «Endpoint1», k=7 — «Endpoint2» (для k≥15 двузначное). Плист: ApplePMP матчится на PMPEndpoint1 и PMPEndpoint2 → ожидаются эндпоинты 6 и 7. Мои 0x20/0x21 из 0.4.523/525 были неверным догадыванием (поправка).
- Эмулятор (MODELLED IOP-сторона): EPMAP = ep0..4 + ep6 + ep7 (0xdf). Реальный список эндпоинтов прошивки PMP НЕИЗВЕСТЕН — ASSUMPTION. Тест обновлён.

## 0.4.531 — РЕЗУЛЬТАТ (лог 1500M)
- «ApplePMP: started», «ApplePMPThermal: started» — узлы PMPEndpoint1/2 созданы, драйвер ApplePMP стартовал; поток ApplePMGR::start (0x8178864/waitForFunctionParent) в стеках ИСЧЕЗ — PMGR больше не блокирует платформу. Ядро отправило StartEP для ep6 и ep7. Это первое реальное продвижение за серию; прошивка PMP по-прежнему НЕ исполняется, IOP-сторона — моделируемая (список эндпоинтов — ASSUMPTION, выведено из кода ядра).
- Дальше: то же для ANS (NVMe ждёт ANS2Endpoint1 = эндпоинт 6).

## 0.4.532 — ANS: та же моделируемая сторона IOP (ASSUMPTION / PLACEHOLDER)
- ASC ANS: первое reg узла 'ans' = 0x277400000+0xc000 (как у aop/sio/smc/pmp), почтовый ящик +0x8000, IRQ 0x16f (третий в списке interrupts, как для pmp). EPMAP: ep0..4 и ep6 (0x5f) — NVMe ждёт узел «ANS2Endpoint1» (k=6). Прошивка ANS НЕ исполняется, NVMe/SART не моделируются. Лог: [asc-ANS2]. Новая функция a64m_asc_set_epmap (карта эндпоинтов на каждый ASC).

## 0.4.533
- Результат 0.4.532 (лог 1500M): модель ASC ANS зарегистрирована (блоки 0x277400000 / +0x8000 добавлены), но ядро НИ РАЗУ не обратилось к блокам ANS (run=0, [asc-ANS2] пуст): RTBuddy(ANS2)::start напечатан, а загрузка/запуск ANS-IOP не происходит; AppleANS2NVMeController::start доходит до ожидания ANS2Endpoint1 (0x8cbfc8c) без обращений к ASC. База ASC 0x277400000 при этом НЕ подтверждена (обращений нет). Кто запускает ANS-RTBuddy (firmware service отсутствует в plist для ANS), неизвестно.
- Диагностика: свойства DT узлов 'iop-ans-nub' и 'iop-pmp-nub' (pre-loaded, no-firmware-service, region-base и т.п.).

## 0.4.533 — результат
- DT: у 'iop-ans-nub' нет firmware-name, region-base = {0,0}, region-size = {0,0} (в отличие от PMP: firmware-name t8020pmp, region 0x23b500000/0x40000). Т.е. прошивку ANS должен был положить iBoot (в DT значения не заполнены) — в нашей загрузке этого нет; поэтому ANS-RTBuddy не запускается и обращений к ASC нет. Для продвижения нужна честная модель (область под прошивку/пред-загрузка) — следующий шаг, требует прочтения кода RTBuddy::start (флаги this+336/+284, region-*), чтобы не придумывать значений.

## 0.4.534
- Диагностика: дамп начала RTBuddy::start (0x82e8400–0x82e8800) и 0x82e93c0–0x82e94a0 — как используются region-base/size и флаги this+336/+284 (кто запускает IOP без службы прошивки, как у ANS).

## 0.4.535
- Находка (стеки 1500M, 0.4.532): ApplePMGR::start снова ЗАБЛОКИРОВАН, но уже в другом месте: поток 816be24 → 81788c4 → 89a8fe4 → 89a7798 → 809e6fc/809e514/809a2ec (ожидание), т.е. после PMP-ожидания PMGR::start идёт дальше и встаёт на следующем ожидании. Моё утверждение «PMGR больше не блокирует платформу» из 0.4.531 было НЕТОЧНЫМ — исчез только первый поток (8178864), PMGR::start не завершился. Также поток 3bf70000: 814a9f0/814a91c ← 8e97f94 ← 8219b38 — другая функция-родитель («function-button_%s»).
- Диагностика: дамп кода 0x89a7640–0x89a7800, 0x89a8f40–0x89a9000 и 0x8178880–0x81788d0.

## 0.4.535 — результат
- Новое ожидание PMGR::start (код 0x89a7740..): цикл «вызвать метод +248 объекта [this+512] с буфером this+136, пока значение < 2»; значение 3 → успешный возврат. Это ожидание ОТВЕТНОГО сообщения от PMP (приём по эндпоинту); AP-сторона при этом НИЧЕГО не отправляла на ep6/ep7 ([asc-PMP] без сообщений данных после StartEP). То есть теперь нужен реальный протокол PMP-прошивки (формат сообщений ApplePMP), который я не знаю и придумывать как «настоящий» не буду; варианты: (а) читать приёмный/передающий код ApplePMP в ядре и моделировать минимально, с пометками ASSUMPTION; (б) исполнять настоящую прошивку PMP (она лежит в kernelcache) — большая работа (отдельное ядро процессора).

## 0.4.536 — вариант (б), шаг 1: вытащить образ прошивки PMP
- Из логов: ядро копирует образ в окно SRAM PMP 0x23b500000; первое слово 0x14000091 (= AArch64 `b +0x244`), по +0x80 — инструкции `mrs` (0xd5385…): прошивка PMP, судя по всему, AArch64 (то есть наш интерпретатор потенциально подходит). Это ВЫВОД ПО ПЕРВЫМ СЛОВАМ, не проверенный факт.
- Эмулятор: только захват (чтение блока не меняется): байты записей в 0x23b500000 копируются в буфер; boot_cli печатает непустые 32-байтные строки [pmpfw] в boot.err для офлайн-разбора.

## 0.4.536 — РЕЗУЛЬТАТ (отчёт по варианту б, разведка)
- Образ прошивки PMP получен (0x3bfe0 байт, 37194 ненулевых слов из 61432); внутри строки «RTKit_iOS-973.200.93.release», «ApplePMP-98.200.34», «PMP started», «management», «ioreporting», имена шин питания (3V0_NAND, CPU_SRAM …). Образ НЕ закоммичен (это код Apple из пользовательского IPSW); при необходимости извлекается из boot.err строками [pmpfw].
- Архитектура: НАСТОЯЩИЙ AArch64 (первая команда `b +0x244`; дальше `mrs CurrentEL`, `eret` из EL3, VBAR_EL1/MAIR_EL1/TCR_EL1/TTBR0_EL1, включение MMU, проверка магического слова 0xcafe4b0b). То есть наш интерпретатор в принципе подходит как ядро IOP.
- Нужно будет смоделировать для IOP: ~21 страница устройств/адресов в литералах (0x200000000 — 26 ссылок, 0x23bc00000 — свой ASC, 0x23b000000, 0x23b800000, 0x23d000000, 0x2540be000, 0x26b000000, 0x20003b000, 0x204000000, 0x210000000, 0x211000000, DRAM 0x800000000 и окна рядом), системные регистры EL3/EL1 (в т.ч. PAN, обращения к ID_*), собственную карту памяти IOP (адрес SRAM с точки зрения IOP неизвестен).

## 0.4.537 — вариант (б): настоящая прошивка PMP исполняется на нашем ядре (локальная разведка)
Это НЕ загрузка и НЕ часть образа приложения: `tools/pmp_cli.c` — локальный стенд (сам образ прошивки в репозиторий не кладётся и в CI не попадает).
Что реально увидели, шаг за шагом (всё — наблюдения, не выдуманные значения):
- Прошивка PMP (RTKit_iOS-973.200.93) сначала «молча» засыпала в WFI на 2362-й инструкции. Причина — не ожидание, а паника: наше ядро отвечало исключением на чтение
  implementation-defined регистров Apple `S3_x_C15_Cx_y`. Включена уже существующая заглушка (ASSUMPTION: читаются как 0, запись игнорируется).
- Дальше падало на `AIDR_EL1`. В `arm64.c` регистры ID с op1=1, CRn=0 (CCSIDR/CLIDR/AIDR) теперь читаются как 0 (ASSUMPTION, не значения реального ядра).
- Затем прошивка пошла по своим устройствам и начала работать со СВОЕЙ стороной ASC (IOP-вид окна 0x23bc00000): +0x40, +0x80c, +0xa00..0xa8c, +0xc00, +0xc18, +0x4100.
  Её собственный распределитель VA (0xffffffffc2…/c3… через TTBR1) отображает устройства; для этого стенду потребовалось окно-ловушка на весь диапазон PA (логируется, читает 0).
- Итог на 50 тыс. инструкций: прошивка в цикле опрашивает `ASC+0x4108` (статус) и `+0x4810/+0x4818` (входящее сообщение, lo/hi) — ждёт сообщение от AP. Это первая настоящая карта IOP-стороны почтового ящика.
Честно: PMP-ответов AP ещё не получает, до связи IOP-ядра с моделью ящика на стороне AP работы остаётся. NAND-путь не сдвинулся; кернел-лог по-прежнему не заявляет загрузку.

## 0.4.538 — вариант (б): настоящая прошивка PMP САМА отправила HELLO
Снова только локальный стенд `tools/pmp_cli.c` (образ Apple не в репозитории). Наблюдения (не выдумка):
- Если статус входящего ящика (IOP-вид `ASC+0x4108`) отдаёт «пусто» (бит 17, как у AP-стороны), прошивка перестаёт читать мусор и пишет первое сообщение в `ASC+0x4820/0x4828`:
  `0x0010_0000_000a_0002` = тип 1 (HELLO), min ver 2, max ver 10. Совпадает с версией 10 в нашей сценарной модели — независимое подтверждение, что версия RTKit угадана верно.
- Регистр `ASC+0x81c` — «взять прерывание»: значение `(тип<<16)|линия`, тип 1 = обычная линия ≤127; обработчик квитирует записью в 0xa80+4*(линия/32) и вызывает функцию из таблицы.
  Прошивка размаскировала линии 101..104 (+0xa8c: 0x20/0x40/0x80/0x100) и линию 0. Линии 103/104 приводят к чтению I2A-статуса `+0x410c`; для 101/102 видимой реакции нет.
- Сценарий стенда (ASSUMPTION, не знания о реальном AP): ответ `HELLO_ACK` формата `0x0020_0000_000a_000a` и IRQ-линия при непустом ящике. Прошивка просыпается, берёт прерывание, но ACK не читает (`+0x4810` не опрашивается): пока не понято, какая линия — «входящее сообщение» и какие условия проверяет обработчик.
Честно: до рабочего обмена HELLO→ACK→EPMAP на настоящей прошивке не дошли; NAND-путь не изменён.

## 0.4.539 — вариант (б): карта прерываний IOP у настоящей прошивки PMP (стенд, без изменений в приложении)
Только `tools/pmp_cli.c` (локально; образ Apple не в репозитории). Наблюдения о прошивке, не выдумки, но интерпретации помечены:
- Двухуровневые прерывания: ASC+0x81c = «взять IRQ» (тип<<16|линия), линия 0 каскадирует во второй контроллер (регистр `0x23b102004`, тот же формат; ack — запись 0x80000000 в `+0x200c`).
  Таблица обработчиков второго уровня (хеш на 19 корзин) у прошивки в момент первого прерывания ПУСТА; на первом уровне зарегистрированы только линии 0 и 101–104.
  Строки «Mbox: overflow/underflow» показывают, что 101–104 — это ошибки FIFO ящика (а не «сообщение пришло»).
- Таймер CNTP у прошивки настроен через TVAL, а FIQ разрешается только короткими окнами (`DAIFClr #1` в 3 местах) — FIQ она пока не использует.
- Поток приёма (`ASC+0x4810/0x4818`) вызывается из потока, который опрашивает ящик единожды при старте и дальше ждёт событие; кто это событие выставляет — не найдено.
- Перебор линий 0..127 (уровень 1) и 0..1023 (уровень 2) с «непустым ящиком» не заставил прошивку прочитать HELLO_ACK: ни одна линия не принята как «сообщение». 
Вывод: к рабочему обмену на настоящей прошивке нужна более полная модель SoC вокруг неё (второй контроллер прерываний, таймеры, остальные периферии); это объём, сравнимый с отдельным эмулятором IOP. 
NAND-путь не изменён; журнал по-прежнему не заявляет загрузку.

## 0.4.540 — диагностика: кто вызывает коллбэк PMGR 0x89a7690 / ожидание 0x89a7740 (ничего не меняет)
- Вывод из прочитанного кода (0x89a7690): коллбэк `(this, событие, флаг)` меняет `this+136`: события 0/3 при флаге 1 → состояние 3 (и рассылка по 8 слотам `this+352`), событие 1 при флаге 1 из состояния 3 → состояние 2. Ожидание 0x89a7740 крутится, пока `this+136 < 2`. Это коллбэк «состояния питания/готовности», а не обязательно «сообщение PMP» — кто его вызывает, ещё неизвестно (не выдумываю).
- Диагностика в лог: BL-вызовы и 8-байтные слова данных (vtable/регистрации), чьи младшие 32 бита равны 0x89a7690/0x89a7740/0x89a7744, с соседними словами.

## 0.4.540 — результат и 0.4.541
- 0.4.540: сканирование ФАЙЛА ядра дало 0 ссылок — в файле указатели закодированы цепочечными fixup'ами (ядро применяет их само при старте), поэтому сравнение «младшие 32 бита» не работает. Моя ошибка в способе поиска.
- 0.4.541: тот же поиск (BL/B и слова данных, младшие 36 бит равны адресу, с соседними словами) выполняется по ПАМЯТИ гостя после прогона, когда fixup'ы применены (строки `[pmgrref]` в boot.err). Ничего не меняет.

## 0.4.541 — РЕЗУЛЬТАТ и 0.4.542
- Найдено (память гостя, 1500M): 0x89a7690 — слот VTABLE: слово данных по 0x90c25a8; соседние слова (0x89a6a2c 0x89a8cb0 0x89a7504 0x89a8704 0x89a82b4 0x89a88bc | 0x89a8344 0x89a85cc 0x89a89f0 0x89a9ee8 …) — виртуальные методы того же класса. Прямых BL/B на неё нет, значит вызывается только через vtable.
- Вывод по коду: ожидание PMGR — это `commandSleep`-подобный вызов на объекте `this+512` с событием `this+136`; коллбэк 0x89a7690 переводит состояние в 3/2 и будит. Его должен вызвать драйвер ApplePMP при получении сообщения от прошивки PMP (это ПРЕДПОЛОЖЕНИЕ, пока не доказано).
- 0.4.542: диагностика (ничего не меняет): начало vtable, смещение слота OFF и все загрузки указателя [reg,#OFF] в ядре (с пометкой «w2=1 рядом») — чтобы найти вызывающего.

## 0.4.542 — результат и 0.4.543
- 0.4.542: начало vtable по правилу «два нулевых слова» НЕ найдено за 600 слов (мой критерий начала vtable оказался неверен для этого ядра). Смещение слота не вычислено, поиск вызывающего не выполнен.
- 0.4.543: печатаю сырые слова (младшие 36 бит) от slot-96 до slot+40, чтобы начало vtable определить глазами.

## 0.4.543 — результат и 0.4.544
- 0.4.543: слова вокруг слота 0x90c25a8 — это длинный непрерывный массив указателей на функции из одного региона (0x88c…–0x89d…), без разделителей; начала vtable по ним определить нельзя (в массиве подряд идут и 0x89a7744 — само ожидание PMGR, и 0x89a7690). Смещение слота и вызывающий не найдены; метод поиска не сработал.
- 0.4.544: другой путь — смотреть, КТО печатает строки консоли с «PMP»/«Endpoint» (в т.ч. «ApplePMP: started»): при печати такой строки записывается цепочка кадров (fp) печатающего потока → адреса кода ApplePMP (start/обработчик). Печать `[pmpprint]` в boot.err. Ничего не меняет в поведении.

## 0.4.544 — РЕЗУЛЬТАТ и 0.4.545
- Находка (цепочки кадров при печати): «ApplePMP: started» и «ApplePMPThermal: starting…» печатаются из 0x89a6a10 (ApplePMP::start), «ApplePMPThermal: started» — из 0x89a9c84; общий родитель 0x805426c ← 0x806c4c8 ← 0x806c29c. Значит область 0x89a6xxx–0x89a9xxx — это драйвер ApplePMP, а вовсе не PMGR: ожидание 0x89a7744 и коллбэк 0x89a7690 — методы ApplePMP. ApplePMP::start (вызывается из PMGR::start через функцию function-pmp_control) блокируется на «состояние ≥ 2», которое выставляет его собственный коллбэк — и вызвать его должно сообщение/событие PMP (предположение).
- 0.4.545: дамп кода соседних методов vtable ApplePMP: 0x89a7500–0x89a7640, 0x89a7800–0x89a7900, 0x89a8cb0–0x89a8d80, 0x89a8704–0x89a8800, 0x89a82b4–0x89a8380, 0x89a6a00–0x89a6b10 (ничего не меняет).

## 0.4.545 — РЕЗУЛЬТАТ и 0.4.546
- Прочитано (код ApplePMP): 0x89a7744 — обобщённый синхронный вызов «pmp_control»: (this, код операции, &вход, &выход); сначала спит на событии `this+136`, пока состояние ≥ 2 (его ставит коллбэк 0x89a7690 при событиях RTBuddy 0/3 → состояние 3, 1 → состояние 2; после перехода будятся 8 слотов `this+352`), затем по коду операции (switch 0..13) шлёт сообщения методом `[this+512]` (смещение 224). 0x89a7570 — вход callPlatformFunction: сравнивает символ функции, для магии 'PMPC' (0x504d5043) создаёт клиента PMP-control.
- Вывод: коллбэк — уведомление RTBuddy о смене состояния (power/endpoint), а не «сообщение данных»; кто и когда его вызывает — ещё ищу.
- 0.4.546: те же поиски vtable, но с глубиной 6000 слов назад (большой vtable IOService-потомка мог не поместиться в 600 слов): начало, смещение слота, все загрузки [reg,#OFF] в ядре.

## 0.4.546 — РЕЗУЛЬТАТ и 0.4.547
- 0.4.546: «начало vtable» найдено на 0x90ba3e0, но смещение слота получилось 0x81c8 (>33 КБ) — это явно НЕ один vtable, а длинная область данных (всех vtable кекста подряд); критерий «два нуля» ошибочен. Вызывающий слот по смещению не искался.
- 0.4.547: берём vtable прямо у объекта: хук на вход 0x89a7744 (x0 = ApplePMP this), после прогона читаются `[this]` (vtable), `this+136` (состояние) и `this+512` (RTBuddy-объект и его vtable) — строка `[pmpthis]`; из неё смещение слота считается честно. Ничего не меняет.

## 0.4.547 — РЕЗУЛЬТАТ и 0.4.548
- Из лога: объект, на котором крутится ожидание, `this`=0xffffffe00086c6c0, его vtable = 0xfffffff007960e90 (НЕ рядом с 0x90c25a8), состояние `this+136` = 0 (то есть ни одного вызова коллбэка не было), RTBuddy-подобный объект `this+512` = 0xffffffe000a3f750 с vtable 0x77e96e8.
- Значит слот 0x90c25a8 — не vtable этого объекта (там лежат те же функции в другом массиве, вероятно vtable-база другого класса/таблица обработчиков).
- 0.4.548: ищу в настоящем vtable объекта (700 слов от 0x7960e90) слова 0x89a7690/0x89a7744, печатаю смещения; плюс слова самого объекта (0x48 qword) для понимания полей. Ничего не меняет.

## 0.4.548 — РЕЗУЛЬТАТ и 0.4.549
- 0.4.548: в первых 700 словах vtable объекта (0x7960e90) слов 0x89a7690/0x89a7744 НЕТ; объект: `this+0`=vtable, `this+512`=0xffffffe000a3f750 (подтверждено), `this+136`=0; в конце объекта маркер 0xdeadbeef (конец аллокации). То есть методы 0x89a7690/0x89a7744 вызываются не напрямую по vtable этого объекта, либо vtable устроен иначе (thunk'и «autibsp; b» в коде).
- 0.4.549: печатаю сами слова vtable (260 слов) — чтобы увидеть, какие адреса там лежат (кекст/ядро) и найти thunk'и, ведущие к 0x89a7690/0x89a7744. Ничего не меняет.

## 0.4.549 — РЕЗУЛЬТАТ и 0.4.550
- 0.4.549: vtable объекта ApplePMP (0x7960e90) содержит переопределения 0x89a63b0/63b4/63e4/644c и 0x89a7574 (= callPlatformFunction-вход 0x89a7570, +4 из-за pacibsp); 0x89a7690 и 0x89a7744 в нём НЕТ. Прочитан код методов-обёрток 0x89a7504/0x89a82d0: они вызывают метод +224 объекта `this+512` (RTBuddy) с символом функции, то есть ApplePMP сам обращается к RTBuddy через платформенные функции.
- Вывод: коллбэк 0x89a7690 хранится/вызывается не через vtable ApplePMP. Если его где-то зарегистрировали, указатель лежит в куче.
- 0.4.550: поиск по ВСЕЙ памяти гостя слов с младшими 36 битами 0x89a7690 / 0x89a7744 / 0x89a7574: адрес, соседние слова (`[cbscan]`). Ничего не меняет.

## 0.4.550 — РЕЗУЛЬТАТ и 0.4.551
- Результат сканирования всей памяти: коллбэк 0x89a7690 лежит РОВНО в одном месте — статическая таблица 0x90c25a8 (соседи: 0x89a8704 0x89a82b4 0x89a88bc | 0x89a8344 0x89a85cc 0x89a89f0); 0x89a7744 — в той же таблице (0x90c2570, соседи 0x89a4450 0x89a3918 0x89a8dec | 0x89a6a2c 0x89a8cb0 0x89a7504) + один раз в куче в кадре стека (this=0xffffffe00086c6c0, 0x89a7798). Регистрации коллбэка в куче НЕТ. 0x89a7574 — только в vtable объекта.
- Вывод: коллбэк вызывается через статическую таблицу методов (индекс), а не через регистрацию в рантайме.
- 0.4.551: ищу код, обращающийся к этой таблице (ADRP на страницу 0x90c2000 + ADD/LDR со смещением 0x540..0x600) — это и есть вызывающий (`[tblref]`). Ничего не меняет.

## 0.4.551 — РЕЗУЛЬТАТ и 0.4.552
- 0.4.551: к таблице 0x90c2540..0x90c25f8 обращаются 24 места по схеме `adrp; ldr x1,[x1,#слот]` (в т.ч. 0x89a7534 — слот 0x5a8, он держит 0x89a7690; 0x89a8fb8 — слот 0x570 = 0x89a7744). Значит это таблица ЗАГРУЖАЕМЫХ ЗНАЧЕНИЙ (GOT-подобная), а не таблица вызовов по индексу: обёртки ApplePMP (0x89a7504, 0x89a82d0…) берут значение из слота, передают его в 0x800a088 (this, значение, 0) и результат отдают в метод +224 объекта this+512 (callPlatformFunction-подобный). То есть 0x89a7690 используется как АРГУМЕНТ (идентификатор/символ), а не как коллбэк, который вызывают.
- Это меняет картину: возможно, я неверно прочитал 0x89a7690 как «коллбэк состояния»; нужно прочитать 0x800a088 и сами значения слотов.
- 0.4.552: дамп 0x800a088–0x800a180 и контекстов использования слотов (0x89a8fb0, 0x89a8c70, 0x89a8880, 0x89a3d40, 0x89a6700). Ничего не меняет.

## 0.4.552 — РЕЗУЛЬТАТ (картина прояснилась) и 0.4.553
- 0x800a088 — стандартный разрешитель указателя на метод-член (ptmf → адрес функции). Значит слоты 0x90c25xx — это pointers-to-member-functions ApplePMP. Метод +224 объекта `this+512` с аргументами (функция, arg0, arg1, 0, 0) — это IOCommandGate::runAction: `this+512` — командный затвор, а 0x89a7690 — действие (owner, событие, флаг), выполняемое в потоке workloop.
- ApplePMP::start (0x89a6700..): берёт `x21=[this+456]` и (1) через 0x89a6734 передаёт ему обработчик 0x89a6a2c (слот 0x578), (2) кладёт в `[x21+184]` разрешённый указатель на обёртку 0x89a7504 (слот 0x588), которая делает `gate->runAction(0x89a7690, событие, флаг)`. То есть `[this+456]` — объект стороны RTBuddy/конечной точки, а +184 — его «коллбэк уведомления»; вызвать его должен код RTBuddy при смене состояния/появлении сообщения.
- 0.4.553: поиск вызовов через указатель `[reg,#184]` (LDR x,[x,#184] + BLR/BLRAA в пределах 8 инструкций) в 0x8000000..0x89a0000 — это и есть кандидат на вызывающего. Ничего не меняет.

## 0.4.553 — РЕЗУЛЬТАТ и 0.4.554
- 0.4.553: вызовов через `[reg,#184]` восемь (0x81af294, 0x8294958, 0x8294e94, 0x82dccfc, 0x8535c0c, 0x865c490, 0x8683ff4, 0x86ac208); в области RTBuddy (0x82c0000–0x8330000) — ровно один: 0x82dccfc (загрузка 0x82dccdc). Вероятный вызывающий коллбэк ApplePMP.
- 0.4.554: дамп кода 0x82dcc00–0x82dcd40 и (для сравнения) 0x8294900–0x8294980, 0x8294e40–0x8294ec0. Ничего не меняет.

## 0.4.554 — РЕЗУЛЬТАТ (ложная находка, поправка) и 0.4.555
- 0.4.554: код 0x82dcca4 — не вызывающий: `[obj+184]` там только проверяется на ноль, вызов 0x82dccfc — вызов другого указателя (виртуальный метод аргумента). Мой вчерашний вывод «вероятный вызывающий» был неверен: сканер не учитывал, что указатели подписаны (PAC) и вызываются через BLRAAZ/BLRABZ (а не BLR/BLRAA).
- 0.4.555: тот же поиск, но с BLRAAZ/BLRABZ и по всему 0x8000000..0x8a00000, до 60 попаданий. Ничего не меняет.

## 0.4.555 — РЕЗУЛЬТАТ и 0.4.556
- 0.4.555: те же 8 попаданий, все с вызовом ровно через 8 инструкций — это перезагруженный регистр (виртуальный вызов), а не вызов через указатель `[+184]`: статический поиск вызывающего не даёт результата (подпись PAC и пересылка регистров).
- 0.4.556: смотрю на объект: слово с обёрткой 0x89a7504 в куче (`[obj184]`), объект = адрес−184; печатаю его первые слова (vtable → класс) и поля +168..+216. Это скажет, что за объект `[this+456]` и какой код его использует. Ничего не меняет.

## 0.4.556 — РЕЗУЛЬТАТ и 0.4.557
- 0.4.556: объект-держатель коллбэка (`[this+456]` ApplePMP) найден в куче: размер 216 байт (маркер 0xdeadbeef на +216), vtable = 0xfffffff00785e0b8, на +24 указатель 0xffffffe000d1fc00, на +48 0xffffffe000a3d720, на +184 — обёртка 0x89a7504, на +168/+176 нули. Вторая находка (статическая таблица PTMF-слотов 0x90c2588…) — это те же слоты.
- 0.4.557: печатаю 90 слов vtable 0x785e0b8, чтобы по адресам методов определить класс и найти метод, вызывающий указатель на +184. Ничего не меняет.

## 0.4.557 — РЕЗУЛЬТАТ и 0.4.558
- 0.4.557: методы vtable объекта-держателя лежат в 0x82ed4xx / 0x82ed8xx / 0x82ee0xx — это область класса конечной точки RTBuddy (рядом строитель имён «<role>Endpoint<N>» 0x82edd1c), то есть `[ApplePMP+456]` — сам сервис конечной точки (PMPEndpoint1), а ApplePMP кладёт ему на +184 свой коллбэк.
- Ошибка в моём сканере вызовов: у BLRAAZ/BLRABZ/BRAAZ/BRABZ поле Rm равно 31 (маска 0x…1f), а я требовал 0 — поэтому подписанные вызовы без модификатора не находились. Поэтому «0 вызывающих» в 0.4.553/0.4.555 — артефакт сканера.
- 0.4.558: сканер исправлен (BLRAAZ/BLRABZ/BRAAZ/BRABZ/BR), поиск вызовов через `ldr [reg,#184]` повторён. Ничего не меняет.

## 0.4.558 — РЕЗУЛЬТАТ и 0.4.559
- 0.4.558: исправленный сканер нашёл 29 мест `ldr [reg,#184]` + подписанный вызов; в классе конечной точки RTBuddy (0x82ed…) — ровно одно: загрузка 0x82edb54, вызов 0x82edb6c. Остальные — общий код ядра/других классов (0x805…, 0x811…, 0x81a…, 0x829…, 0x82a…, 0x82dc…, 0x853…, 0x865…, 0x868…, 0x86a…, 0x86d…, 0x89b…, 0x89c…, 0x89e…).
- 0.4.559: дамп кода 0x82edae0–0x82edc40 (кандидат на вызов коллбэка ApplePMP; ничего не меняет).

## 0.4.559 — РЕЗУЛЬТАТ (вызывающий коллбэка найден) и 0.4.560
- 0x82edb4c (метод класса конечной точки RTBuddy): `if ([obj+40] != 0 && [obj+184] != 0) [obj+184]([obj+80], x1, x2)` — вызывает зарегистрированный ApplePMP коллбэк (обёртка 0x89a7504 → `gate->runAction(0x89a7690, x1, x2)`) с ПЕРЕДАННЫМИ x1, x2 (событие и флаг от вызывающего). `[obj+40]` — байт-признак «активна/запущена». Рядом 0x82edb84 — такой же вызов через `[obj+192]` (только с `[obj+80]`, без доп. аргументов) — вероятно второй коллбэк.
- Значит условия для перехода ApplePMP в состояние ≥2: (1) `[obj+40] != 0` у сервиса конечной точки PMPEndpoint1 и (2) вызывающий 0x82edb4c должен позвать его с (событие 0 или 3, флаг 1) — кто зовёт 0x82edb4c, ещё не знаю.
- 0.4.560: ищу вызывающих 0x82edb4c и 0x82edb84 (BL/B в ядре и слова-указатели в RAM/vtable); дамп 0x82edc40–0x82edd20. Ничего не меняет.

## 0.4.560 — РЕЗУЛЬТАТ и 0.4.561
- 0.4.560: прямые вызывающие: 0x82edb4c (коллбэк `[obj+184]`) вызывается через BL из 0x82e9fe8; 0x82edb84 (коллбэк `[obj+192]`) — из 0x82ec9f4. Указателей на них в vtable/RAM нет. Оба места лежат в коде RTBuddy (рядом с RTBuddy::start 0x82e94xx).
- 0.4.561: дамп 0x82e9e80–0x82ea020 и 0x82ec960–0x82eca20 (контекст вызовов: какое событие RTBuddy порождает коллбэк). Ничего не меняет.

## 0.4.561 — РЕЗУЛЬТАТ и 0.4.562
- 0x82e9eb0 — метод RTBuddy `notify(событие=x1, флаг=x2)`: вызывает виртуальные методы (+880, +144; при нулевом результате — отладочная печать), через `[this+192]` синхронизируется (vfunc +160), затем, если `byte [this+290]` и счётчик `[this+416]` ≠ 0, обходит массив конечных точек `[this+400]` и для каждой зовёт 0x82edb4c (коллбэк `[+184]`: у ApplePMP — обёртка → runAction(0x89a7690, событие, флаг)).
- 0x82ec9c8 — родственный метод RTBuddy: обходит те же конечные точки и зовёт 0x82edb84 (коллбэк `[+192]`, без аргументов).
- 0.4.562: ищу вызывающих 0x82e9eb0 и 0x82ec9c8 (BL/B, слова-указатели) + дамп 0x82ea010–0x82ea140 и 0x82eca18–0x82ecb20 (соседи). Ничего не меняет.

## 0.4.562 — РЕЗУЛЬТАТ и 0.4.563
- 0.4.562: `notify` 0x82e9eb0 вызывается из 0x82e9c40 и 0x82e9cac (оба рядом, в одной функции/соседних), второй метод 0x82ec9c8 — из 0x82f1100. Указателей на эти методы в vtable/RAM нет (вызываются только напрямую).
- 0.4.563: дамп 0x82e9b80–0x82e9e80 (вызывающий notify) и 0x82f1040–0x82f1130 (вызывающий 0x82ec9c8): надо увидеть, какие события (значения x1, x2) и при каких условиях RTBuddy рассылает. Ничего не меняет.

## 0.4.563 — РЕЗУЛЬТАТ и 0.4.564
- Функция вокруг 0x82e9b80–0x82e9d4c (RTBuddy): вызывает через провайдера (`[this]`→vfunc +1344, затем метод +224 — platform-функция) три раза с символами из GOT-слотов 2144/2152/2136, читает результат в `[sp+44]` (событие) и `[sp+43]`/`[sp+42]` (логические флаги); если вызов вернул 0 — рассылает `notify([sp+44], [this+204])` (0x82e9c40) и затем `notify([this+204], [this+204])` (0x82e9cac); поля `this+200/204/216` задают состояние (индекс 1..7 → таблица +1184). Значит значения (0|3, 1), нужные ApplePMP, берутся из ответа платформенной функции и поля `this+204`.
- 0x82f10ac — обработчик сообщения (разбор 64-бит слова: поля бит 44.. — размеры буфера «<<12/20/21» — похоже на сообщение выделения буфера IOP); из него зовётся 0x82ec9c8 (+192-коллбэки).
- 0.4.564: дамп головы функции 0x82e9940–0x82e9b90 (откуда вход, какие значения), ничего не меняет.

## 0.4.564 — РЕЗУЛЬТАТ (ключевая находка) и 0.4.565
- 0x82e9ac0 — это `RTBuddy::setPowerState(unsigned long state=x1, IOService *device=x2)` (IOKit power management): кладёт `[sp+44]=state`, `[sp+43]=(this!=device)`, читает через платформу значения и рассылает `notify(state, [this+204])` конечным точкам; состояние >4 → возврат 5.
- Следовательно коллбэк ApplePMP (события 0/3 с флагом 1 → состояние 3 → снимается ожидание PMGR) приходит, когда PM-движок IOKit вызывает `setPowerState` у RTBuddy(PMP). То есть PMGR ждёт НЕ сообщения прошивки PMP, а смены состояния питания RTBuddy в дереве PM — прежняя гипотеза «нужно сообщение от прошивки» была ошибочной.
- 0.4.565: хук на вход 0x82e9ac0 (x0=RTBuddy, x1=состояние, lr; первые/последние 16 вызовов, строки `[waitN]`) — узнаем, вызывается ли setPowerState вообще и с какими состояниями. Ничего не меняет.

## 0.4.565 — РЕЗУЛЬТАТ и 0.4.566
- `RTBuddy::setPowerState` вызывался 5 раз, ВСЕ с состоянием 1: четыре раза из 0x807c2e8 (PM-движок IOKit: объекты 0xffffffe000d1f400, …f800, …fc00 — последний это RTBuddy(PMP) — и «iommu-mapper») и один раз из самого RTBuddy (0x82e9628, объект …fc00).
- Событие 1 у ApplePMP переводит 3→2, но из начального состояния 0 переходов нет; нужны события 0/3 с флагом 1, то есть вызов setPowerState(0 или 3). До состояния питания 3 (рабочего) дело не доходит.
- 0.4.566: дамп вызова 0x82e9628 (0x82e95a0–0x82e9720: что RTBuddy просит у PM-движка) и места вызова из PM-движка 0x807c2a0–0x807c300. Ничего не меняет.

## 0.4.566 — РЕЗУЛЬТАТ и 0.4.567
- Код 0x82e95a0–0x82e9640: RTBuddy вызывает platform-функцию (символ GOT-слот 2104; x2/x3 — аргументы вызывающего), и при успехе (w0==0) вызывает виртуальный метод `+1232(this, 1, 0)` — это запрос состояния питания 1 (IOService::changePowerStateTo-подобный). Именно он даёт тот самый `setPowerState(1)` из 0x82e9628.
- Функция 0x82e9a50: `если [this+294]==0 → this->+1232(3, 0)` (запрос рабочего состояния 3!), затем вызов `[+640](this, x1)`. Это единственное найденное место, запрашивающее состояние 3.
- 0.4.567: ищу вызывающих 0x82e9a50 (и 0x82e9640) — BL/B и указатели в RAM/vtable. Ничего не меняет.

## 0.4.567 — РЕЗУЛЬТАТ и 0.4.568
- 0.4.567: прямых вызовов 0x82e9a50 нет; указатель лежит ТОЛЬКО в двух vtable (0x785dcf8 — RTBuddy, 0x7861a78 — RTBuddyV2; между соседями 0x808c268 | 0x806e854, 0x82ec8dc, 0x82ec950). То есть это переопределённый виртуальный метод. 0x82e9640 — тоже виртуальный (0x785e010; соседи 0x82eba80 …); 0x82e96a8 — из таблицы 0x8030bf830 (PTMF/GOT-подобная).
- 0.4.568: считаю смещение слота как (адрес слота − vtable объекта RTBuddy(PMP) 0xffffffe000d1fc00) и ищу в ядре все загрузки виртуального указателя по этому смещению (LDRAA/LDR) — это вызывающие метода, запрашивающего состояние 3 (`[reqpow]`). Ничего не меняет.

## 0.4.568 — РЕЗУЛЬТАТ и 0.4.569
- Смещение слота метода 0x82e9a50 («запросить состояние питания 3») в vtable RTBuddyV2 = 0x270. Поиск загрузок по этому смещению дал 30 попаданий, все в общем коде 0x72e…–0x7bb…, при лимите 30 — результат шумный и неполный.
- 0.4.569: диагностика сужена до областей PM/RTBuddy/PMP (0x8060000–0x8330000, 0x89a0000–0x89c0000), лимит 80. Ничего не меняет.

## 0.4.569 — РЕЗУЛЬТАТ и 0.4.570
- В областях PM/RTBuddy/PMP — 33 загрузки указателя по смещению 0x270 (80 7f9dc, 807fda0, 8080360, 8080704, 8105de8…8107b50, 82fbd88, 82fd158, 8300dcc, 8300eb0 и др.). Смещение слишком общее (слот базового класса IOService), поэтому список сам по себе вызывающего не указывает.
- 0.4.570: статический дамп кода вокруг шести ближайших к RTBuddy мест (82fbd88, 82fd158, 8300dcc, 807f9dc, 807fda0, 8080360), чтобы прочитать, что за метод — только диагностика.

## 0.4.570 — РЕЗУЛЬТАТ и 0.4.571 (ОШИБКА МОЕГО СКАНЕРА)
- Дамп показал: «попадание» 82fbd88 — это `ldr x1,[x19,#624]`, то есть обычное поле структуры по смещению 0x270, а не вызов через vtable. Сканер 0.4.568/0.4.569 ошибочно считал простой LDR; все 33+30 «попаданий» — шум. Ошибка моя.
- 0.4.571: считаются только загрузки LDRAA с пре-индексом (настоящий vtable-вызов), фильтр по областям снят. Только диагностика.

## 0.4.571 — РЕЗУЛЬТАТ и 0.4.572
- Ни одной загрузки LDRAA со смещением 0x270 в образе. Компилятор здесь вызывает методы по схеме `add x9,x8,#off; ldr x8,[x8,#off]; blraa x8,x9` (как в PM-движке 0x807c2d0), а не LDRAA.
- 0.4.572: сканер ищет именно эту пару ADD+LDR с одним и тем же регистром и смещением 0x270. Только диагностика.

## 0.4.572 — РЕЗУЛЬТАТ и 0.4.573
- Найдено 21 место с парой ADD+LDR по смещению 0x270 (8105f8c, 8106b88, 81073cc, 810757c, 82103cc, 821733c, 84fd148, … 8eb8164). Метод общий для многих классов, значит это либо базовый метод IOService, либо метод RTBuddy-семейства.
- 0.4.573: дамп кода вокруг первых шести мест (8105f8c, 8106b88, 81073cc, 810757c, 82103cc, 821733c), чтобы прочитать, что вызывается. Только диагностика.

## 0.4.573 — РЕЗУЛЬТАТ и 0.4.574
- Места вызова по смещению 0x270 используют РАЗНЫЕ PAC-дискриминаторы (6734 в 8105f94/8106b90, 26820 в 82103d4) — значит смещение общее для разных классов, а не один метод. Статический поиск вызывающих по смещению бесполезен; признаю тупик этого пути.
- 0.4.574: хук перенесён на сам вход 0x82e9a50 (вызывался ли он вообще, с каким lr) и дамп кода 0x82e9a00–0x82e9ac0 и 0x82e9560–0x82e9660. Только диагностика.

## 0.4.574 — РЕЗУЛЬТАТ и 0.4.575
- Метод 0x82e9a50 за весь прогон НЕ вызывался (`[wait-calls] 0`). Его код: если байт [this+294]==0 — вызывает +1232(this, 3, 0) (тот же метод, через который PM-движок зовёт setPowerState), затем вызывает метод суперкласса (дискриминатор 6734) с аргументом x1. Это однопараметровый «базовый» метод IOService (похоже на системное событие/останов), а не нужная нам ветка запуска. Прежнюю гипотезу «надо добиться вызова 0x82e9a50» ОТКАЗЫВАЮСЬ считать подтверждённой.
- 0.4.575: дамп всего тела RTBuddy::setPowerState 0x82e9ac0–0x82e9eb0 и RTBuddy::notify 0x82e9eb0–0x82e9fb0, чтобы точно прочитать, как состояние превращается в событие 0/1/3. Только диагностика.

## 0.4.575 — РЕЗУЛЬТАТ и 0.4.576
- Прочитан RTBuddy::setPowerState (0x82e9ac0): `state=x1`; платформенная функция (символ 2144) с &state; затем `notify(this, state, [this+204])` (0x82e9eb0), платформенная функция (2152), снова notify с `w1=[this+204], x2=[this+204]` и платформенная функция (2136). Таблица перевода состояний на 0x82e9e6c.
- Значит у ApplePMP событие определяется состоянием и полем [this+204] RTBuddy; на ApplePMP событие 1 из состояния 0 ничего не делает, нужно событие 0/3 с флагом 1 — то есть либо [this+204] должно быть 1 (и событие 0/3), либо состояния.
- 0.4.576: печать полей объекта RTBuddy(PMP) +184..+232, +288..+312, +400..+424 после прогона (особенно +200/+204/+216/+220). Только диагностика.

## 0.4.576 — РЕЗУЛЬТАТ и 0.4.577
- Объект RTBuddy(PMP) в конце прогона: [+200]=0x504d5000, [+204]=1, [+216]=7, [+220]=0 (байт), [+222]=1, [+290]=0, [+291]=1, массив конечных точек +400 (8 штук). Главное: **байт [this+290]=0**, а RTBuddy::notify (0x82e9eb0) при нём сразу выходит, не вызывая обработчики конечных точек — поэтому ApplePMP не получает событий.
- 0.4.577: ищу код, который пишет/читает байт по смещению 290/291 (STRB/LDRB) в области 0x8060000–0x8a00000, чтобы найти, кто должен выставить [this+290]. Только диагностика.

## 0.4.577 — РЕЗУЛЬТАТ и 0.4.578
- Запись байта +290 в области RTBuddy: 0x82e8368 и 0x82e8d88; байта +291: 0x82e8cb0 (остальные места 847…/84b…/87e…/880… — другие классы). Читают +290 в самом RTBuddy: 0x82e9f58 (notify), 0x82ea0b8.
- 0.4.578: дамп кода 0x82e8300–0x82e83a0 и 0x82e8c60–0x82e8e00 — прочитать условия, при которых [this+290] выставляется. Только диагностика.

## 0.4.578 — РЕЗУЛЬТАТ и 0.4.579
- Расшифровано: `RTBuddy::start` читает свойства device-tree-узла провайдера ([this+176]) через виртуальный метод +272 и превращает каждое в булев флаг: [+289], **[+290]** (строка по смещению 999 от базы 0x7517000), [+291] (746), [+292] (760), [+293] (1023), [+294] (1011). Сброс в 0 делает инициализатор 0x82e8368.
- Байт [+290]=0 значит: у узла iop-pmp-nub в нашем дереве устройств нет нужного свойства. То есть это не поведение прошивки, а **содержимое device tree** — честный путь: выяснить имя свойства и есть ли оно в реальном дереве iPhone.
- 0.4.579: дамп строк 0x75172c0–0x7517440 (имена свойств). Только диагностика.

## 0.4.579 — РЕЗУЛЬТАТ и 0.4.580 (моя арифметическая ошибка)
- Дамп 0.4.579 показал не имена свойств, а текст сообщений RTBuddyIOReportingEndpoint.cpp: я неверно посчитал базу ADRP (3538 страниц = 0xDD2, а не 0xDD1) — база строк на самом деле 0x7516000, не 0x7517000. Ошибка моя, вывода из того дампа нет.
- 0.4.580: дамп 0x7516280–0x7516480 (строки по смещениям 746…1050 от верной базы). Только диагностика.
