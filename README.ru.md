<p align="center"><img src="docs/banner.svg" alt="XNUron" width="100%"></p>

<p align="center">
  <a href="README.md">English</a> · <b>Русский</b>
</p>

<p align="center">
  <img alt="Язык: C" src="https://img.shields.io/badge/ядро-C-6750A4?style=for-the-badge&labelColor=EADDFF">
  <img alt="Оболочка: Android" src="https://img.shields.io/badge/оболочка-Android%20%2F%20Java-7D5260?style=for-the-badge&labelColor=FFD8E4">
  <img alt="Архитектура: AArch64" src="https://img.shields.io/badge/arch-AArch64%20EL0%2FEL1-4F378B?style=for-the-badge&labelColor=E8DEF8">
  <img alt="Статус: экспериментальный" src="https://img.shields.io/badge/статус-экспериментальный-B3261E?style=for-the-badge&labelColor=F9DEDC">
</p>

# XNUron

**Интерпретатор AArch64 на C в оболочке Android-приложения, который честно пытается загрузить ядро настоящего iPhone из публичной прошивки (IPSW).**

Цель: iPhone XS Max (D331AP, `iPhone11,4`, A12, iOS 12.x): `kernelcache.release.iphone11`, насколько получится, до `/sbin/launchd`.
Это исследовательский проект. Это **не** рабочий эмулятор iOS, и он никогда не будет притворяться таковым.

---

## ✦ Где мы сейчас

| Этап | Состояние |
|---|---|
| Ядро AArch64 (EL0/EL1, MMU с 16-КиБ страницами, исключения, IRQ, таймер, FP/SIMD, эксклюзивные доступы) | ✅ работает, покрыто тестами на хосте. Pointer authentication *смоделирован*: подписи не вычисляются, `AUT*` всегда успешен |
| Загрузка настоящего kernelcache из IPSW (IMG4 → LZFSE → Mach-O) | ✅ работает |
| Загрузка XNU: `kernel_bootstrap`, подбор драйверов IOKit, десятки драйверов Apple стартуют | ✅ доходит до цикла простоя планировщика |
| Корневая ФС (RAM-диск) смонтирована в `bsd_init` | ✅ `hfs: mounted …CustomerRamDisk` |
| Запуск `/sbin/launchd` | ❌ **ещё нет** |

Часть потоков IOKit всё ещё ждёт platform-функции (`function-*`), провайдеры которых (GPIO, SMC, PMU, порт аксессуаров) требуют настоящего железа, которого XNUron не моделирует. Сейчас выясняю, какая из них действительно мешает `launchd`.
Подробная история по версиям с разбором каждой остановки: [`docs/CHANGELOG.md`](docs/CHANGELOG.md).

## ✦ Правила проекта

- **Никаких подделанных загрузок.** Если ядро остановилось, журнал говорит где и почему.
- **Никаких выдуманных значений прошивки под видом настоящих.** Всё смоделированное помечено `MODELLED`, каждое допущение `ASSUMPTION`, каждое выдуманное значение `PLACEHOLDER`.
- **Никакого обхода шифрования и подписей.** Криптографические проверки (SecureRoot и подобные) не подделываются; если ядру нужны данные, которых у нас нет (ключи, настоящий ответ SEP), журнал так и говорит.
- **Тест на каждое изменение.**
- Прошивка Apple **никогда** не хранится в репозитории. Headless-workflow скачивает публичный IPSW с серверов Apple во время запуска.

## ✦ Как попробовать

### Android-приложение
Каждый push в `main` собирает APK и публикует его в **[Releases](../../releases)** (тег `v<версия>`, pre-release).
Откройте, укажите IPSW и читайте журнал.

### Тесты на хосте
Нужны `gcc`, `clang`, `ld.lld`, `llvm-objcopy`, `python3` (Java по желанию).

```sh
tests/run_all.sh        # при успехе печатает ALL TEST GROUPS PASSED
```

### Headless-попытка загрузки (GitHub Actions)
**Actions → Boot attempt (headless) → Run workflow.** Собирает те же C-исходники, что и APK, скачивает публичный IPSW, запускает попытку загрузки и публикует `boot.log` / `boot.err` в релиз `boot-log`.

## ✦ Карта репозитория

| Путь | Что там |
|---|---|
| `app/src/main/cpp` | интерпретатор AArch64, виртуальная машина, загрузчики, попытка загрузки IPSW (C) |
| `app/src/main/java` | Android-оболочка (Java) |
| `tests/` | тесты на хосте (`tests/run_all.sh`) |
| `tools/` | headless-запуск (`boot_cli.c`), помощники для IPSW, скрипты сборки гостя |
| `guest/` | небольшое собственное тестовое гостевое ядро |
| `docs/` | [`FEASIBILITY.md`](docs/FEASIBILITY.md) (исходный честный анализ), [`CHANGELOG.md`](docs/CHANGELOG.md) (полная история) |

## ✦ Правовая сторона

В XNUron нет прошивок Apple, ключей и подписывающего материала, и он не обходит никакую защиту. Прошивку вы приносите сами; на неё действует лицензия Apple на iOS. Ничто здесь не является юридической консультацией.
`app/src/main/cpp/lzfse` — открытый декодер LZFSE от Apple под собственной BSD-лицензией (см. `lzfse/LICENSE`).
Файла лицензии самого проекта пока нет; пока его нет, все права принадлежат автору.

> Раньше проект назывался *A64 Lab*.
