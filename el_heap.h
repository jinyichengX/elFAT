#ifndef EL_HEAP_H
#define EL_HEAP_H

#include <stdint.h>
#include <stdlib.h>

/* 以N字节对齐 */
#define tBYTE_ALIGNMENT 4

/* 全局静态堆大小 */
#define tMEM_SIZETOALLOC (1024)

/* 支持内存碎片整理 */
#define tMEM_DFGMENTATION 0

extern void * tAllocHeapforeach(unsigned int sizeToAlloc);
extern void tFreeHeapforeach(void* tObj);
extern unsigned char CalcMemUsgRtLikely(void *mem);
#endif