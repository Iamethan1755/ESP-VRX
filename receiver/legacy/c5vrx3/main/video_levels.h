#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
/* Host raster tests retain the production default unless explicitly overridden. */
#ifndef CONFIG_C5VRX_DAC_BITS
#define CONFIG_C5VRX_DAC_BITS 6
#endif

#if CONFIG_C5VRX_DAC_BITS == 8
#define C5VRX_DAC_CODE(code6) (((unsigned)(code6) * 255u + 31u) / 63u)
#define C5VRX_DAC_MODE_NAME "8BIT@40"
#elif CONFIG_C5VRX_DAC_BITS == 6
#define C5VRX_DAC_CODE(code6) (code6)
#define C5VRX_DAC_MODE_NAME "6BIT@40"
#else
#error "Supported physical DAC resolutions are 6 and 8 bits"
#endif
