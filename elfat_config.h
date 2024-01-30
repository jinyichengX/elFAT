#ifndef ELFAT_CONFIG_H
#define ELFAT_CONFIG_H

/* 超时检测 */
#define YC_TIMEOUT_SWITCH   0
#define YC_TIMESTAMP_ON 0

/* 文件重定向至内存 */
#define YC_FILE2MEM 1

/* 支持字符编码功能 */
#define YC_FAT_ENCODE 1

/* 开启调试功能 */
#define PRINT_DEBUG_ON 0

/* 定义文件最大大小 */
#define FILE_MAX_SIZE (4*1024*1024*1024-1)

/* FAT备份扇区有效 */
#define FAT2_ENABLE 0

/* 格式化磁盘 */
#define YC_FAT_MKFS 1
#if YC_FAT_MKFS
#define SFD 0
#define FDISK 1
#define OTHER_STRATEGY ...
#define FROMAT_STRATEGY_SET SFD/* 格式化策略选择 */
#endif

/* 文件信息缓存 */
/* 适用于文件频繁打开关闭 */
#define FILE_CACHE 1
#if FILE_CACHE
#define MAX_FILES_CACHE 5 /* 最大缓存 */
#endif

/* 可同时打开的最大文件数量 */
#define MAX_OPEN_FILES 5

/* 文件裁剪 */
#define YC_FAT_CROP 1

/* 使用长文件名 */
#define YC_FAT_LFN 0

/* 使用回收站 */
#define YC_FAT_RECYCLE 1

/* 定义每个磁盘驱动号最大长度 */
#define YC_FAT_PERDDN_MAXSZIE 10
#endif
