<p align="center"><img src="docs/banner.svg" alt="XNUron" width="100%"></p>

<p align="center">
  <b>English</b> · <a href="README.ru.md">Русский</a>
</p>

<p align="center">
  <img alt="Language: C" src="https://img.shields.io/badge/core-C-6750A4?style=for-the-badge&labelColor=EADDFF">
  <img alt="Target: Android" src="https://img.shields.io/badge/shell-Android%20%2F%20Java-7D5260?style=for-the-badge&labelColor=FFD8E4">
  <img alt="Arch: AArch64" src="https://img.shields.io/badge/arch-AArch64%20EL0%2FEL1-4F378B?style=for-the-badge&labelColor=E8DEF8">
  <img alt="Status: experimental" src="https://img.shields.io/badge/status-experimental-B3261E?style=for-the-badge&labelColor=F9DEDC">
</p>

# XNUron

**An AArch64 interpreter written in C, wrapped in an Android app, that tries – honestly – to boot the kernel of a real iPhone from its public IPSW.**

The target is the iPhone XS Max (D331AP, `iPhone11,4`, A12, iOS 12.x): `kernelcache.release.iphone11`, as far as `/sbin/launchd`.
This is a research project. It is **not** an iOS emulator you can use, and it never will pretend to be one.

---

## ✦ Where it stands

| Stage | State |
|---|---|
| AArch64 core (EL0/EL1, MMU with 16 KiB pages, exceptions, IRQ, timer, FP/SIMD, exclusives) | ✅ working, covered by host tests. Pointer authentication is *modelled*: signatures are not computed, `AUT*` always succeeds |
| Loading the real kernelcache from the IPSW (IMG4 → LZFSE → Mach-O) | ✅ working |
| XNU boot: `kernel_bootstrap`, IOKit matching, dozens of Apple drivers start | ✅ reaches the scheduler idle loop |
| Root filesystem (RAM disk) mounted by `bsd_init` | ✅ `hfs: mounted …CustomerRamDisk` |
| `/sbin/launchd` started | ❌ **not yet** |

Several IOKit threads still wait for platform functions (`function-*`) whose providers (GPIO, SMC, PMU, accessory port) need real hardware that XNUron does not model. Finding out which of these actually blocks `launchd` is the current work.
The detailed, version-by-version story, with every stop analysed, is in [`docs/CHANGELOG.md`](docs/CHANGELOG.md) (Russian).

## ✦ Supported firmware

**Only one build family is supported: iOS 12.x for the iPhone XS Max (`iPhone11,4`).** That is the only target the project is developed and tested against (a specific `kernelcache.release.iphone11`).
Other iOS 12 builds and other A12 iPhones may start or may stop early with different errors; this is untested and not promised.
Anything that is not iOS 12 is **not supported**. If you feed XNUron such an IPSW, the log says so in red (`[UNSUPPORTED] iOS 13.x … is NOT supported`) and the app shows a notice, but the attempt still runs, so you can see how far it gets.

## ✦ The ground rules

- **No faked boots.** If the kernel stops, the log says where and why.
- **No invented firmware values presented as real.** Everything modelled is labelled `MODELLED`, every assumption `ASSUMPTION`, every invented value `PLACEHOLDER`.
- **No decryption or signature bypass.** SecureRoot-style cryptographic checks are not faked; if the kernel needs data we do not have (keys, a real SEP answer), the log says so.
- **A test for every change.**
- Apple firmware is **never** stored in this repository. The headless workflow downloads the public IPSW from Apple's servers at run time.

## ✦ Try it

### Android app
Every push to `main` builds an APK and publishes it under **[Releases](../../releases)** (tag `v<version>`, pre-release).
Open it, point it at an IPSW, and read the log.

### Host tests
Needs `gcc`, `clang`, `ld.lld`, `llvm-objcopy`, `python3` (Java optional).

```sh
tests/run_all.sh        # prints ALL TEST GROUPS PASSED on success
```

### Headless boot attempt (GitHub Actions)
**Actions → Boot attempt (headless) → Run workflow.** It builds the same C sources as the APK, fetches the public IPSW, runs the boot attempt and publishes `boot.log` / `boot.err` to the `boot-log` release.

## ✦ Repository map

| Path | What lives there |
|---|---|
| `app/src/main/cpp` | AArch64 interpreter, virtual machine, loaders, the IPSW boot attempt (C) |
| `app/src/main/java` | Android shell (Java) |
| `tests/` | Host tests (`tests/run_all.sh`) |
| `tools/` | Headless runner (`boot_cli.c`), IPSW helpers, guest build scripts |
| `guest/` | Small self-made test guest kernel |
| `docs/` | [`FEASIBILITY.md`](docs/FEASIBILITY.md) (the original honest analysis), [`CHANGELOG.md`](docs/CHANGELOG.md) (full history) |

## ✦ Legal

XNUron contains no Apple firmware, keys or signing material and does not circumvent any protection. You supply the firmware; Apple's licence terms for iOS apply to it. Nothing here is legal advice.
`app/src/main/cpp/lzfse` is Apple's open-source LZFSE decoder under its own BSD licence (see `lzfse/LICENSE`).
This repository does not yet carry a project licence file; until one is added, all rights are reserved by the author.

> XNUron was formerly called *A64 Lab*.
