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
