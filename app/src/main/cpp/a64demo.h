#pragma once
#include "arm64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Loads the demo program + data into guest RAM (base 0, >= 64 KiB) and sets pc/sp. */
void a64_demo_load(A64 *c);
#ifdef __cplusplus
}
#endif
