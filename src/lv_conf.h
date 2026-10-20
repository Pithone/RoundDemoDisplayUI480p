#pragma once

#ifndef LV_CONF_H
#define LV_CONF_H

#ifndef __ASSEMBLER__
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* lvglMemoryPoolAlloc(size_t size);
#ifdef __cplusplus
}
#endif
#endif

#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_MEM_SIZE (5U * 1024U * 1024U)
#define LV_MEM_POOL_ALLOC(size) lvglMemoryPoolAlloc(size)
#define LV_DRAW_BUF_ALIGN 4
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_CACHE_DEF_SIZE (1U * 1024U * 1024U)
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1

#define LV_USE_LODEPNG 1
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'S'
#define LV_FS_STDIO_PATH "/sdcard/"
#define LV_FS_STDIO_CACHE_SIZE 0

#define LV_USE_FLEX 1
#define LV_USE_GRID 1
#define LV_USE_IMAGE 1
#define LV_USE_SLIDER 1
#define LV_USE_DROPDOWN 1
#define LV_USE_BUTTON 1
#define LV_USE_LABEL 1
#define LV_USE_ARC 1

#endif
