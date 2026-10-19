#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_HOR_RES_MAX          480
#define LV_VER_RES_MAX          480
#define LV_COLOR_DEPTH          16

#define LV_MEM_CUSTOM           1
#if LV_MEM_CUSTOM
#define LV_MEM_CUSTOM_ALLOC     malloc
#define LV_MEM_CUSTOM_FREE      free
#define LV_MEM_CUSTOM_REALLOC   realloc
#endif

#define LV_USE_FS_STDIO         1
#if LV_USE_FS_STDIO
#define LV_FS_STDIO_LETTER      'S'
#define LV_FS_STDIO_PATH        ""
#define LV_FS_STDIO_CACHE_SIZE  0
#endif

#define LV_USE_PNG              1

#define LV_USE_ANIMATION        1
#define LV_FONT_DEFAULT         &lv_font_montserrat_14

#endif /* LV_CONF_H */
