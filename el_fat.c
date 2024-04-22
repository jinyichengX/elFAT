/******************************************************************************************
* @file         : el_fat.c
* @Description  : A FAT32 file system.
* @autor        : jinyicheng
* @emil:        : 2907487307@qq.com
* @version      : 1.0
* @date         : 2022/9/11    
 * 修改日期        版本号     修改人	      修改内容
 * ----------------------------------------------------
 * 2022/09/11	    V1.0.0	    jinyicheng	        创建，实现文件系统基本结构，目录和文件访问读写等基本功能
 * 2023/02/17       V1.1.0      jinyicheng          添加文件系统挂载，卸载功能
 * 2023/03/05       V1.1.1      jinyicheng          添加Unicode字符集编码，GBK<-->Utf-8互解码
 * 2023/05/28       V1.1.2      jinyicheng          添加文件重定向功能
 * 2023/08/16       V1.2.3      jinyicheng          自定义内存堆替换malloc
 * 2024             V1.2        jinyicheng          添加DMA请求
 * 2024             V1.3        jinyicheng          等OS完成后添加的功能,初步思路是将此文件系统运行在内核态，使用SVC系统调用实现多线程访问
 * ****************************************************************************************/

/*！！！！！重要说明！！！！！*/
/* 在使用这个库时，请在这个c文件中搜索关键词"重要说明"教你怎么用这个库 */
/* 因为这个库非常简单，没有什么复杂的功能，所以没有额外的什么说明文件  */
/* 用到的技术点：文件系统结构，链表，位图，二分查找，内存管理，字符编码解码，重定向输出 */
/* （不涉及复杂算法和结构，技术点简单，大佬嘴下留情 *^_^*） */
#include "el_fat.h"
#include "elfat_config.h"
#include "el_heap.h"/* 这里替换成el_heap.h */
#include "el_list.h"
#include <string.h>
#include <stdbool.h>
/* 移植报错就关掉这个宏 */
#define YC_FAT_DEBUG 0

#if YC_FAT_DEBUG
#include <stdio.h>
#include "bsp_uart.h"
extern en_result_t bsp_usart_Init(Usartx_Init_str_Typedef *Usartx_Init_str);
#endif

/* 数据类型重定义 */
typedef unsigned char   J_UINT8;
typedef unsigned short  J_UINT16;
typedef unsigned int    J_UINT32;
typedef unsigned long   J_UINT64;
typedef const unsigned char    J_ROM_UINT8;
typedef const unsigned short   J_ROM_UINT16;
typedef const unsigned int     J_ROM_UINT32;
typedef const unsigned long    J_ROM_UINT64;

char endian;/* 硬件相关，大小端模式 */

/* 字符编码类型重定义 */
typedef unsigned int unic32;
typedef enum
{
    NOTFOUND = -1,
    FOUND = 0,
}SeekFile; 

#define USE_FAT32 1
#if USE_FAT32
/* 定义簇链单位大小 */
#define FAT_SIZE 4
/* 定义文件最大大小 */
#define FILE_MAX_SIZE (4ULL * 1024 * 1024 * 1024 - 1)
#define FATSIZE(S_DISK,S_CLUS) FAT_SIZE*S_DISK/S_CLUS /* FAT1/FAT2 表大小(字节) */
#endif

/* 定义首目录簇开始扇区 */
//static unsigned int FirstDirSector = 0;/* 此变量可删除？不清楚，在这个文件系统编写早期定义的，已经忘了干什么用的了 */

/* 固定参数begin，不能改 */
/* 定义FAT32扇区大小，固定为512Byte */
#define PER_SECSIZE 512
/* 定义根目录簇簇号 */
#define ROOT_CLUS   2
/* 定义每个FAT文件系统对象最大分区数 */
#define PER_FATOBJ_PARTITION_NUM 4
/* 固定参数end，不能改 */
#if YC_FAT_MKFS
#define DBR1_SEC_OFF 0
#define DBR2_SEC_OFF 0
#define DBR3_SEC_OFF 0
#define DBR4_SEC_OFF 0

#define NSECPERTRACK 63 /* 每道扇区数 */
#define NHEADDER 255    /* 磁头数 */
#define NSECPERCYLINDER (NSECPERTRACK*NHEADDER)/* 柱面总扇区数 */
#define FS_RSVDSEC_NUM 32
#define GET_RCMD_FATSZ(SEC_NUM,SEC_PER_CLU) (SEC_NUM+2*SEC_PER_CLU-FS_RSVDSEC_NUM)/((PER_SECSIZE/FAT_SIZE)*SEC_PER_CLU+ROOT_CLUS)
#endif

/* 初始化参数 */
struct FatInitArgs {
    unsigned int DBR_Ss;          /* DBR起始扇区 */
    unsigned int FirstDirSector;  /* 根目录起始扇区 */
    unsigned int FAT1Sec;         /* FAT1表起始扇区 */
   
    unsigned int FreeClusNum;     /* 剩余空簇数目 */
    unsigned int NextFreeClu;     /* 下一个空簇 */
};
struct FatInitArgs FatInitArgs_a[4];

/* 定义分区属性，16Byte */
typedef struct DiskPartitionTable
{
    J_UINT8 isActive;           /* 引导指示符 */
    J_UINT8 partStartHead;      /* 分区开始磁头 */
    J_UINT16 startCylSec;       /* 开始柱面与扇区 */
    J_UINT8 partType;           /* 分区类型，定义分区类型
                                    本系统的所有分区类型为FAT32 */
    J_UINT8 partEndHead;        /* 分区结束磁头 */
    J_UINT16 endCylSect;        /* 结束柱面和扇区 */
    J_UINT32 partStartSec;      /* 分区开始扇区 */
    J_UINT32 perPartSecNum;     /* 分区所占总扇区数 */
}DPT_t,*DPT;

/* 定义主引导头，占用FAT32的第一个扇区 */
typedef struct MasterBootRecord
{
    J_UINT8 BootLoader[446];    /* 引导程序 */
    DPT_t dpt[4];               /* 分区表，FAT32至多支持4个分区
                                    即一个物理磁盘至多可以划分成四个逻辑磁盘 */
    J_UINT8 fixEndMark[2];      /* 固定标记码0x55,0xAA */
}MBR_t,*MBR;

/* BIOS PARAMETER BLOCK DATA STRUCTURE */
typedef struct DOSBootRecord{
  J_UINT8 jmpBoot[3];   /* 跳转指令 */
  J_UINT8 OEMName[8];   /* OEM厂商 */

  /* BOIS param block */
  J_UINT16 bytsPerSec;  /* 每扇区大小，通常为512 */
  J_UINT8 secPerClus;   /* 每簇扇区数 */
  J_UINT16 rsvdSecCnt;  /* 保留扇区数（DBR->FAT1） */
  J_UINT8 numFATs;      /* FAT表数，通常为2 */

  J_UINT16 rootEntCnt;  /* 根目录项数，FAT32为0 */
  J_UINT16 totSec16;    /* 小扇区数，FAT32为0 */
  J_UINT8 media;        /* 媒体描述符，主要用于FAT16 */
  J_UINT16 FATSz16;     /* FAT扇区数（针对FAT12/16） */
  J_UINT16 secPerTrk;   /* 每道扇区数 */
  J_UINT16 numHeads;    /* 磁头数 */
  J_UINT32 hiddSec;     /* 隐藏扇区数 */

  J_UINT32 totSec32;    /* 总扇区数 */
  J_UINT32 FATSz32;     /* 每个FAT的扇区数，FAT32专用 */

  J_UINT16 extFlags; /* 扩展标志 */
  J_UINT16 FSVer; /* 文件系统版本 */
  J_UINT32 rootClusNum; /* 根目录簇号 */
  J_UINT16 FSInfo; /* 通常为1 */
  J_UINT16 bkBootSec; /* 通常为6 */
  J_UINT8 reserved[12]; /* 总为0 */
  J_UINT8 drvNum; /* 物理驱动器号 */
  J_UINT8 reserved1;  /* 总为0 */
  J_UINT8 exBootSig; /* 扩展引导标签 */
  J_UINT32 vollD; /* 分区序号 */
  J_UINT8 volLab[11]; /* 卷标 */
  J_UINT8 filSysType[8]; /* 系统ID */
}DBR_t;

typedef struct FileSystemInfoStruct
{
    J_UINT8 Head[4];//"RRaA"
    J_UINT8 Resv1[480];
    J_UINT8 Sign[4]; //"rrAa"
    J_UINT8 Free_nClus[4]; //剩余空簇数量
    J_UINT8 Next_Free_Clus[4];//下一个剩余空簇
    J_UINT8 Resv2[14];
    J_UINT8 FixTail[2];//"55 AA"
}FSINFO_t;

typedef enum
{
    RD_WR = 0X00, /* 读写 */
    RD_ONLY = 0X01, /* 只读 */
    HIDDEN = 0X02,/* 隐藏 */
    SYSTEM = 0X40,/* 系统 */
    VOLUME = 0X08, /* 卷标 */
    TP_DIR = 0X10,/* 子目录 */
    ARCHIVE = 0X20, /* 存档 */
}FLA;

/* 目录项 */
typedef struct FileDirectoryItem
{
    J_UINT8 fileName[8];    /* 文件名 */
    J_UINT8 extName[3];        /* 扩展文件名 */

    union{
        J_UINT8 attribute;    /* 属性 */
        FLA attribute1;         /* 属性 */
    };
    
    J_UINT8 UpLower;        /* 文件名大小写标志位 */
    J_UINT8 crtTime_10ms; /* 创建时间10ms */
    union{
        J_UINT16 crtTime;J_UINT8 crtTime_a[2];       /* 创建时间 */
    };
    union{
        J_UINT16 crtDate;J_UINT8 crtDate_a[2];       /* 创建日期 */
    };
    union{
        J_UINT16 acsDate;J_UINT8 acsDate_a[2];       /* 最后访问日期 */
    };
    J_UINT8 startClusUper[2];      /* 起始簇高16位 */
    J_UINT8 modTime[2];         /* 最近修改时间 */
    J_UINT8 modDate[2];         /* 最近修改日期 */
    J_UINT8 startClusLower[2];  /* 起始簇低16位 */
    J_UINT8 fileSize[4];        /* 文件大小（字节） */
}FDI_t;
#if YC_FAT_LFN
typedef struct 
{
    J_UINT8 scode;
    J_UINT8 lfn1[10];/* 长文件名1-5字符(a.b.c.d.e.) */
    J_UINT8 sign;/* 长文件名目录项标志，固定为0x0f */
    J_UINT8 rsvd;/* 保留 */
    J_UINT8 crc;/* 校验值 */
    J_UINT8 lfn1[12];/* 长文件名6-11字符*/
    J_UINT8 rsvd2[2];/* 保留 */
    J_UINT8 lfn3[4];/* 长文件名12-13字符*/
}lFDI_t;
#endif
/* 一个扇区内的FDI */
typedef struct FDIInOneSec
{
    FDI_t fdi[PER_SECSIZE/sizeof(FDI_t)];
}FDIs_t;

/* 全局MBR DBR */
MBR_t g_mbr;DBR_t g_dbr[4];
static unsigned char g_dbr_n = 0;

typedef enum{
    FILE_CLOSE,
    FILE_OPEN
}FILE_STATE;

/* 文件句柄 */
typedef struct fileHandler
{
     /* 文件描述符 */
    unsigned int fd;
	/* 文件首簇 */
    unsigned int FirstClu;
	
    /* 数据锚定（读） */
    unsigned int CurClus_R;   /* 当前簇 */
#if !YC_FAT_MULT_SEC_READ
    short CurOffSec;    /* 当前簇内偏移扇区 */
	unsigned short CurOffByte;  /* 当前扇区/簇内偏移字节 */
#else
    unsigned short EndCluSizeRead; /* 当前簇已读字节数 */
#endif
    /* 剩余大小（读） */
    unsigned int left_sz;
	
	/* 文件大小 */
    unsigned int fl_sz;
    /* 文件状态 */
    FILE_STATE file_state;
#if YC_FILE2MEM
    int (*load2memory)(struct fileHandler *,void *mem_base,int);/* 文件是否加载至内存操作 */
    int (*Writeback)(struct fileHandler *,void *mem_base,int);/* 文件回写 */
#endif
    struct list_head WRCluChainList;/* 簇链缓冲头节点，不携带实际数据 */
    struct list_head RDCluChainList;/* 簇链缓冲头节点，不携带实际数据 */
    /* 文件末簇 */
    unsigned int EndClu;
	
	/* 数据锚定（写） */
	unsigned int CurClus;/* 暂时没用 */
    /* 文件末簇未写大小 */
    unsigned int EndCluLeftSize;
	
    /* 文件FDI所在扇区及其偏移 */
    struct fdi_info {
        unsigned int fdi_sec;
        unsigned short fdi_off;
    }fdi_info_t;
}FILE1;

typedef struct WRCluChainBuffer
{
    struct list_head WRCluChainNode;
    unsigned int w_s_clu;    /* 头簇 */
    unsigned int w_e_clu;    /* 尾簇 */
}w_buffer_t; 

typedef struct FAT_Table
{
    J_UINT8 fat[FAT_SIZE];
}FAT32_t;

typedef struct FAT_TableSector
{
    FAT32_t fat_sec[PER_SECSIZE/FAT_SIZE];//128
}FAT32_Sec_t;

static unsigned int open_sem = MAX_OPEN_FILES;
#if MAX_OPEN_FILES
typedef struct {
    unsigned int ffdi_sec;
    unsigned short ffdi_off;
    char is_open;
}Match_Info_t;
static Match_Info_t matchInfo[MAX_OPEN_FILES] = {0};
#endif

#if FILE_CACHE
/* 文件缓存数据结构 */
typedef struct {
    /* 文件FDI所在扇区及其偏移 */
    struct fdi_info_c {
        unsigned int fdi_sec_c;
        unsigned short fdi_off_c;
    }fdi_info_c_t;
    unsigned int tail_cluster;
} FileCacheEntry;
FileCacheEntry file_cache[MAX_FILES_CACHE];/* 文件缓存区 */
#endif

typedef enum CreatFDItype
{
    FDIT_DIR = 0,
    FDIT_FILE = 1
}FDIT_t;

#if YC_FAT_MKFS
enum PERCLUSZ {
    _DEFAULT = 0,
    _512B = 512,
    _1K = 1024,
    _2K = 2048,
    _4K = 4096,
    _8k = 8192,
    _16K = 16384,
    _32K = 65536,
    _64K = 131072,
};
#endif

/* 定义磁盘-文件系统挂载状态 */
enum FATStatus {
    MOUNT_SUCCESS,     	/* 挂载成功 */
	FAULTY_DISK,		/* 坏盘 */
    ACCESSIBLE,        	/* 可访问 */
    FILE_NOT_FOUND,    	/* 文件不存在 */
    PERMISSION_DENIED, 	/* 权限被拒绝 */
};
/* 重要说明 */
/* 底层实现集只需要底层向JYCFAT库提供数据buffer，起始扇区，扇区数三个参数即可，具体看挂载函数就知道怎么用了 */
/* 拿一个Nor Flash举例
    假设一个flash有下面几个参数
    .Pagesize = 256,			//页大小256B
	.Sectorsize = 4096, 		//扇区大小4K
	.MiniBlocksize = 32*1024,	//小型块32K
	.Blocksize = 64*1024, 		//正常块64K
	.Capacity = 256*1024*1024,	//容量256M
	.Sectornum = 1024*64,		//1024*64个扇区 
假设你已经实现了下面的功能：
NorFlashWritePage(uint32_t PageIndex, uint8_t* dBuffer, uint32_t PageNum)//页写(三个参数分别是，1-第几个页 2-数据地址 3-要写入的页数)
NorFlashWriteSector(uint32_t SectorIndex, uint8_t* dBuffer, uint32_t SectorNum)//扇区写（三个参数分别是，1-第几个扇区 2-数据地址 3-要写入的扇区数）
NorFlashWriteBlock(uint32_t BlockIndex, uint8_t* dBuffer, uint32_t BlockNum)//块写（三个参数分别是，1-第几个块 2-数据地址 3-要写入的块数）
NorFlashRead(uint32_t ReadAddr, uint8_t* pBuffer, uint32_t NumByteToRead)//读（三个参数分别是1-读开始地址（以字节为单位） 2-读入地址 3-需要读取的字节数）
既然你的flash页大小是256B，物理扇区大小是4K，而这个库又是以512B的扇区为单位的，所以出现参数不对应问题，那么有下面两种解决办法：
方法1：把NorFlashWritePage函数封装成以512字节一个扇区的写函数
    比如：
    NorFlashWrite(uint8_t* dBuffer,uint32_t Index, uint32_t Num){
        NorFlashWritePage(Index/2, uint8_t* dBuffer, 2*Num);
    }
    这样就变成以512B为一单位写了，再把NorFlashWrite封装在ioopr_t结构体中传入给挂载函数
方法2：直接把NorFlashWriteSector传入给挂载函数，但是！会造成4K-512B=3.5K每扇区的空间浪费
同理读操作也是一样你需要把ReadAddr转化为扇区（512B为单位的扇区，不是flash中的物理扇区4K）索引
*/
typedef struct {
	char (*DeviceOpr_WR)(void * buffer,unsigned int SecIndex,unsigned int SecNum);//写设备
	char (*DeviceOpr_RD)(void * buffer,unsigned int SecIndex,unsigned int SecNum);//读设备
	char (*DeviceOpr_CLR)(unsigned int SecIndex,unsigned int SecNum);//擦除设备
}ioopr_t;

/* 文件系统实例 */
typedef struct FilesystemOperations{
    struct list_head mountNode;
	unsigned char ddn[PER_FATOBJ_PARTITION_NUM][YC_FAT_PERDDN_MAXSZIE];//驱动号
	enum FATStatus hay;//挂载号
	int (*fsOpr_Init)(struct FilesystemOperations* );//文件系统初始化
	FILE1 * (*fileOpr_Open)(FILE1 *, unsigned char *);
	int (*fileOpr_Create)(unsigned char *);//创建文件
	int (*DirOpr_Create)(unsigned char *);//创建目录
	int (*DirOpr_Enter)(unsigned char *);//进入目录
	int (*fileOpr_Del)(unsigned char *);//删除文件
	int (*fileOpr_Close)(FILE1 *);//关闭文件
	int (*fileOpr_Write)(FILE1*,unsigned char *,unsigned int);//写文件
	unsigned int (*DirOpr_GetCWD)(void);//获取当前工作目录
	int (*fileOpr_Rename)(unsigned char *,unsigned char *);//重命名文件
	int (*DirOpr_Rename)(unsigned char *,unsigned char *);//重命名目录
	int (*fileOpr_Crop)(FILE1 *,unsigned int);/* 从尾部裁剪文件 */
#if YC_FAT_MKFS
	int (*diskOpr_Format)(unsigned int ,enum PERCLUSZ);/* 格式化 */
#endif
	/* 底层实现集 */
	ioopr_t ioopr;
}ycfat_t;
/* 大小端检测 */
union e_cont
{
	unsigned short val;
	unsigned char ch[2];
};

/* 日期(年-月-日)掩码 */
#define DATE_YY_BASE 1980
#define MASK_DATE_YY 0xFE00
#define MASK_DATE_MM 0x01E0
#define MASK_DATE_DD 0x001F
#define MASK_TIME_HOUR 0xF800
#define MASK_TIME_MIN  0x07E0
#define MASK_TIME_SEC  0x001F
#define MAKS_HID_RECYCLE {0xEB,0x90,0xEB,0x90,0x6A}
/* 创建文件目录项的时间和日期 */
#define MAKETIME(T) 
#define MAKEDATE(T)
/* 由簇号锚定其FAT表所在扇区 */
#define CLU_TO_FATSEC(clu) ((clu * FAT_SIZE / PER_SECSIZE) + FatInitArgs_a[0].FAT1Sec)
/* 由簇号到扇区映射 */
#define START_SECTOR_OF_FILE(clu) (((clu-2)*g_dbr[0].secPerClus)+FatInitArgs_a[0].FirstDirSector)
/* 由簇号得其FAT所在扇区内偏移 */
#define TAKE_FAT_OFF(clu) ((clu * FAT_SIZE) % PER_SECSIZE)/FAT_SIZE
/* 检查文件信息中的文件属性字段 */
#define CHECK_FDI_ATTR(x) x->attribute
/* 是否为文件末尾 */
#define IS_EOF(clu) (clu == 0x0fffffff)
/* 扩展名是否为空 */
#define EXTNAME_EMP(x) ((x[8] == ' ')&&(x[9] == ' ')&&(x[10] == ' '))
/* 是否超时 */
#if YC_TIMEOUT_SWITCH
#define IS_TIMEOUT(n_t,tot) ((n_t-tot)>yc_fstick)
#endif

/* 寻找空簇错误码 */
#define FOUND_FREE_CLU 0
#define NO_FREE_CLU -1
/* 创建文件错误码 */
#define CRT_FILE_OK 0
#define CRT_SAME_FILE_ERR -1
#define CRT_FILE_NO_FREE_CLU_ERR -2
#define CRT_FILE_INVALID_PARAM -3
/* 创建目录错误码 */
#define CRT_DIR_OK 0
#define CRT_SAME_DIR_ERR -1
#define CRT_DIR_NO_FREE_CLU_ERR -2
/* 目录索引错误码 */
#define ENTER_ROOT_PDIR_ERROR -1
#define ENTER_DIR_TIMEOUT_ERROR -2
#define ENTER_DIR_ERROR -3
/* 关闭文件错误 */
#define CLOSE_HOLE_FILE_ERR -1
/* 写文件错误码 */
#define WRITE_FILE_PARAM_ERR -1
#define WRITE_FILE_CLOSED_ERR -2
#define WRITE_FILE_LENGTH_WARN -3
/* 删除文件错误码 */
#define DEL_FILE_OPENED_ERR -1
#if YC_FAT_MKFS
/* 格式化错误码 */
#define NOTSUPPORTED_SIZE -1
#endif

#ifndef MIN
/* 简单逻辑运算，标准C平台可能报错 */
#define MIN(x, y) ({\
                    typeof(x) _x = (x);\
                    typeof(y) _y = (y);\
                    _x > _y ? _y : _x;\
                    })/* 任意值比较返回最小 */
#endif
#ifndef MAX
#define MAX(x, y) ({\
                    typeof(x) _x = (x);\
                    typeof(y) _y = (y);\
                    _x > _y ? _x : _y;\
                    })/* 任意值比较返回最大 */
#endif
#ifndef SWAP_TWO_BYTES
#define SWAP_TWO_BYTES(ptr1, ptr2) \
    do { \
        unsigned char temp = *(ptr1); \
        *(ptr1) = *(ptr2); \
        *(ptr2) = temp; \
    } while (0)
#endif
#define CLR_BIT(a,n) (a = a&(~(1<<n)))/* a的第n位清0 */
#define SET_BIT(a,n) (a = a|(1<<n))/* a的第n位置1 */
struct list_head ycfatBlockHead;/* 挂载链头节点，不携带实际数据 */
static char fatobjNodeNum = 0;
// bit map for FAT table
/* 只定义一个位图，不支持多磁盘分区 */
/* FAT进行位图映射时，直接将FAT值和0作逻辑或运算 */
uint8_t clusterBitmap[(PER_SECSIZE/FAT_SIZE)/8];//16Bytes
/* 下一个可用FAT所在扇区 */
static unsigned int cur_fat_sec;
/* 当前所在目录 */
static unsigned int work_clu[4] = {2,2,2,2};
/* 临时交换区,TakeFileClusList_Eftv用 */
static unsigned char buffer0[PER_SECSIZE];
/* 临时交换区,写文件用 */
static unsigned char buffer1[PER_SECSIZE];
#if FAT2_ENABLE
/* 临时交换区,FAT2备份用 */
static unsigned char buffer2[PER_SECSIZE];
#endif
/* 临时交换区,删除文件用 */
static unsigned char buffer3[PER_SECSIZE];
#if YC_FAT_MKFS
/* 临时交换区,格式化用 */
static unsigned char buffer4[PER_SECSIZE];
#if FROMAT_STRATEGY_SET == FDISK
J_ROM_UINT8 temp_fs_mbr[PER_SECSIZE] = {
	0x02, 0x03, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x99, 0x0B, 0x51, 0x48, 0x00, 0x00, 0x00, 0x00, 
	0x21, 0x00, 0x0C, 0xFE, 0xFF, 0xFF, 0x20, 0x00, 0x00, 0x00, 0xE0, 0x3F, 0xD8, 0x01, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0xAA
};
#endif
/* DBR模板 */
J_ROM_UINT8 temp_fs_dbr[PER_SECSIZE] = {
	0xEB, 0x58, 0x90, 0x4D, 0x53, 0x44, 0x4F, 0x53, 0x35, 0x2E, 0x30, 0x00, 0x02, 0x08, 0x60, 0x14, 
	0x02, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0x00, 0x20, 0x00, 0x00, 0x00, 
	0xE0, 0x3F, 0xD8, 0x01, 0xD0, 0x75, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 
	0x01, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x80, 0x00, 0x29, 0x1F, 0xEB, 0x1E, 0xB0, 0x4E, 0x4F, 0x20, 0x4E, 0x41, 0x4D, 0x45, 0x20, 0x20, 
	0x20, 0x20, 0x46, 0x41, 0x54, 0x33, 0x32, 0x20, 0x20, 0x20, 0x33, 0xC9, 0x8E, 0xD1, 0xBC, 0xF4, 
	0x7B, 0x8E, 0xC1, 0x8E, 0xD9, 0xBD, 0x00, 0x7C, 0x88, 0x56, 0x40, 0x88, 0x4E, 0x02, 0x8A, 0x56, 
	0x40, 0xB4, 0x41, 0xBB, 0xAA, 0x55, 0xCD, 0x13, 0x72, 0x10, 0x81, 0xFB, 0x55, 0xAA, 0x75, 0x0A, 
	0xF6, 0xC1, 0x01, 0x74, 0x05, 0xFE, 0x46, 0x02, 0xEB, 0x2D, 0x8A, 0x56, 0x40, 0xB4, 0x08, 0xCD, 
	0x13, 0x73, 0x05, 0xB9, 0xFF, 0xFF, 0x8A, 0xF1, 0x66, 0x0F, 0xB6, 0xC6, 0x40, 0x66, 0x0F, 0xB6, 
	0xD1, 0x80, 0xE2, 0x3F, 0xF7, 0xE2, 0x86, 0xCD, 0xC0, 0xED, 0x06, 0x41, 0x66, 0x0F, 0xB7, 0xC9, 
	0x66, 0xF7, 0xE1, 0x66, 0x89, 0x46, 0xF8, 0x83, 0x7E, 0x16, 0x00, 0x75, 0x39, 0x83, 0x7E, 0x2A, 
	0x00, 0x77, 0x33, 0x66, 0x8B, 0x46, 0x1C, 0x66, 0x83, 0xC0, 0x0C, 0xBB, 0x00, 0x80, 0xB9, 0x01, 
	0x00, 0xE8, 0x2C, 0x00, 0xE9, 0xA8, 0x03, 0xA1, 0xF8, 0x7D, 0x80, 0xC4, 0x7C, 0x8B, 0xF0, 0xAC, 
	0x84, 0xC0, 0x74, 0x17, 0x3C, 0xFF, 0x74, 0x09, 0xB4, 0x0E, 0xBB, 0x07, 0x00, 0xCD, 0x10, 0xEB, 
	0xEE, 0xA1, 0xFA, 0x7D, 0xEB, 0xE4, 0xA1, 0x7D, 0x80, 0xEB, 0xDF, 0x98, 0xCD, 0x16, 0xCD, 0x19, 
	0x66, 0x60, 0x80, 0x7E, 0x02, 0x00, 0x0F, 0x84, 0x20, 0x00, 0x66, 0x6A, 0x00, 0x66, 0x50, 0x06, 
	0x53, 0x66, 0x68, 0x10, 0x00, 0x01, 0x00, 0xB4, 0x42, 0x8A, 0x56, 0x40, 0x8B, 0xF4, 0xCD, 0x13, 
	0x66, 0x58, 0x66, 0x58, 0x66, 0x58, 0x66, 0x58, 0xEB, 0x33, 0x66, 0x3B, 0x46, 0xF8, 0x72, 0x03, 
	0xF9, 0xEB, 0x2A, 0x66, 0x33, 0xD2, 0x66, 0x0F, 0xB7, 0x4E, 0x18, 0x66, 0xF7, 0xF1, 0xFE, 0xC2, 
	0x8A, 0xCA, 0x66, 0x8B, 0xD0, 0x66, 0xC1, 0xEA, 0x10, 0xF7, 0x76, 0x1A, 0x86, 0xD6, 0x8A, 0x56, 
	0x40, 0x8A, 0xE8, 0xC0, 0xE4, 0x06, 0x0A, 0xCC, 0xB8, 0x01, 0x02, 0xCD, 0x13, 0x66, 0x61, 0x0F, 
	0x82, 0x74, 0xFF, 0x81, 0xC3, 0x00, 0x02, 0x66, 0x40, 0x49, 0x75, 0x94, 0xC3, 0x42, 0x4F, 0x4F, 
	0x54, 0x4D, 0x47, 0x52, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x0A, 0x44, 0x69, 
	0x73, 0x6B, 0x20, 0x65, 0x72, 0x72, 0x6F, 0x72, 0xFF, 0x0D, 0x0A, 0x50, 0x72, 0x65, 0x73, 0x73, 
	0x20, 0x61, 0x6E, 0x79, 0x20, 0x6B, 0x65, 0x79, 0x20, 0x74, 0x6F, 0x20, 0x72, 0x65, 0x73, 0x74, 
	0x61, 0x72, 0x74, 0x0D, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAC, 0x01, 0xB9, 0x01, 0x00, 0x00, 0x55, 0xAA
};
J_ROM_UINT8 temp_fsinfo1[4] = {
	0x52, 0x52, 0x61, 0x41,
};
J_ROM_UINT8 temp_fsinfo2[28] = {
                            0x72, 0x72, 0x41, 0x61, 0xFC, 0x7B, 0x1D, 0x00, 0x04, 0x00, 0x00, 0x00, //总空簇数和下一个空簇号
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0xAA
};
J_ROM_UINT8 temp_fattable[32] = {
	0xF8, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF, 0xFF, 0x0F, 
	0xFF, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
};
J_ROM_UINT8 temp_rootdir[128] = {
	0x42, 0x20, 0x00, 0x49, 0x00, 0x6E, 0x00, 0x66, 0x00, 0x6F, 0x00, 0x0F, 0x00, 0x72, 0x72, 0x00, 
	0x6D, 0x00, 0x61, 0x00, 0x74, 0x00, 0x69, 0x00, 0x6F, 0x00, 0x00, 0x00, 0x6E, 0x00, 0x00, 0x00, 
	0x01, 0x53, 0x00, 0x79, 0x00, 0x73, 0x00, 0x74, 0x00, 0x65, 0x00, 0x0F, 0x00, 0x72, 0x6D, 0x00, 
	0x20, 0x00, 0x56, 0x00, 0x6F, 0x00, 0x6C, 0x00, 0x75, 0x00, 0x00, 0x00, 0x6D, 0x00, 0x65, 0x00, 
	0x53, 0x59, 0x53, 0x54, 0x45, 0x4D, 0x7E, 0x31, 0x20, 0x20, 0x20, 0x16, 0x00, 0x35, 0x03, 0xA6, 
	0x31, 0x58, 0x31, 0x58, 0x00, 0x00, 0x04, 0xA6, 0x31, 0x58, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
#endif

/* JYCFAT库只需要向底层提供数据buffer，起始扇区，扇区数三个参数即可 */
extern void usr_read(void * buffer,unsigned int SecIndex,unsigned int SecNum);
extern void usr_write(void * buffer,unsigned int SecIndex,unsigned int SecNum);
extern void usr_clear(unsigned int SecIndex,unsigned int SecNum);

/* 大小端检测 */
int endian_checker(void)
{
	union e_cont emp;
	emp.val = 0x90eb;
	if(emp.ch[0] == 0xeb) endian = 0x00;//little-endian
	else endian = 0x01;//big-endian
}

/* 字符串复制1 */
static void YC_StrCpy(unsigned char *_tar,unsigned char *_src)
{
	do
	{
		*_tar++ = *_src;
	}
	while ((*_src++)!='\0');
}

/* 字符串复制2 */
static void YC_StrCpy_l(unsigned char *_tar,unsigned char *_src, unsigned len)
{
	if((NULL == _tar) || (0 == len))
		return;
	do
	{
		*_tar++ = *_src;
        len = len - 1;
	}while (((*_src++)!='\0') && len);
}
/* 字符串复制3 */
static void YC_ConstMem_l(unsigned char *_tar, const unsigned char *_src, unsigned len){
    if ((NULL == _tar) || (NULL == _src) || (0 == len)) return;
    do {
        *_tar++ = *_src;
        _src++;
        len = len - 1;
    } while (len);
}
/* 内存设置 */
#if 1
static void *YC_Memset(void *dest, int set, unsigned len)
{
    if ((NULL == dest) || (0 == len))
        return NULL;
    char *pdest = (char *)dest;
    uint32_t set_word = (uint32_t)((set & 0xFF) | (set << 8) | (set << 16) | (set << 24));
    while (len >= 4){
        *((uint32_t *)pdest) = set_word;
        pdest += 4;
        len -= 4;
    }
    while (len-- > 0) *pdest++ = (char)set;
    return dest;
}
#endif

/* 内存复制，总线利用率较高 */
static void YC_MemCpy(unsigned char *_tar, unsigned char *_src, unsigned len) {
    if ((NULL == _tar) || (NULL == _src) || (0 == len)) return;
    int temp = len/sizeof(unsigned int);
	int i;
    for(i=0; i<temp; i++)
    {
        ((unsigned int *)_tar)[i] = ((unsigned int *)_src)[i];
    }
    i *= sizeof(unsigned int);
    for(;i<len;i++) _tar[i] = _src[i];
}

/* 解析字符串长度 */
static unsigned int YC_StrLen(unsigned char *str)
{
    unsigned char * p_str = str; unsigned int len = 0;

    /* 当遍历到字符'\0'时停止 */
    while( '\0' != (*p_str))
    {
        len ++; p_str ++;
    }
    return len;
}

/* 字符串比较 */
static char YC_StrCmp(unsigned char *str1, unsigned char *str2) {
    while (*str1 != '\0' && *str2 != '\0') {
        if (*str1 != *str2)
			return 0; // 不相等
        str1++; str2++;
    }
    return (*str1 == '\0' && *str2 == '\0');
}
/* 文件名不允许的字符：
反斜杠 ()：在FAT32中，反斜杠用作目录分隔符，因此不能在文件名中使用。
正斜杠 (/)：与反斜杠一样，正斜杠也被用作目录分隔符，不能在文件名中使用。
冒号 (:)：冒号在FAT32中有特殊含义，用于分隔驱动器名和路径，因此不能在文件名中使用。
星号 (*)：星号在FAT32中被视为通配符，不能用于文件名。
问号 (?)：问号也被视为通配符，在文件名中不允许使用。
双引号 (")：双引号也不允许在文件名中使用。
尖括号 (< >)：尖括号也被禁止在文件名中。
竖线 (|)：竖线也不能在文件名中使用。
分号 (;)：分号也不允许在文件名中使用。
逗号 (,)：逗号也被禁止在文件名中。*/
/* 对于其他特殊字符，某些操作系统或应用程序可能对这些字符有特殊处理，请谨慎使用 */
/* 文件名是否合规 */
static char IS_FILENAME_ILLEGAL(unsigned char *fn)
{
    J_UINT8 isIllegal = 1;
    while (*fn)
    {
        if ((*fn == '\\') || (*fn == '/') || (*fn == ':') || (*fn == '*') ||
            (*fn == '?') || (*fn == '"') || (*fn == '<') || (*fn == '>') ||
            (*fn == '|') || (*fn == ';') || (*fn == ',') || (*fn == ' '))
        {
            isIllegal = 0;
            break;
        }
        fn++;
    }
    return isIllegal;
}

/* 将Byte转化为数值 */
static unsigned int Byte2Value(unsigned char *data,unsigned char len)
{
    if(len > 4) return 0;

    unsigned int temp = 0;
    if(len >= 1) temp|=((unsigned char)data[0]);
    if(len >= 2) temp|=((unsigned char)data[1])<<8;
    if(len >= 3) temp|=((unsigned char)data[2])<<16;
    if(len >= 4) temp|=((unsigned char)data[3])<<24;

    return temp;
}

/* 将长整型数值转换为4字节 */
static void Value2Byte4(unsigned int *data,unsigned char * des)
{
    *(unsigned char *)des = *(unsigned int *)data;
    *((unsigned char *)des+1) = *(unsigned int *)data>>8;
    *((unsigned char *)des+2) = *(unsigned int *)data>>16;
    *((unsigned char *)des+3) = *(unsigned int *)data>>24;
}

/* 将长整型数值转换为2字节 */
static void Value2Byte2(unsigned short *data,unsigned char * des)
{
    *(unsigned char *)des = *(unsigned short *)data;
    *((unsigned char *)des+1) = *(unsigned short *)data>>8;
}

/* 二分查表,数据是char型,必须是升序数列 */
static int bslp_srch(int NumToSearch,unsigned char *sequence,unsigned int len)
{
    int b1 = 0,b2 = len-1;
    int index;
    while(b1<=b2)
    {
        index = b1+(b2-b1)/2;
        if(sequence[index] != NumToSearch)
        {
            if( sequence[index] < NumToSearch )
            {
                b1 = index+1;
            }
            else if(sequence[index] > NumToSearch )
            {
                b2 = index-1;
            }
        }
        else if(sequence[index] == NumToSearch)
        {
#if YC_FAT_DEBUG
            printf("index is %d\r\n",index);
#endif
            return index;
        }
    }
    return -1;
}

/* 匹配驱动号 */
static struct list_head * YC_FAT_MatchDdn(unsigned char *drvn)
{
	/* 从挂载链删除 */
	struct list_head *pos = NULL,*tmp;
	list_for_each_safe(pos, tmp, &ycfatBlockHead){
		if(YC_StrCmp(drvn,(unsigned char *)(((ycfat_t *)pos)->ddn))) break;
	}
	return pos;
}

/* 解析DBR */
static void YC_FAT_ReadDBR(DBR_t * dbr_n)
{
    DBR_t * dbr = dbr_n;
	char i;
    unsigned char buffer[PER_SECSIZE];

    /* 若没有MBR扇区，则读取绝对0扇区 */
    if(0 == g_dbr_n)
        usr_read((unsigned char *)buffer,0,1);
    /* 读DBR所在扇区 */
    else
	{	
		for(i = 0;i<g_dbr_n;i++)
		{
			usr_read((unsigned char *)buffer,g_mbr.dpt[i].partStartSec,1);
				/* 解析buffer数据 */
			dbr->bytsPerSec = Byte2Value((unsigned char *)(buffer+11),2); /* 每扇区大小，通常为512 */
			dbr->secPerClus = Byte2Value((unsigned char *)(buffer+13),1); /* 每簇扇区数 */  
			dbr->rsvdSecCnt = Byte2Value((unsigned char *)(buffer+14),2); /* 保留扇区数（DBR->FAT1）*/
			dbr->numFATs = Byte2Value((unsigned char *)(buffer+16),1);  /* FAT表数，通常为2 */
			dbr->totSec32 = Byte2Value((unsigned char *)(buffer+32),4); /* 总扇区数 */
			dbr->FATSz32 = Byte2Value((unsigned char *)(buffer+36),4); /* 每个FAT（FAT1或FAT2）表占用的扇区数，FAT32专用 */
		}
	}
}

/* 解析绝对0扇区的MBR或DBR */
static void YC_FAT_AnalyseSec0(void)
{
    MBR_t * mbr = (MBR_t *)&g_mbr;

    unsigned char buffer[PER_SECSIZE];

    /* 读取绝对0扇区 */
    usr_read(&buffer,0,1);

    /* 判断绝对0扇区是不是为MBR扇区 */
    if((*buffer == 0xEB)&&(*(buffer+1) == 0x58)&&(*(buffer+2) == 0x90))
    {
        g_dbr_n = 0;
    }

    /* 解析分区开始扇区和分区所占总扇区数 */
    for(unsigned char i = 0;i < 4 ; i++)
    {
        if( 0 == *(unsigned int *)(buffer+446+16*i+8) )
            continue;
        mbr->dpt[i].partStartSec = Byte2Value((unsigned char *)(buffer+446+16*i+8),4);
        g_dbr_n ++;
    }

    /* DBR初始化 */
    if(!g_dbr_n)
    {
        YC_FAT_ReadDBR((DBR_t *)&g_dbr[0]);
        /* 初始化系统参数 */
        FatInitArgs_a[0].FAT1Sec = g_dbr[0].rsvdSecCnt; /* FAT1起始扇区等于保留扇区数 */
        FatInitArgs_a[0].FirstDirSector = FatInitArgs_a[0].FAT1Sec\
                            + (g_dbr[0].numFATs * g_dbr[0].FATSz32);
    }else{
        for(unsigned char i = 0;i<g_dbr_n;i++)
        {
            YC_FAT_ReadDBR((DBR_t *)&g_dbr[i]);
            /* 初始化系统参数 */
            FatInitArgs_a[i].FAT1Sec = g_mbr.dpt[i].partStartSec\
                            + g_dbr[i].rsvdSecCnt;/* FAT1起始扇区等于DBR起始扇区保留扇区数 */
            FatInitArgs_a[i].FirstDirSector = FatInitArgs_a[i].FAT1Sec\
                             + (g_dbr[i].numFATs * g_dbr[i].FATSz32);
        }
    }
}

/* 文件名匹配，返回文件名长度 */
static unsigned char ycFilenameMatch(unsigned char *fileToMatch,unsigned char *fileName)
{
    unsigned char len1 = YC_StrLen(fileName);
    unsigned char len2 = YC_StrLen(fileToMatch);

    if(len1 != len2) return 0;
    unsigned char f_c = 0;
    while( (*(fileToMatch + f_c) == *(fileName + f_c)) && ( f_c < len1) )
    {
        f_c ++;
    }
    if(f_c < len1)
		return 0;
    return len1;
}

/* 8*3文件名转化为字符串类型 */
/* ycfat中规定文件名不含空格 */
static void FDI_FileNameToString(unsigned char *from, unsigned char *to)
{
    if((from == NULL) || (to == NULL))
        return;

    unsigned char *fn_fr = from;
    unsigned char *fn_to = to;
    unsigned char i = 0;

    /* 提取文件名 */
    while( ' ' != (*fn_fr))
    {
        *(fn_to + i) = (* fn_fr);
        if( 7 == i) break;
        fn_fr ++; i ++;
    }

    if(EXTNAME_EMP(from))
    {
        *(fn_to + (++i) - 1) = '\0';
        return;
    }
    *(fn_to + (++i) - 1) = '.';

    /* 提取扩展名 */
    fn_fr = from + 8;
    unsigned char j = 0;
    while(' ' != (*fn_fr))
    {
        *(fn_to + i) = (* fn_fr);
        if(2 == j) break;
        fn_fr ++; i ++; j ++;
    }
    *(fn_to + i + 1) = '\0';
}

/* 解析文件信息 */
static void YC_FAT_AnalyseFDI(FDI_t *fdi,FILE1 *fileInfo)
{
    FILE1 * flp = fileInfo;

    /* 文件起始簇号 */
    unsigned int fl_clus = 0;
    fl_clus =  Byte2Value((unsigned char *)&fdi->startClusLower,2);
    fl_clus |=  (Byte2Value((unsigned char *)&fdi->startClusUper,2) << 16);

    /* 解析文件创建日期 */
    typedef struct {J_UINT8 hour;J_UINT8 min;J_UINT8 sec;} fl_crtTime;
    typedef struct {J_UINT16 yy;J_UINT8 mm;J_UINT8 dd;} fl_crtDate;
    fl_crtTime time = {0}; fl_crtDate date = {0};
    unsigned short o_date = (Byte2Value(((unsigned char *)&fdi->crtDate),2));
    date.yy =  DATE_YY_BASE + ( (MASK_DATE_YY & o_date)>>9 );
    date.mm =  (MASK_DATE_MM & o_date) >> 5;
    date.dd = MASK_DATE_DD & o_date;

    /* 解析文件创建时间 */
    unsigned short o_time = (Byte2Value(((unsigned char *)&fdi->crtTime),2));
    time.hour =  (MASK_TIME_HOUR & o_time)>>11 ;
    time.min =  (MASK_TIME_MIN & o_time) >> 5;
    time.sec = 2 * (MASK_TIME_SEC & o_time);

    /* 解析文件最近访问日期 */
    fl_crtDate a_date = {0};
    unsigned short acd = (Byte2Value(((unsigned char *)&fdi->acsDate),2));
    a_date.yy =  DATE_YY_BASE + ( (MASK_DATE_YY & acd)>>9 );
    a_date.mm =  (MASK_DATE_MM & acd) >> 5;
    a_date.dd = MASK_DATE_DD & acd;

    /* 解析文件大小,精确到字节 */
    unsigned int f_s = Byte2Value((unsigned char *)&fdi->fileSize,4);

    /* 将读出的文件信息保存 */
    flp->CurClus_R = fl_clus;
#if !YC_FAT_MULT_SEC_READ
    flp->CurOffSec = 0;
	flp->CurOffByte = 0;
#else
	flp->EndCluSizeRead = 0;
#endif
	
    flp->fl_sz = flp->left_sz = f_s ;
    flp->FirstClu = fl_clus;
}

/* 获取文件下一簇簇号 */
static unsigned int YC_TakefileNextClu(unsigned int fl_clus)
{
    unsigned int fat_n = 0;
    FAT32_Sec_t fat_sec;

    /* 解析文件首簇在FAT表中的偏移 */
    /* 先计算总偏移 */
    unsigned int off_b = fl_clus * FAT_SIZE;
    /* 再计算扇区偏移,得到FAT所在绝对扇区 */
    unsigned int off_sec = off_b / PER_SECSIZE;
    unsigned int t_rSec = off_sec + FatInitArgs_a[0].FAT1Sec; /* 默认取DBR0中的数据 */

    /* 取当前扇区所有FAT */
    usr_read((unsigned char *)&fat_sec,t_rSec,1);

    FAT32_t * fat = (FAT32_t * )&fat_sec.fat_sec[0];
    unsigned char off_fat = (off_b % PER_SECSIZE)/4;/* 计算在FAT中的偏移（以FAT大小为单位） */
    fat += off_fat;

    /* 返回下一FAT */    
    return fat_n = Byte2Value((unsigned char *)fat,FAT_SIZE);
}

/* 解析根目录簇文件目录信息 */
static SeekFile YC_FAT_ReadFileAttribute(FILE1 * file,unsigned char *filename)
{
    unsigned char fileToMatch[13]; /* 最后一字节为'\0' */

    /* 获取根目录起始簇（第2簇） */
    /* FAT32中簇号是从2开始 */
    /* 先由DBR计算首目录簇所在扇区，这里默认只有一个DBR */
    unsigned int fileDirSec;
    if(!g_dbr_n)
        fileDirSec = g_dbr[0].rsvdSecCnt + (g_dbr[0].numFATs * g_dbr[0].FATSz32);
    else
        fileDirSec = g_mbr.dpt[0].partStartSec + g_dbr[0].rsvdSecCnt + (g_dbr[0].numFATs * g_dbr[0].FATSz32);
    
    unsigned int fdi_clu = ROOT_CLUS;

    /* 读取首目录簇下的所有扇区 */
    FDIs_t fdis;
    do{
        for(int i = 0;i < g_dbr[0].secPerClus;i++)
        {
            usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(fdi_clu)+i,1);

            /* 从buffer进行文件名匹配 */
            FDI_t *fdi = NULL;
            fdi = (FDI_t *)&fdis.fdi[0];

            /* 先检查文件属性是否为目录 */
            if( 1 ) /* 不是目录且没有删除 */
            {
                /* 从当前扇区地址循环偏移固定字节取文件名 */
                for( ; (unsigned int)fdi < (((unsigned int)&fdis)+PER_SECSIZE) ; fdi ++)
                {   
                    if(0xE5 != fdi->fileName[0])
                    {
                        /* 将目录簇中的8*3文件名转化为字符串类型 */
                        FDI_FileNameToString((unsigned char *)fdi->fileName, fileToMatch);

                        /* 匹配到文件名 */
                        if( ycFilenameMatch(fileToMatch,filename) )
                        {
                            YC_FAT_AnalyseFDI(fdi,file);
                            file->file_state = FILE_OPEN; return FOUND;
                        }
                    }
                }
            }
        }
        fdi_clu = YC_TakefileNextClu(fdi_clu);
    }while(!IS_EOF(fdi_clu));
    
    return NOTFOUND;
}

/* 从第n簇（目录起始簇）解析目录簇链文件目录信息 */
static SeekFile YC_FAT_MatchFile(unsigned int clu,FILE1 * file,unsigned char *filename)
{
    unsigned char DirToMatch[13]; /* 最后一字节为'\0' */
    unsigned int fdi_clu = clu;
    if(NULL == filename)
        return NOTFOUND;

    /* 读取首目录簇下的所有扇区 */
    FDIs_t fdis;
    do{
        for(int i = 0;i < g_dbr[0].secPerClus;i++)
        {
            usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(fdi_clu)+i,1);

            /* 从buffer进行文件名匹配 */
            FDI_t *fdi = NULL;
            fdi = (FDI_t *)&fdis.fdi[0];

            /* 先检查文件属性是否为目录 */
            if( 1 ) /* 不是目录且没有删除 */
            {
                /* 从当前扇区地址循环偏移固定字节取目录名 */
                for( ; (unsigned int)fdi < (((unsigned int)&fdis)+PER_SECSIZE) ; fdi ++)
                {   
                    if(0xE5 != fdi->fileName[0])
                    {
                        /* 将目录簇中的8*3文件名转化为字符串类型 */
                        FDI_FileNameToString((unsigned char *)fdi->fileName, DirToMatch);

                        /* 匹配到目录名 */
                        if( ycFilenameMatch(DirToMatch,filename) )
                        {
                            /* 解析文件目录项 */
                            YC_FAT_AnalyseFDI(fdi,file);
                            /* 保存文件FDI所在扇区及其在扇区内便宜啊至FILE1结构体，供写文件使用 */
                            file->fdi_info_t.fdi_sec = START_SECTOR_OF_FILE(fdi_clu)+i;
                            file->fdi_info_t.fdi_off = (unsigned int)fdi - (unsigned int)&fdis;

                            return FOUND;
                        }
                    }
                }
            }
        }
        fdi_clu = YC_TakefileNextClu(fdi_clu);
    }while(!IS_EOF(fdi_clu));
    
    return NOTFOUND;
}

/**************************************************/
/* 读取文件整条簇链（文件所有数据在所遍历的簇链中） */
/* 传入参数：文件首簇，其他中间簇或尾簇未测试      */
/* 传出参数：文件末簇                              */
/* 效率较低，对磁盘寿命有影响                 	   */
/**************************************************/
static int TakeFileClusList(unsigned int first_clu)
{
    unsigned int clu = first_clu;
#if YC_FAT_DEBUG
    printf("start\r\n");
#endif
    /* 遍历整条簇链 */
    do 
    {
        /* 添加打印信息 */
#if YC_FAT_DEBUG
        printf("%d\r\n",clu);
#endif
        clu = YC_TakefileNextClu(clu);
    }while(!IS_EOF(clu));
#if YC_FAT_DEBUG
    printf("end\r\n");
#endif
    return clu;
}

/**************************************************/
/* 读取文件整条簇链（文件所有数据在所遍历的簇链中） */
/* 传入参数：任意簇                              */
/* 传出参数：文件末簇                              */
/* 效率较高                 						  */
/**************************************************/
static int TakeFileClusList_Eftv(unsigned int first_clu)
{
    unsigned int clu = first_clu;
	unsigned int bk1;
    unsigned int *p = NULL;
    unsigned char off_fat = ((clu * FAT_SIZE) % PER_SECSIZE)/FAT_SIZE;/* 计算在FAT中的偏移（以FAT大小为单位） */
#if YC_FAT_DEBUG
    printf("文件首簇为%d\r\n",clu);
#endif
    /* 遍历整条簇链 */
    do
    {
		unsigned int h_clu = clu-off_fat;//当前FAT表内约束1
		unsigned int t_clu = h_clu+(PER_SECSIZE/FAT_SIZE)-1;//当前FAT表内约束2
#if YC_FAT_DEBUG
		printf("当前FAT扇区文件起始簇号为%d\r\n",clu);
		printf("当前FAT扇区最小簇号为%d\r\n",h_clu);
		printf("当前FAT扇区最大簇号为%d\r\n",t_clu);
#endif
        p = (unsigned int *)buffer0 + off_fat;
        /* 读出段簇所在扇区数据 */
        usr_read(buffer0,CLU_TO_FATSEC(clu),1);
		bk1 = clu;
#if YC_FAT_DEBUG
		printf("%d\r\n",clu);
#endif
        for(;;)
        {
			/* 在当前FAT扇区内逐个遍历，碰到段尾簇就寻找下一个段簇，继续读出下一段簇所在FAT扇区 */
            if( (*p > t_clu) || (*p < h_clu) )
            {
                clu = YC_TakefileNextClu(bk1);
                break;
            }
#if YC_FAT_DEBUG
			printf("%d\r\n",*p);
#endif
			bk1 = *p;
			*p = *((unsigned int *)buffer0+TAKE_FAT_OFF(bk1));
        }
        /* 重新计算偏移量 */
        off_fat = (((unsigned int)(*p) * FAT_SIZE) % PER_SECSIZE)/FAT_SIZE;
    }while(!IS_EOF(clu));
#if YC_FAT_DEBUG
	printf("文件尾簇为%d\r\n",bk1);
#endif
    return bk1;
}

/* 跨扇区，这个宏应该没什么用 */
#define READ_EOS(f) (0 == (f->fl_sz-f->left_sz)%PER_SECSIZE)

/* 删除并释放所有读簇链节点 */
static void YC_FAT_DelAndFreeAllCluChainNode(struct list_head * p_ChainHead)
{
    struct list_head *pos, *tmp;
    list_for_each_safe(pos, tmp, p_ChainHead)
    {
        list_del(pos);
        tFreeHeapforeach((void *)pos);
    }
}

/* 构建读簇链 */
static int YC_FAT_CreatReadCluChain(FILE1* fileInfo,unsigned int CluNum)
{
    struct list_head *pos, *tmp;
    unsigned int last_clu = fileInfo->CurClus_R;
    w_buffer_t * r_ccb = (w_buffer_t *)0;
    if(fileInfo == NULL) return -1;
    do{
        last_clu = YC_TakefileNextClu( last_clu );
        if(last_clu == 0x0fffffff) return -2;/* 改为断言 */
        if((last_clu != (r_ccb->w_e_clu + 1)) || (r_ccb == (w_buffer_t *)0))
        {
            r_ccb = (w_buffer_t *)tAllocHeapforeach(sizeof(w_buffer_t));
            if(NULL == r_ccb)
            {
                /* 错误处理，删除并释放所有链表节点 */
                YC_FAT_DelAndFreeAllCluChainNode(&fileInfo->RDCluChainList);
                return -1;
            }
            /* add to clu chain */
            list_add_tail(&r_ccb->WRCluChainNode,&fileInfo->RDCluChainList);
            r_ccb->w_s_clu = r_ccb->w_e_clu = last_clu;
            continue;
        }
        r_ccb->w_e_clu = last_clu;
    }while( -- CluNum );
    return 0;
}

/* 数据读取函数 */
static J_UINT32 YC_ReadDataNoCheck(FILE1* fileInfo,unsigned int off,unsigned int len,unsigned char * buffer)
{
    FILE1 * f_r;struct list_head *pos, *tmp;
    unsigned int t_rSize = MIN(len, fileInfo->left_sz);/* 需要读的数据大小 */
    unsigned int t_rSec,t_rCluNum;
    if(!t_rSize) return 0;
    unsigned int chain_low,chain_high;
    unsigned int once_secNum,int_secNum;
    unsigned int r_off = 0;
	unsigned short powder_len;
    unsigned char off_sec;
    unsigned int clu_size = PER_SECSIZE*g_dbr[0].secPerClus;
    unsigned int cur_leftsize = clu_size - fileInfo->EndCluSizeRead;
#if YC_FAT_MULT_SEC_READ
    if(t_rSize <= cur_leftsize)
    {
        off_sec = fileInfo->EndCluSizeRead/PER_SECSIZE;
        powder_len = cur_leftsize%PER_SECSIZE;
        if(powder_len)
        {
            if(t_rSize <= powder_len)
            {
                usr_read(buffer0,START_SECTOR_OF_FILE(fileInfo->CurClus_R)+off_sec,1);
                YC_MemCpy(buffer,buffer0+(PER_SECSIZE-powder_len),t_rSize);
            }
            else
            {
                usr_read(buffer0,START_SECTOR_OF_FILE(fileInfo->CurClus_R)+off_sec,1);
                YC_MemCpy(buffer,buffer0+(PER_SECSIZE - powder_len),powder_len);
                r_off += powder_len;off_sec += 1;
                int_secNum = once_secNum = (t_rSize-powder_len)/PER_SECSIZE;
                if((t_rSize-powder_len)%PER_SECSIZE) once_secNum++;
                usr_read(buffer0+r_off,START_SECTOR_OF_FILE(fileInfo->CurClus_R)+off_sec+1,int_secNum);
                if(int_secNum != once_secNum){
                    r_off += int_secNum*PER_SECSIZE;
                    usr_read(buffer0,START_SECTOR_OF_FILE(fileInfo->CurClus_R)+off_sec+int_secNum,1);
                    YC_MemCpy(buffer+r_off,buffer0,(t_rSize-powder_len)%PER_SECSIZE);
                }
            }
        }
        else
        {
            int_secNum = once_secNum = t_rSize/PER_SECSIZE;
            if(t_rSize%PER_SECSIZE) once_secNum++;
            usr_read(buffer,START_SECTOR_OF_FILE(fileInfo->CurClus_R)+off_sec,int_secNum);
            if(int_secNum != once_secNum)
            {
                r_off += int_secNum*PER_SECSIZE;
                usr_read(buffer0,START_SECTOR_OF_FILE(fileInfo->CurClus_R)+off_sec+int_secNum,1);
                YC_MemCpy(buffer+r_off,buffer0,t_rSize%PER_SECSIZE);
            }
        }
        goto refresh_para_and_exit;
    }
    else
    {
        t_rCluNum = (t_rSize - cur_leftsize)/clu_size;
        if((t_rSize - cur_leftsize)%clu_size)
        {
            t_rCluNum += 1;
        }
        if(0 > YC_FAT_CreatReadCluChain(fileInfo,t_rCluNum))
            return 0;
        int_secNum = once_secNum = cur_leftsize/PER_SECSIZE;
        powder_len = cur_leftsize%PER_SECSIZE;
        if(powder_len) once_secNum++;
        off_sec = fileInfo->EndCluSizeRead/PER_SECSIZE;
        if(int_secNum != once_secNum)
        {
            usr_read(buffer0,START_SECTOR_OF_FILE(fileInfo->CurClus_R)+off_sec,1);
            YC_MemCpy(buffer,buffer0+PER_SECSIZE-powder_len,powder_len);
            r_off += powder_len;
			off_sec ++;
        }
        usr_read(buffer+r_off,START_SECTOR_OF_FILE(fileInfo->CurClus_R)+off_sec,int_secNum);
        r_off += cur_leftsize;
    }

    /* 处理读缓冲簇链 */
    list_for_each_safe(pos, tmp, &fileInfo->RDCluChainList)
    {
        if(list_is_last(pos,&fileInfo->RDCluChainList))
        {
            chain_low = ((w_buffer_t *)pos)->w_s_clu;
            chain_high = ((w_buffer_t *)pos)->w_e_clu;
            fileInfo->CurClus_R = ((w_buffer_t *)pos)->w_e_clu;
			int_secNum = once_secNum = (t_rSize - r_off)/PER_SECSIZE;
			if((t_rSize - r_off)%PER_SECSIZE) once_secNum++;
            usr_read(buffer+r_off,START_SECTOR_OF_FILE(chain_low),int_secNum);
			r_off = r_off + int_secNum * PER_SECSIZE;
			powder_len = t_rSize - r_off;//最后不足一扇区的字节
			if(powder_len){
				usr_read(buffer0,START_SECTOR_OF_FILE(chain_low)+int_secNum,1);
			    YC_MemCpy(buffer+r_off,buffer0,powder_len);
            }
            break;/* 读簇缓冲链节点遍历完毕，跳出 */
        }
        /* 读连续簇链 */
        chain_low = ((w_buffer_t *)pos)->w_s_clu;
		chain_high = ((w_buffer_t *)pos)->w_e_clu;
        once_secNum = (chain_high-chain_low+1)*g_dbr[0].secPerClus;
        usr_read(buffer+r_off,START_SECTOR_OF_FILE(chain_low),once_secNum);
        r_off += once_secNum * PER_SECSIZE;/* 更新偏移量 */
    }
    /* 释放读簇缓冲链 */
	YC_FAT_DelAndFreeAllCluChainNode(&fileInfo->RDCluChainList);
refresh_para_and_exit:
    fileInfo->left_sz -= t_rSize;
    fileInfo->EndCluSizeRead = (fileInfo->fl_sz-fileInfo->left_sz)%clu_size;
    if(fileInfo->EndCluSizeRead == 0) fileInfo->EndCluSizeRead = clu_size;
#else
    /* 单扇区读 */
    char i;unsigned int l_ilegal = 0;  /* 已读的有效数据长度 */
    static unsigned char app_buf[PER_SECSIZE];static unsigned int bk = 0;
    unsigned int Secleft = 0,t_rb = t_rSize;/* 备份 */
    unsigned int n_clu = fileInfo->CurClus_R; /* 初始簇 */
    /* 计算需要读的扇区个数 */
	t_rSec = ( ( PER_SECSIZE*((fileInfo->fl_sz - fileInfo->left_sz + t_rSize)/PER_SECSIZE) )\
        - ( PER_SECSIZE*((fileInfo->fl_sz - fileInfo->left_sz)/PER_SECSIZE) ) )/PER_SECSIZE;
    if((fileInfo->fl_sz - fileInfo->left_sz + t_rSize)%PER_SECSIZE)
    {
		t_rSec += 1;
    }
    /* 找出当前簇内的首扇区（扇区偏移） */
    if(t_rSec)
        Secleft =  g_dbr[0].secPerClus - fileInfo->CurOffSec;
    Secleft = (Secleft <= t_rSec)?Secleft : t_rSec;
    do 
    {
        if(Secleft)
		{
            /* 下面的逻辑都是当前簇读 */
            for(i = 0; i < Secleft; i ++)
            {
                /* 取当前扇区数据 */
				usr_read(app_buf,START_SECTOR_OF_FILE(n_clu)+fileInfo->CurOffSec , 1);
                memcpy((unsigned char *)buffer+l_ilegal,app_buf+fileInfo->CurOffByte,MIN(PER_SECSIZE-fileInfo->CurOffByte,t_rSize));
				YC_Memset(buffer,0,PER_SECSIZE);
				YC_StrCpy_l(buffer,app_buf+fileInfo->CurOffByte,MIN(PER_SECSIZE-fileInfo->CurOffByte,t_rSize));
				
				l_ilegal += MIN(PER_SECSIZE-fileInfo->CurOffByte,t_rSize);      /* 迄今所读出的有效数据大小 */
				
				if(t_rSize <= PER_SECSIZE) bk = t_rSize;
				
                t_rSize = t_rSize - MIN(PER_SECSIZE-fileInfo->CurOffByte,t_rSize);/* 剩余数据的总大小 */
                
                if(!t_rSize){ 
					/* 锚定起始字节 */
					fileInfo->CurOffByte += bk;
					if(PER_SECSIZE == fileInfo->CurOffByte)
					{
						fileInfo->CurOffSec ++;
						if(g_dbr[0].secPerClus-1 == fileInfo->CurOffSec) 
							fileInfo->CurOffSec = 0;
						fileInfo->CurOffByte = 0;
					}
                    break;
                }
                /* 重新锚定起始字节 */
                fileInfo->CurOffByte = 0;
				/* 重新锚定起始扇区 */
				fileInfo->CurOffSec ++;
				if(g_dbr[0].secPerClus-1 == fileInfo->CurOffSec) 
					fileInfo->CurOffSec = 0;
            }
			
            /* 重新锚定起始簇 */
            t_rSec = t_rSec - Secleft;
            if(t_rSec){//剩下的需要读的总扇区大于0
                n_clu = YC_TakefileNextClu(n_clu);
                fileInfo->CurClus_R = n_clu;
                Secleft = (t_rSec >= g_dbr[0].secPerClus)?(g_dbr[0].secPerClus):(t_rSec);
            }else{
                break;
            }
            /* 重新锚定起始扇区 */
            fileInfo->CurOffSec = START_SECTOR_OF_FILE(n_clu);
        }
		else
		{
            break;
        }
    }while(!IS_EOF(n_clu));
    /* 可读扇区=0 进行边界处理 */
    if(READ_EOS(fileInfo) && (fileInfo->left_sz == 0)){
        fileInfo->CurClus_R = fileInfo->FirstClu;
        fileInfo->CurOffSec = 0;
    }
    fileInfo->left_sz -= t_rSize;
#endif
	return t_rSize;
}

/* 读文件 */
unsigned int YC_FAT_Read(FILE1* fileInfo,unsigned char * d_buf,unsigned int len)
{
    unsigned int ret;unsigned int off;
    if((FILE_OPEN != fileInfo->file_state) || (!fileInfo->fl_sz)) 
        return -1;
    if(1){
        off = fileInfo->fl_sz - fileInfo->left_sz;
	    ret = YC_ReadDataNoCheck(fileInfo,off,len,d_buf);//追加数据
    }
	return ret;
}

/* 小写转大写 */
static J_UINT8 Lower2Up(J_UINT8 ch)
{
    J_UINT8 Upch = 0xff;
    if((ch >= 0x61) && (ch <= 0x7a))
         Upch = *((J_UINT8 *)&ch) - 0x20;
    return Upch;
}

/* 大写转小写 */
static J_UINT8 Up2Lower(J_UINT8 ch)
{
    J_UINT8 Lowch = 0xff;
    if((ch >= 0x41) && (ch <= 0x5a))
         Lowch = *((J_UINT8 *)&ch) + 0x20;
    return Lowch;
}

/* 子字符串定位 */
static J_UINT8 FindSubStr(unsigned char *str,unsigned char *substr,unsigned char pos)
{
    J_UINT8 i = pos,j = 0,lens = YC_StrLen(str),lent = YC_StrLen(substr);
    while((i < lens) && (j < lent))
    {
        if((str[i] == substr[j]) || ('?' == substr[j])){
            i ++; j ++;
        }
        else{
            i = i - j + 1; j = 0;
        }
    }
    if(j == lent) 
        return (i - lent);
    else 
        return 0xff;
}

/* 文件通配 */
static J_UINT8 FileNameMatch(unsigned char *s,unsigned char *t)
{
    J_UINT8 i = 0, j = 0, lens = YC_StrLen(s), lent = YC_StrLen(t), flag = 0;
    J_UINT8 buf[10];
    J_UINT8 bufp = 0;
    while((j < lent) && ('*' != t[j]))
    {
        buf[bufp] = Lower2Up(t[j]);
        /* 大小写转化失败 */
        if(buf[bufp] == 0xff) return 1;
        bufp ++; j ++;
    }
    buf[bufp] = '\0';
    if(FindSubStr(s, buf, 0) != 0) return 0;
    i = bufp;
    while(1)
    {
        while((j < lent) && ('*' == t[j])) j ++;
        if(j == lent) return 1;
        bufp = 0;
        while((j < lent) && ('*' != t[j]))
        {
            buf[bufp] = Lower2Up(t[j]);
            /* 大小写转化失败 */
            if(buf[bufp] == 0xff) return 1;
            bufp ++; j ++;
        }
        buf[bufp] = '\0';
        if(j == lent)
        {
            if(FindSubStr(s, buf, i) != (lens - bufp)) return 0;
            return 1;
        }
        i = FindSubStr(s, buf, i);
        if(0xff == i) return 0;
        i += bufp;
    }
}

/* 文件路径预处理，剔除无效空格字符 */
static void DelexcSpace(unsigned char * s,unsigned char * d)
{
    int i = 0,j = 0;
    int sp_l = 0;
    unsigned char * s1 = s;
    while(' ' == *s1)
    {
        s1 += 1; sp_l++;
    }
    for(i = sp_l;i < YC_StrLen(s);i++)
    {
        d[j] = s[i];j++;
    }
    d[j] = '\0';
}

/* 字符串自截断 */
/* start是丢弃前start个字节 */
/* 传参source不能被const修饰 */
static void YC_SubStr(unsigned char *source, int start, int length)
{
    int sourceLength = YC_StrLen(source);
    /* Tip：这里直接定义一个50Bytes的数组不严谨，建议采用malloc机制 */
    unsigned char bk[50] = {0};
    if (start < 0 || start >= sourceLength || length <= 0){
        source[0] = '\0';
        return;
    }

    int i, j = 0;
    for (i = start; i < start + length && i < sourceLength; i++){
        bk[j++] = source[i];
    }

    bk[j] = '\0';
    YC_StrCpy(source, bk);
}

/* 从文件路径中匹配文件名 */
static char YC_FAT_TakeFN(unsigned char * s,unsigned char *d)
{
    int i = 0;
    unsigned char s_l = YC_StrLen(s);
    unsigned char *s_end = s + s_l-1;

    /* 匹配最后一个'/'所在位置，采用后序遍历 */
    while(*s_end)
    {
        if((*s_end == '/')||(*s_end == '\\'))
        {
            if(i == 0)
                return 0;
            break;
        }
        s_end--;i++;
    }
    YC_StrCpy(d, s_end+1);
    return 1;
}

/* 从文件路径中匹配目录名 */
static char YC_FAT_TakeFP(unsigned char * s,unsigned char *d)
{
    int i = 0;
    char s_l = YC_StrLen(s);
    unsigned char *s_end = s + s_l-1;

    /* 匹配最后一个'/'所在位置，采用后序遍历 */
    while(*s_end)
    {
        if((*s_end == '/')||(*s_end == '\\'))
        {
            if(i == 0)
                return 0;
            break;
        }
        s_end--;i++;
    }
    YC_StrCpy_l(d, s, s_l-i-1);
    return 1;
}

#if FILE_CACHE
/* 从缓冲区匹配文件 */
static int MatchFromCache(FILE1 * ftc)
{
	int i;
    for (i = 0; i < MAX_FILES_CACHE; i++){
        if((file_cache[i].fdi_info_c_t.fdi_sec_c == ftc->fdi_info_t.fdi_sec)&&
        (file_cache[i].fdi_info_c_t.fdi_off_c == ftc->fdi_info_t.fdi_off))
        {
            return i;
        }
    }
    return -1;
}
/* 更新文件缓冲区 */
static void update_cache(int ind, FILE1 * ftc)
{
    int i,j;FileCacheEntry tmp_cache;
    if(ind < 0) 
	{/* 缓冲区匹配不到该文件 */
		for(i = 0; i < MAX_FILES_CACHE; i++)
		{
			if (file_cache[i].fdi_info_c_t.fdi_sec_c == 0)
			{ /* 找到第一个空位 */
				for(j = i; j > 0; j--){
					file_cache[j] = file_cache[j-1];
				}
				file_cache[0].tail_cluster = ftc->EndClu;
				file_cache[0].fdi_info_c_t.fdi_off_c = ftc->fdi_info_t.fdi_off;
				file_cache[0].fdi_info_c_t.fdi_sec_c = ftc->fdi_info_t.fdi_sec;
				return;
			}
		}
		/* 找不到空位插到头部，其余缓冲区循环后移 */
		for(j = MAX_FILES_CACHE-1; j > 0; j--){
			file_cache[j] = file_cache[j-1];
		}
		file_cache[0].tail_cluster = ftc->EndClu;
		file_cache[0].fdi_info_c_t.fdi_off_c = ftc->fdi_info_t.fdi_off;
		file_cache[0].fdi_info_c_t.fdi_sec_c = ftc->fdi_info_t.fdi_sec;
		return;
    }else{/* 缓冲区可以匹配到该文件 */
        for(j = ind; j > 0; j--){
			file_cache[j] = file_cache[j-1];
        }
        file_cache[0] = tmp_cache;
		file_cache[0].fdi_info_c_t.fdi_off_c = ftc->fdi_info_t.fdi_off;
		file_cache[0].fdi_info_c_t.fdi_sec_c = ftc->fdi_info_t.fdi_sec;
		file_cache[0].tail_cluster = ftc->EndClu;
    }
}
#endif
#if MAX_OPEN_FILES
static char update_matchInfo(FILE1 * fto,char add_or_del,char del_mode)
{
	int i;
	if(add_or_del == 1){
		for(i = 0; i < MAX_OPEN_FILES; i++){
			if((matchInfo[i].ffdi_off == 0)&&(matchInfo[i].ffdi_sec == 0)) {
				matchInfo[i].ffdi_off = fto->fdi_info_t.fdi_off;matchInfo[i].ffdi_sec = fto->fdi_info_t.fdi_sec;
				matchInfo[i].is_open = 1;
				return 0;
			}
		}
	}else if(add_or_del == 2){
		if(del_mode == 1){
			for(i = 0; i < MAX_OPEN_FILES; i++){
				if((matchInfo[i].ffdi_off == fto->fdi_info_t.fdi_off)&&(matchInfo[i].ffdi_sec == fto->fdi_info_t.fdi_sec)) {
					matchInfo[i].ffdi_off = matchInfo[i].ffdi_sec = 0;
					matchInfo[i].is_open = 0;
					return 0;
				}
			}
		}else if(del_mode == 2){
			for(i = 0; i < MAX_OPEN_FILES; i++){
				if((matchInfo[i].ffdi_off == fto->fdi_info_t.fdi_off)&&(matchInfo[i].ffdi_sec == fto->fdi_info_t.fdi_sec)) return 1;
			}
		}
	}
	return 0;
}
#endif
/* 函数声明 */
static int YC_FAT_EnterDir(unsigned char *dir);
/* 打开文件（雏形） */
FILE1 * YC_FAT_OpenFile(FILE1 * f_op, unsigned char * filepath)
{
    if(!open_sem) return NULL;
    FILE1 * file = NULL;
    unsigned char fp[50];
    unsigned int file_clu = 0;
    int j = -1;
    if(f_op->file_state == FILE_OPEN)
        return NULL;
	unsigned char len1,len2;
    /* 文件路径预处理 */
    DelexcSpace(filepath,fp);
	len1 =  YC_StrLen(fp);
    /* 从路径匹配文件名 */
    unsigned char f_n[50] = {0};
    if(!YC_FAT_TakeFN(fp,f_n)) return NULL;
	len2 =  YC_StrLen(f_n);
    /* 从路径匹配目录名 */
    unsigned char f_p[50] = {0};
	if(len1-len2)
		YC_StrCpy_l(f_p,fp,len1-len2);
	
    if(!IS_FILENAME_ILLEGAL(f_n))
        return NULL;
    /* 进入文件目录，这里假设是标准绝对路径寻找文件 */
    file_clu = YC_FAT_EnterDir(f_p);

    if(FOUND == YC_FAT_MatchFile(file_clu,f_op,f_n))
    {
        /* 匹配成功 */
        file = f_op; 
		/* 找出文件尾簇,写文件用 */
		if(file->fl_sz == 0){
			file->EndClu = 0;
			file->EndCluLeftSize = 0;
		}
		else{
#if FILE_CACHE
            j = MatchFromCache(f_op);
            if(j < 0) {
                file->EndClu = TakeFileClusList_Eftv(file->FirstClu);
            }
            else {
                file->EndClu = TakeFileClusList_Eftv(file_cache[j].tail_cluster);
            }
            /* 更新文件缓冲区 */
            update_cache(j,f_op);
#else
            file->EndClu = TakeFileClusList_Eftv(file->FirstClu);
#endif
			/* 计算尾簇剩余可用空间 */
			file->EndCluLeftSize = PER_SECSIZE*g_dbr[0].secPerClus-(file->fl_sz)%(PER_SECSIZE*g_dbr[0].secPerClus);
			if(file->EndCluLeftSize == PER_SECSIZE*g_dbr[0].secPerClus)/* 临界处理 */
				file->EndCluLeftSize = 0;
        }
		file->EndCluSizeRead = 0;
        file->CurClus_R = file->FirstClu;//读索引（以簇为单位）
		INIT_LIST_HEAD(&file->RDCluChainList);
		INIT_LIST_HEAD(&file->WRCluChainList);
        file->file_state = FILE_OPEN; open_sem--;update_matchInfo(f_op,1,1);
        return file;
    }
#if YC_FAT_DEBUG
	else{
		printf("需要打开的文件不存在\r\n");
	}
#endif
    return NULL;
}

/* 关闭文件 */
int YC_FAT_Close(FILE1 * f_cl)
{
    if(NULL == f_cl) return CLOSE_HOLE_FILE_ERR;
	update_matchInfo(f_cl,2,1);
	open_sem ++;
    f_cl->CurClus_R = 0;
#if !YC_FAT_MULT_SEC_READ
	f_cl->CurOffSec = 0;
	f_cl->CurOffByte = 0;
#else
	f_cl->EndCluSizeRead = 0;
#endif
    f_cl->file_state = FILE_CLOSE;
    f_cl->FirstClu = 0;
    f_cl->fl_sz = 0;
    f_cl->left_sz = 0;
	f_cl->EndClu = 0;
	f_cl->EndCluLeftSize = 0;
	f_cl->WRCluChainList.next = NULL;
	f_cl->WRCluChainList.prev = NULL;
	f_cl->fdi_info_t.fdi_off = 0;
	f_cl->fdi_info_t.fdi_sec = 0;
	f_cl = NULL;
	return 0;
}

/* 从第n号簇（某一目录开始簇）开始匹配目录，并返回目录首簇 */
/* 配合enterdir函数使用 */
static unsigned int YC_FAT_MatchDirInClus(unsigned int clu,unsigned char *DIR)
{
    unsigned char DirToMatch[13]; /* 最后一字节为'\0' */
    unsigned int fdi_clu = clu;
    /* 目录起始簇号 */
    unsigned int dir_clu = 0;

    /* 读取首目录簇下的所有扇区 */
    FDIs_t fdis;
    do{
        for(int i = 0;i < g_dbr[0].secPerClus;i++)
        {
            usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(fdi_clu)+i,1);

            /* 从buffer进行文件名匹配 */
            FDI_t *fdi = NULL;
            fdi = (FDI_t *)&fdis.fdi[0];

            /* 先检查文件属性是否为目录 */
            if(1) /* 是目录且没有删除 */
            {
                /* 从当前扇区地址循环偏移固定字节取目录名 */
                for( ; (unsigned int)fdi < (((unsigned int)&fdis)+PER_SECSIZE) ; fdi ++)
                {   
                    if(0xE5 != fdi->fileName[0])
                    {
                        /* 将目录簇中的8*3名转化为字符串类型 */
                        FDI_FileNameToString((unsigned char *)fdi->fileName, DirToMatch);

                        /* 匹配到目录名 */
                        if( ycFilenameMatch(DirToMatch,DIR) )
                        {
                            dir_clu =  Byte2Value((unsigned char *)&fdi->startClusLower,2);
                            dir_clu |=  (Byte2Value((unsigned char *)&fdi->startClusUper[1],2) << 16);
                            return dir_clu;
                        }
                    }
                }
            }
        }
        fdi_clu = YC_TakefileNextClu(fdi_clu);
    }while(!IS_EOF(fdi_clu));
    
    return 0;
}

/* 目录解析，保存'/'或者'\\'之后的第一个目录 */
/* 将dir_s下的第一个/（斜杠）或者\\（反斜杠）后的子目录 */
/* 斜杠的ascii码为0x2f(47)，用'/'表示，占用一个字节*/
/* 反斜杠的ascii码为0x5c(92)，用'\\'表示，占用一个字节 */
/* 根目录没有./和../目录项，根目录下的目录的../项的簇号为2，也就是根目录簇号为2 */
/* 输入为标准的/dir1/dir2/dir3格式或者.././dir/dir1/.等格式 */
static unsigned char YC_FAT_ParseDir(unsigned char * dir_s,unsigned char * dir_d)
{
    int i = 0;unsigned char *p;
    int len = YC_StrLen(dir_s);
    if(
        ( '/' == (*(dir_s)) ) || ( '\\' == (*(dir_s)) )
    )
    {
        p = dir_s+1;
        while((*p != '\0')&&(*p != '/')&&(*p != '\\'))
        {
            dir_d[i++] = *p;
            p++;
        }
        dir_d[i] = '\0';
    }
    else if('.' == *dir_s){
        p = dir_s;
        while((*p != '\0')&&(*p != '/')&&(*p != '\\'))
        {
            dir_d[i++] = *p;
            p++;
        }
        dir_d[i] = '\0';
    }
	else
	{
		p = dir_s;
		while((*p != '\0')&&(*p != '/')&&(*p != '\\'))
        {
            dir_d[i++] = *p;
            p++;
        }
		dir_d[i] = '\0';
	}
    return i;
}

static unsigned int yc_fstick = 0;
unsigned int YC_TakeSystick(void)
{   
    return yc_fstick;
}

/* 进入目录 */
static int YC_FAT_EnterDir(unsigned char *dir)
{
    unsigned int dir_clu = 0xffffffff;

    unsigned char dir_temp[20] = {0};
    unsigned char i = 0;
	/* 起始目录处理 */
	if(YC_StrLen(dir) == 0){
		dir_clu = work_clu[0];/* 表示当前目录 */
		return dir_clu;
	}
	else if(( (*(dir) == '/') || (*(dir) == '\\') )&&(YC_StrLen(dir) == 1))
	{
		return ROOT_CLUS;/* 表示根目录 */
	}
	else if((*(dir) != '/') && ((*(dir) != '\\')))
	{
		dir_clu = work_clu[0];/* 表示当前目录 */
	}
	if(dir_clu == 0xffffffff)
		dir_clu = ROOT_CLUS;
#if YC_TIMEOUT_SWITCH
    int tick_now = YC_TakeSystick();
#endif

    /* 匹配子目录 */
    for( ; ; )
    {
        if(!YC_StrLen(dir))
            break;
        {
            i = YC_FAT_ParseDir(dir,dir_temp);/* 输入为标准的/dir1/dir2/dir3格式或者.././dir1/dir2/.等格式 */
			if(0 == i)
				break;
            if(
				( 
			(ROOT_CLUS == dir_clu) && 
			('.' == *dir_temp) && 
			('.' == *(dir_temp+1)) && (YC_StrLen(dir_temp) == 2)
			) 
			){
                return ENTER_ROOT_PDIR_ERROR;/* 非法目录 */
            }
            if(
				( 
			(ROOT_CLUS == dir_clu) && 
			('.' == *dir_temp) && (YC_StrLen(dir_temp) == 1)
			) 
			)
				dir_clu = ROOT_CLUS;
			else
				dir_clu = YC_FAT_MatchDirInClus(dir_clu,dir_temp);
			if(!dir_clu)
			{
				return ENTER_DIR_ERROR;/* 没有此目录 */
			}
            YC_SubStr(dir, i+1, 100);
        }
        /* 匹配超时退出，返回错误码 */
#if YC_TIMEOUT_SWITCH
        if(IS_TIMEOUT(tick_now,1000));
            return ENTER_DIR_TIMEOUT_ERROR;
#endif
    }
    return dir_clu;
}

/* 进入目录（用户接口） */
int YC_FAT_UsrEnterDir(unsigned char *dir1)
{
	unsigned int dir_clu = 0xffffffff;

    unsigned char dir_temp[20] = {0};
    unsigned char i = 0;
	
	unsigned char dir[30] = {0};
	YC_StrCpy_l(dir,dir1,YC_StrLen(dir1));
	/* 起始目录处理 */
	if(YC_StrLen(dir) == 0){
		dir_clu = work_clu[0];/* 表示当前目录 */
		return dir_clu;
	}
	else if(( (*(dir) == '/') || (*(dir) == '\\') )&&(YC_StrLen(dir) == 1))
	{
		work_clu[0] = ROOT_CLUS;
		return ROOT_CLUS;/* 表示根目录 */
	}
	else if((*(dir) != '/') && ((*(dir) != '\\')))
	{
		dir_clu = work_clu[0];/* 表示当前目录 */
	}
	if(dir_clu == 0xffffffff)
		dir_clu = ROOT_CLUS;
#if YC_TIMEOUT_SWITCH
    int tick_now = YC_TakeSystick();
#endif

    /* 匹配子目录 */
    for( ; ; )
    {
        if(!YC_StrLen(dir))
            break;
        {
            i = YC_FAT_ParseDir(dir,dir_temp);/* 输入为标准的/dir1/dir2/dir3格式或者.././dir1/dir2/.等格式 */
			if(0 == i)
				break;
            if(
				( 
			(ROOT_CLUS == dir_clu) && 
			('.' == *dir_temp) && 
			('.' == *(dir_temp+1)) && (YC_StrLen(dir_temp) == 2)
			) 
			){
                return ENTER_ROOT_PDIR_ERROR;/* 非法目录 */
            }
            if(
				( 
			(ROOT_CLUS == dir_clu) && 
			('.' == *dir_temp) && (YC_StrLen(dir_temp) == 1)
			) 
			)
				dir_clu = ROOT_CLUS;
			else
				dir_clu = YC_FAT_MatchDirInClus(dir_clu,dir_temp);
			if(!dir_clu)
			{
				return ENTER_DIR_ERROR;/* 没有此目录 */
			}
            YC_SubStr(dir, i+1, 100);
        }
        /* 匹配超时退出，返回错误码 */
#if YC_TIMEOUT_SWITCH
        if(IS_TIMEOUT(tick_now,1000));
            return ENTER_DIR_TIMEOUT_ERROR;
#endif
    }
	work_clu[0] = dir_clu;
    return dir_clu;
}

/* 获取当前工作目录 */
unsigned int YC_FAT_GetCurWorkDir(void)
{
	return work_clu[0];
}

/* 更新FSINFO扇区，主要用于更新剩余空闲簇数目 */
static void YC_FAT_UpdateFSInfo(void)
{
    FSINFO_t fsi,* pfsi = &fsi;
    usr_read((unsigned char *)&fsi,g_mbr.dpt[0].partStartSec+1,1);
    pfsi->Free_nClus[0] = FatInitArgs_a[0].FreeClusNum;
    pfsi->Free_nClus[1] = FatInitArgs_a[0].FreeClusNum>>8;
    pfsi->Free_nClus[2] = FatInitArgs_a[0].FreeClusNum>>16;
    pfsi->Free_nClus[3] = FatInitArgs_a[0].FreeClusNum>>24;
    usr_write((char *)&fsi,g_mbr.dpt[0].partStartSec+1,1);
}

/* 读取FSINFO扇区 */
static void YC_FAT_ReadInfoSec(unsigned int *leftnum)
{
    FSINFO_t fsinfo;
    usr_read((unsigned char *)&fsinfo,g_mbr.dpt[0].partStartSec+1,1);
    FatInitArgs_a[0].FreeClusNum = Byte2Value((unsigned char *)&fsinfo.Free_nClus,4);
}

/* 从FAT第一个扇区遍历FAT，寻找第一个空簇 */
static int YC_FAT_SeekFirstEmptyClus(unsigned int * d)
{
    /* 遍历FAT所有扇区 */
    /* 由DBR获取FAT首扇区地址 */
    int j = g_dbr[0].FATSz32;int k;
    unsigned int fat_ss = g_mbr.dpt[0].partStartSec+g_dbr[0].rsvdSecCnt;
    FAT32_Sec_t fat_secA;
    FAT32_t * fat;
    for(k = 0; k < j; k++)
    {
        /* 取当前扇区所有FAT链 */
        usr_read((unsigned char *)&fat_secA,fat_ss+k,1);
		fat = (FAT32_t *)&fat_secA.fat_sec[0];
        for(; (unsigned int)fat < ((unsigned int)&fat_secA+sizeof(FAT32_Sec_t)); fat++)
        {
            /* 找到一个空FAT */
            if(0x00 == *(unsigned int *)fat) {
                /* 将找到的这个FAT转换为转化为簇号 */
                *(unsigned int *)d = k*(PER_SECSIZE/FAT_SIZE)+((unsigned int)fat-(unsigned int)&fat_secA)/FAT_SIZE;
                return 0;
            }
            else continue;
        }
    }
    /* 找不到空FAT了返回错误码 */
    return -1;
}

/* FAT表映射到位图,默认1个扇区的FAT */
static int YC_FAT_RemapToBit(unsigned int start_sec)
{
    FAT32_Sec_t fat_secA;
    unsigned int *pi = (unsigned int *)&fat_secA;
    unsigned char *pc = clusterBitmap;
    unsigned char n = 0,k = 0;
    YC_Memset(clusterBitmap, 0, sizeof(clusterBitmap));
    /* 先读出FAT扇区所有数据 */
    usr_read((unsigned char *)&fat_secA,start_sec,1);
    /* 将整个FAT扇区映射到位图，0->0,!0->1 */
    while((unsigned int)pi < ((unsigned int)&fat_secA + PER_SECSIZE))
    {
        if((*pi)&&(0xffffffff) != 0){
            SET_BIT(*pc,n);k++;
        }
        /*else
            CLR_BIT(*pc,n);*/
        if(n == 7)
        {
            pc++;
            n = 0;
        }else{
            n++;
        }
        pi++;
    }
    /* 无空闲簇，返回错误码 */
    if(PER_SECSIZE/FAT_SIZE == k)
        return -1;
    return 0;
}

/* 扩展簇链（不进行自动缝合簇链） */
static int YC_FAT_ExpandCluChain(unsigned int theclu,unsigned int nextclu)
{
    /* 索引theclu在FAT表中的偏移 */
    FAT32_Sec_t fat_sec1;

    /* 解析文件首簇在FAT表中的偏移 */
    /* 先计算总偏移 */
    unsigned int off_b = theclu * FAT_SIZE;
    /* 再计算扇区偏移,得到FAT所在绝对扇区 */
    unsigned int off_sec = off_b / PER_SECSIZE;
    unsigned int t_rSec = off_sec + FatInitArgs_a[0].FAT1Sec; /* 默认取DBR0中的数据 */

    /* 取当前扇区所有FAT */
    usr_read((unsigned char *)&fat_sec1,t_rSec,1);

    FAT32_t * fat = (FAT32_t * )&fat_sec1.fat_sec[0];
    unsigned char off_fat = (off_b % PER_SECSIZE)/4;/* 计算在FAT中的偏移（以FAT大小为单位） */
    fat += off_fat;

    /* 修改此簇的下一簇为nextclu */    
    *(unsigned char *)(fat) = nextclu;
    *((unsigned char *)(fat)+1) = nextclu >> 8;
    *((unsigned char *)(fat)+2) = nextclu >> 16;
    *((unsigned char *)(fat)+3) = nextclu >> 24;

    /* 回写扇区 */
    usr_write((unsigned char *)&fat_sec1,t_rSec,1);
    return 0;
}
#define ARGVS_ERROR -99
/* 寻找当前簇的下一个空闲簇 */
/* 简单测试通过 */
static int YC_FAT_SeekNextFirstEmptyClu(unsigned int current_clu,unsigned int * free_clu)
{
    if(!free_clu) return ARGVS_ERROR;

    FAT32_Sec_t fat_sec1;FAT32_t * fat;
    current_clu ++;
    /* 是否存在满足需求的空簇 */
    if(!FatInitArgs_a[0].FreeClusNum) return NO_FREE_CLU;
    /* 从当前FAT表所在扇区向后遍历FAT表中的所有扇区，找出第一个空闲簇 */
    unsigned int t_rSec = (current_clu * FAT_SIZE / PER_SECSIZE) + FatInitArgs_a[0].FAT1Sec;
    for(;t_rSec < FatInitArgs_a[0].FAT1Sec + g_dbr[0].FATSz32;t_rSec ++)
    {
        /* 取当前扇区所有FAT */
        usr_read((unsigned char *)&fat_sec1,t_rSec,1);
        fat = (FAT32_t * )&fat_sec1.fat_sec[0];
        fat = fat + (current_clu * FAT_SIZE % PER_SECSIZE)/4;
        /* 从当前FAT所在扇区偏移开始向后遍历 */
        for(; (unsigned int)fat < ((unsigned int)&fat_sec1+sizeof(FAT32_Sec_t)); fat++)
        {
            current_clu ++;
            /* 找到一个FAT为0 */
            if(0 == *(unsigned int *)fat) {
                /* 将在一个扇区内的Byte偏移转化为簇号 */
                *(unsigned int *)free_clu = (t_rSec-FatInitArgs_a[0].FAT1Sec)*(PER_SECSIZE/FAT_SIZE)+\
                                            ((unsigned int)fat-(unsigned int)&fat_sec1)/FAT_SIZE;
                return 0;
            }
        }
    }
    /* 若遍历完，还未找到空簇，从头开始遍历 */
    if(-1 == YC_FAT_SeekFirstEmptyClus(free_clu))
        return -1;
    return FOUND_FREE_CLU;
}

/* ycfat初始化 */
int YC_FAT_Init(struct FilesystemOperations * fatobj)
{
    //if(NULL == fatobj) return -1;
    /* 大小端检测 */
    endian_checker();
	//unsigned char hid_rec[5] = MAKS_HID_RECYCLE;char i;
    /* 解析绝对0扇区 */
    YC_FAT_AnalyseSec0();
#if YC_FAT_DEBUG
	
#endif
    /* 解析DBR */
    YC_FAT_ReadDBR(&g_dbr[0]);

    /* 遍历FAT表，寻找第一个空闲簇 */
    if(-1 == YC_FAT_SeekFirstEmptyClus((unsigned int *)&FatInitArgs_a[0].NextFreeClu)) return -2;
    /* 第一个空闲簇所在FAT扇区 */
    cur_fat_sec = CLU_TO_FATSEC(FatInitArgs_a[0].NextFreeClu);

    /* 找出第一个有空闲簇的FAT扇区 */
    if((FatInitArgs_a[0].NextFreeClu != 0xffffffff) && (FatInitArgs_a[0].NextFreeClu != 0))
        YC_FAT_RemapToBit(cur_fat_sec);

    /* 读取FSINFO扇区，更新剩余空簇 */
    YC_FAT_ReadInfoSec((unsigned int *)&FatInitArgs_a[0].FreeClusNum);
		
    return 0;
}

/* 生成文件名，传参最多含有一个. */
static void Genfilename_s(unsigned char *filename,unsigned char *d)
{
    if((NULL == d)||(NULL == filename)) return;
    int len = YC_StrLen(filename);
    unsigned char *fn = filename;
    char i = 0;char j = 0;
    for(i = 0; i < len; i++){
        if('.' == fn[i])
            break;
    }
    /* 遍历后未发现字符'.' */
    if( len == i ) {
        if(8 >= len){
            YC_StrCpy_l(d,fn,len);
            for(j = len;j <= 7;j++) {d[j] = ' ';}
        }else{
            YC_StrCpy_l(d,fn,7);
            d[7] = '~';
        }
        d[8] = d[9] = d[10] = ' ';
    }
    /* 在串尾发现字符'.' */
    else if((len - 1) == i){
        if(i <= 8){
            YC_StrCpy_l(d,fn,i);
            for(j = i;j <= 7;j++) {d[j] = ' ';}
        }else{
            YC_StrCpy_l(d,fn,7);
            d[7] = '~';
        }
        d[8] = d[9] = d[10] = ' ';
    }
    /* 串头发现字符'.' */
    else if(!i){
        /* 直接返回，不做处理 */
        return;
    }
    /* 标准*.*格式或者多.格式 */
    else{
        if(i <= 8){
            YC_StrCpy_l(d,fn,i);
            for(j = i;j <= 7;j++) {d[j] = ' ';}
        }else{
            YC_StrCpy_l(d,fn,7);
            d[7] = '~';
        }
        if((len-i-1) <= 3){
            YC_StrCpy_l(d+8,fn+i+1,len-i-1);
            for(j=(len-i+7);j <= 10;j++) {d[j] = ' ';}
        }else{
            YC_StrCpy_l(d+8,fn+i+1,2);
            d[10] = '~';
        }
    }
}

/* 生成文件目录项fdi */
static void YC_FAT_GenerateFDI(FDI_t *fdi,unsigned char *filename,FDIT_t fdi_t)
{
    /* 检查传参合法性 */
    if((NULL == fdi)||(NULL == filename)||(!IS_FILENAME_ILLEGAL(filename))) return;
    
    FDI_t *fdi2full = fdi;
    unsigned char fn[11] = {0};

    /* 创建短文件名 */
    if(FDIT_FILE == fdi_t)
        Genfilename_s(filename,fn);
    else{
        if((*filename == '.')&&(*(filename+1) == '.')){
            fn[0] = '.';fn[1] = '.';fn[2]=fn[3]=fn[4]=fn[5]=fn[6]=fn[7]=fn[8]=fn[9]=fn[10] = ' ';
        }else if(*filename == '.'){
            fn[0] = '.';fn[1]=fn[2]=fn[3]=fn[4]=fn[5]=fn[6]=fn[7]=fn[8]=fn[9]=fn[10] = ' ';
        }
        else if((*filename != '.')&&(*(filename+1) != '.')){
            Genfilename_s(filename,fn);
        }
    }

    YC_StrCpy_l((unsigned char *)fdi2full->fileName,fn,sizeof(fn));/* fill file name */
    fdi2full->attribute = (FDIT_FILE == fdi_t)?ARCHIVE:TP_DIR;/* 属性字段文件or目录 */
	if(*filename == '.')
		fdi2full->UpLower = 0;
	else
		fdi2full->UpLower = (FDIT_FILE == fdi_t)?0x10:0x08;
    *(J_UINT16 *)fdi2full->startClusUper = 0;
    *(J_UINT16 *)fdi2full->startClusLower = 0;
    *(J_UINT32 *)fdi2full->fileSize = 0;
#if YC_TIMESTAMP_ON
    *(J_UINT16 *)fdi2full->crtTime = MAKETIME(systime_now());
    *(J_UINT16 *)fdi2full->crtDate = MAKEDATE(sysdate_now());
#else
    fdi2full->crtTime = 0;
    fdi2full->crtDate = 0;
#endif
}

/* 创建文件 */
int YC_FAT_CreateFile(unsigned char *filepath)
{
    if(NULL == filepath)
        return -1;
    int file_clu = 0;
	unsigned char f_n[50] = {0};
	unsigned char f_p[50] = {0};
    unsigned char fp[50] = {0};
    unsigned char FileToMatch[13] = {0}; /* 最后一字节为'\0' */
    unsigned int tail_clu = 0;
	unsigned char len1,len2;
    /* 文件路径预处理 */
    DelexcSpace(filepath,fp);
    len1 =  YC_StrLen(fp);
	
    if(!YC_FAT_TakeFN(fp,f_n)) 
		return CRT_FILE_INVALID_PARAM;
	len2 =  YC_StrLen(f_n);
	
	if(len1-len2)
		YC_StrCpy_l(f_p,fp,len1-len2);
    if(!IS_FILENAME_ILLEGAL(f_n))
		return CRT_FILE_INVALID_PARAM;

    /* 进入文件目录，返回首目录簇 */
    file_clu = YC_FAT_EnterDir(f_p);
	if(file_clu < 0)
		return ENTER_DIR_ERROR;
	
    FDIs_t fdis; FDI_t *fdi;
    do{
        tail_clu = file_clu;
        /* 遍历簇下所有扇区 */
        for(int i = 0;i < g_dbr[0].secPerClus;i++)
        {
            usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(file_clu)+i,1);
            fdi = (FDI_t *)&fdis.fdi[0];
            /* 从当前扇区地址循环偏移固定字节取文件/目录名 */
            for( ; (unsigned int)fdi < (((unsigned int)&fdis)+PER_SECSIZE) ; fdi ++)
            {   
                if(0x00 == *(char *)fdi) 
                {
                    YC_FAT_GenerateFDI(fdi,f_n,FDIT_FILE);
                    /* 回写当前扇区并退出 */
                    usr_write((char *)&fdis,START_SECTOR_OF_FILE(file_clu)+i,1);
                    return CRT_FILE_OK;
                }
                /* 将目录簇中的8*3名转化为字符串类型 */
                FDI_FileNameToString((unsigned char *)fdi->fileName, FileToMatch);
                /* 同名文件 返回错误码 */
                if(ycFilenameMatch(FileToMatch,f_n)) return CRT_SAME_FILE_ERR;
            }
        }
        file_clu = YC_TakefileNextClu(file_clu);
    }while(!IS_EOF(file_clu));

    /* 当前簇空间不足，寻找空簇扩展目录簇链 */
    /* 寻找第一个空闲簇 */
    unsigned int freeclu = FatInitArgs_a[0].NextFreeClu;

    /* 若没有空闲簇，错误返回 */
    if(0xffffffff == freeclu)
        return CRT_FILE_NO_FREE_CLU_ERR;

    /* 扩展目录簇链 */
    YC_FAT_ExpandCluChain(tail_clu,freeclu);
    YC_FAT_ExpandCluChain(freeclu,0x0fffffff);

    /* 在新簇头部写入新fdi */
    usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(freeclu),1);
    fdi = (FDI_t *)&fdis.fdi[0];
    YC_FAT_GenerateFDI(fdi,f_n,FDIT_FILE);
    usr_write((unsigned char *)&fdis,START_SECTOR_OF_FILE(freeclu),1);
    
    /* 更新FSINFO扇区中的空簇数目 */
    FatInitArgs_a[0].FreeClusNum --;
    YC_FAT_UpdateFSInfo();
    /* 寻找下一空闲簇 */
    if(FatInitArgs_a[0].FreeClusNum){
        if(-1 == YC_FAT_SeekNextFirstEmptyClu(freeclu,(unsigned int *)&FatInitArgs_a[0].NextFreeClu))
		{
			return CRT_FILE_OK;
		}            
		/* 继续将下一空闲簇所在FAT扇区映射 */
		cur_fat_sec = CLU_TO_FATSEC(FatInitArgs_a[0].NextFreeClu);
		if((FatInitArgs_a[0].NextFreeClu != 0xffffffff) && (FatInitArgs_a[0].NextFreeClu != 0))
			YC_FAT_RemapToBit(cur_fat_sec);
	}

    return CRT_FILE_OK;
}

/* 在当前簇下创建新目录，p_clu是新目录的父目录簇号 */
static int YC_GenDirInClu(unsigned int thisclu,unsigned int p_clu)
{
    FDIs_t fdis; FDI_t *fdi = (FDI_t *)&fdis;
	YC_Memset((char *)&fdis,0,sizeof(FDIs_t));
    YC_FAT_GenerateFDI(fdi,(unsigned char *)".",FDIT_DIR);
	fdi->startClusUper[0] = thisclu >> 16;
    fdi->startClusUper[1] = thisclu >> 24;
    fdi->startClusLower[0] = thisclu;
    fdi->startClusLower[1] = thisclu >> 8;
    
	fdi ++;
    YC_FAT_GenerateFDI(fdi,(unsigned char *)"..",FDIT_DIR);
	if(ROOT_CLUS != p_clu)
	{
		fdi->startClusUper[0] = p_clu >> 16;
		fdi->startClusUper[1] = p_clu >> 24;
		fdi->startClusLower[0] = p_clu;
		fdi->startClusLower[1] = p_clu >> 8;
	}
    usr_write((unsigned char *)&fdis,START_SECTOR_OF_FILE(thisclu),1);
    return 0;
}

/* 创建目录 */
int YC_FAT_CreateDir(unsigned char *dir)
{
    if(NULL == dir)
        return -1;
    unsigned int file_clu = 0; unsigned char f_n[50] = {0};unsigned char f_p[50] = {0};
    unsigned char fp[50];int freeclu;
    unsigned char FileToMatch[13]; /* 最后一字节为'\0' */
    unsigned int tail_clu = 0;
	unsigned char len1,len2;
    /* 文件路径预处理 */
    DelexcSpace(dir,fp);
    len1 =  YC_StrLen(fp);
	
    if(!YC_FAT_TakeFN(fp,f_n)) 
		return CRT_FILE_INVALID_PARAM;
	len2 =  YC_StrLen(f_n);
	
	if(len1-len2)
		YC_StrCpy_l(f_p,fp,len1-len2);
    if(!IS_FILENAME_ILLEGAL(f_n))
		return CRT_FILE_INVALID_PARAM;
	
    /* 进入文件目录，返回首目录簇 */
    file_clu = YC_FAT_EnterDir(f_p);

    FDIs_t fdis; FDI_t *fdi;
    do{
        tail_clu = file_clu;
        /* 遍历簇下所有扇区 */
        for(int i = 0;i < g_dbr[0].secPerClus;i++)
        {
            usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(file_clu)+i,1);
            fdi = (FDI_t *)&fdis.fdi[0];
            /* 从当前扇区地址循环偏移固定字节取文件/目录名 */
            for( ; (unsigned int)fdi < (((unsigned int)&fdis)+PER_SECSIZE) ; fdi ++)
            {   
                if(0x00 == *(char *)fdi) 
                {
                    if(!FatInitArgs_a[0].FreeClusNum)
						return CRT_DIR_NO_FREE_CLU_ERR;
                    YC_FAT_GenerateFDI(fdi,f_n,FDIT_DIR);
                    fdi->startClusUper[0] = FatInitArgs_a[0].NextFreeClu >> 16;
                    fdi->startClusUper[1] = FatInitArgs_a[0].NextFreeClu >> 24;
                    fdi->startClusLower[0] = FatInitArgs_a[0].NextFreeClu;
                    fdi->startClusLower[1] = FatInitArgs_a[0].NextFreeClu >> 8;

                    usr_write((char *)&fdis,START_SECTOR_OF_FILE(file_clu)+i,1);
                    YC_FAT_ExpandCluChain(FatInitArgs_a[0].NextFreeClu,0x0fffffff);
                    YC_GenDirInClu(FatInitArgs_a[0].NextFreeClu,file_clu);
                    freeclu = FatInitArgs_a[0].NextFreeClu;
                    YC_FAT_SeekNextFirstEmptyClu(freeclu,(unsigned int *)&FatInitArgs_a[0].NextFreeClu);
                    
                    /* 更新FSINFO扇区中的空簇数目 */
                    FatInitArgs_a[0].FreeClusNum --;
                    YC_FAT_UpdateFSInfo();
                    return CRT_DIR_OK;
                }
                /* 将目录簇中的8*3名转化为字符串类型 */
                FDI_FileNameToString((unsigned char *)fdi->fileName, FileToMatch);

                /* 同名目录 返回错误码 */
                if(ycFilenameMatch(FileToMatch,f_n)) return CRT_SAME_DIR_ERR;
            }
        }
        file_clu = YC_TakefileNextClu(file_clu);
    }while(!IS_EOF(file_clu));

    /* 当前簇空间不足，寻找空簇扩展目录簇链 */
    /* 寻找第一个空闲簇 */
    freeclu = FatInitArgs_a[0].NextFreeClu;

    /* 若没有空闲簇，错误返回 */
    if(0xffffffff == freeclu)
        return CRT_DIR_NO_FREE_CLU_ERR;

    /* 判断剩余空闲簇数目是否足够扩展目录 */
    if(!(FatInitArgs_a[0].FreeClusNum-2))
        return CRT_DIR_NO_FREE_CLU_ERR;
    
    /* 扩展目录簇链 */
    YC_FAT_ExpandCluChain(tail_clu,freeclu);
    YC_FAT_ExpandCluChain(freeclu,0x0fffffff);
    YC_FAT_SeekNextFirstEmptyClu(freeclu,(unsigned int *)&FatInitArgs_a[0].NextFreeClu);
    /* 在当前目录扩展新簇头部写入新fdi */
    usr_read((unsigned char *)&fdis,START_SECTOR_OF_FILE(freeclu),1);
    YC_Memset(&fdis, 0, sizeof(FDIs_t));
    fdi = (FDI_t *)&fdis.fdi[0];
    YC_FAT_GenerateFDI(fdi,f_n,FDIT_DIR);
    fdi->startClusUper[0] = FatInitArgs_a[0].NextFreeClu >> 16;
    fdi->startClusUper[1] = FatInitArgs_a[0].NextFreeClu >> 24;
    fdi->startClusLower[0] = FatInitArgs_a[0].NextFreeClu;
    fdi->startClusLower[1] = FatInitArgs_a[0].NextFreeClu >> 8;
    usr_write((unsigned char *)&fdis,START_SECTOR_OF_FILE(freeclu),1);

    YC_FAT_ExpandCluChain(FatInitArgs_a[0].NextFreeClu,0x0fffffff);
    /* 在子目录新簇写入fdi */
    YC_GenDirInClu(FatInitArgs_a[0].NextFreeClu,tail_clu);

    /* 更新FSINFO扇区中的空簇数目 */
    FatInitArgs_a[0].FreeClusNum -= 2;
    YC_FAT_UpdateFSInfo();
    /* 寻找下一空闲簇 */
    if(FatInitArgs_a[0].FreeClusNum){
        if(-1 == YC_FAT_SeekNextFirstEmptyClu(FatInitArgs_a[0].NextFreeClu,(unsigned int *)&FatInitArgs_a[0].NextFreeClu))
		{
			return CRT_DIR_OK;
		}
		/* 继续将下一空闲簇所在FAT扇区映射 */
		cur_fat_sec = CLU_TO_FATSEC(FatInitArgs_a[0].NextFreeClu);
		if((FatInitArgs_a[0].NextFreeClu != 0xffffffff) && (FatInitArgs_a[0].NextFreeClu != 0))
			YC_FAT_RemapToBit(cur_fat_sec);
	}
    return CRT_DIR_OK;
}

/* 在FAT位图中寻找下一个空簇,找不到下一个空簇就返回-1 */
static int SeekNextFreeClu_BitMap(unsigned int clu)
{
    /* clu在bitmap中的索引 */
    int a = clu%(PER_SECSIZE/FAT_SIZE);
    unsigned char b = a/8;
    signed char c = a%8;
    unsigned char * p = ((unsigned char *)clusterBitmap + b);
    unsigned int next = 0;
    char k = 0;

    if(c == 7){
        p++;c=-1;
    }
    for(;p < clusterBitmap + sizeof(clusterBitmap); p++)
    {
        if((*p & 0xff) == 0xff)
        {
            c = -1;
            continue;
        }
        else
        {
            for(k=c+1; k<8; k++)
            {
                if (((*p >> k) & 0x01) != 0x01)
                {
                    next = (cur_fat_sec-FatInitArgs_a[0].FAT1Sec)*(PER_SECSIZE/FAT_SIZE)\
                            + ((unsigned int)p - (unsigned int)clusterBitmap)*8 + k;
                    return next;
                }
            }
        }
        c = -1;
    }
    /* clu在当前FAT位图向下索引时没有空簇了 */
    return -1;
}

/* 将空簇添加至文件写缓冲簇链中 */
static int YC_FAT_AddToList(FILE1 *fl,unsigned int clu)
{
    if(NULL == fl) return -1;
    w_buffer_t *w_ccb = NULL;
    struct list_head *pos,*tmp;

    if(list_empty(&fl->WRCluChainList))
    {
        /* 新建第一个节点 */
        w_ccb = (w_buffer_t *)tAllocHeapforeach(sizeof(w_buffer_t));
        if(NULL == w_ccb)
        {
            /* 错误处理，删除并释放所有链表节点 */
            list_for_each_safe(pos, tmp, &fl->WRCluChainList)
            {
                list_del(pos);
                tFreeHeapforeach((void *)pos);
            }
            return -1;
        }
        /* 初始化第一个节点 */
        w_ccb->w_s_clu = w_ccb->w_e_clu = clu;
        list_add_tail(&w_ccb->WRCluChainNode,&fl->WRCluChainList);
    }
    else
    {
        /* 先找到最后一个压缩簇链缓冲节点 */
        w_ccb = (w_buffer_t *)(fl->WRCluChainList.prev);
        /* 新簇与压缩簇链缓冲节点进行匹配,匹配成功则添加至此节点 */
        if(clu == (w_ccb->w_e_clu + 1))
        {
            w_ccb->w_e_clu = clu;
        }
        /* 匹配失败则分配新节点 */
        else
        {
            w_ccb = (w_buffer_t *)tAllocHeapforeach(sizeof(w_buffer_t));
            if(NULL == w_ccb)
            {
                /* 错误处理，删除并释放所有链表节点 */
                list_for_each_safe(pos, tmp, &fl->WRCluChainList)
                {
                    list_del(pos);
                    tFreeHeapforeach((void *)pos);
                }
                return -1;
            }
            w_ccb->w_s_clu = w_ccb->w_e_clu = clu;
            list_add_tail(&w_ccb->WRCluChainNode,&fl->WRCluChainList);
        }
    }
    return 0;
}

/* 预建文件簇缓冲链（写） */
static int YC_FAT_CreateFileCluChain(FILE1 *fl,unsigned int cluNum)
{
    unsigned int ret = 0;
    /* 还原FatInitArgs_a[0].NextFreeClu备用 */
    unsigned int bkclu = FatInitArgs_a[0].NextFreeClu;
    /* 还原FatInitArgs_a[0].NextFreeClu备用1 */
    unsigned int bkclu1;
	if(!cluNum) return ret;
    /* 遍历bit map，将0位存放到链表中 */
    while(cluNum--)
    {
        if(-1 == YC_FAT_AddToList(fl,FatInitArgs_a[0].NextFreeClu)) //返回-1表示堆栈空间不足
        {
            /* 还原历史数据 */
            FatInitArgs_a[0].NextFreeClu = bkclu;
            cur_fat_sec = CLU_TO_FATSEC(FatInitArgs_a[0].NextFreeClu);
            YC_FAT_RemapToBit(cur_fat_sec);
            /* 退出,返回错误码 */
            ret = -1;
            break;
        }
        bkclu1 = FatInitArgs_a[0].NextFreeClu;
        FatInitArgs_a[0].NextFreeClu = SeekNextFreeClu_BitMap(FatInitArgs_a[0].NextFreeClu);
        if(-1 == FatInitArgs_a[0].NextFreeClu)
        {
            /* 继续往下找出第一个空闲簇 */
            FatInitArgs_a[0].NextFreeClu = bkclu1;
            YC_FAT_SeekNextFirstEmptyClu(FatInitArgs_a[0].NextFreeClu,(unsigned int *)&FatInitArgs_a[0].NextFreeClu);

            /* 继续将下一空闲簇所在FAT扇区映射 */
            cur_fat_sec = CLU_TO_FATSEC(FatInitArgs_a[0].NextFreeClu);
            if((FatInitArgs_a[0].NextFreeClu != 0xffffffff) && (FatInitArgs_a[0].NextFreeClu != 0))
                YC_FAT_RemapToBit(cur_fat_sec);
            else{
                /*没有整张磁盘都没有空闲簇了*/
                /* 错误处理 */
                ret = -1;break;
            }
        }
    }
#if YC_FAT_DEBUG
		struct list_head *pos1;int k = 0;
		list_for_each(pos1, &fl->WRCluChainList)
		{
			k++;
			printf("SEG%d:start_clu = %d,end_clu = %d\n",k,((w_buffer_t *)pos1)->w_s_clu,((w_buffer_t *)pos1)->w_e_clu);
		}
#endif
    return ret;
}

#if FAT2_ENABLE
/* 将FAT1表全部备份至FAT2,耗时长约几分钟，不建议使用 */
static void YC_FAT_BackedUpFAT2_A(void)
{
	unsigned int i = FatInitArgs_a[0].FAT1Sec;
	unsigned int i1 = FatInitArgs_a[0].FAT1Sec+g_dbr[0].FATSz32;
	unsigned int j;
    /* 备份FAT1至FAT2 */
    {
		for(j=0;j<g_dbr[0].FATSz32;j++){
			usr_read( buffer2,i+j,1);
			usr_write(buffer2,i1+j,1);
		}
	}
}
/* 将FAT1表局部备份至FAT2,适用与缝合簇链时同时进行 */
static void YC_FAT_BackedUpFAT2_1(unsigned int clu){
    unsigned int sec = START_SECTOR_OF_FILE(clu);
    usr_read(buffer2,sec,1);
    usr_write(buffer2,sec+g_dbr[0].FATSz32,1);
}

/* 将FAT1表局部备份至FAT2，适用缝合簇链时后 */
static void YC_FAT_BackedUpFAT2(FILE1 *f){

}
#endif

/* 缝合簇链 */
static void YC_FAT_SewCluChain(FILE1 *fl)
{
	struct list_head *pos,*next;
	unsigned int bootclu = 0;
	unsigned short bootclu_l16 = 0,bootclu_h16 = 0;
	unsigned temp,temp1,temp2;
	unsigned int t_clu;//当前FAT表内最大约束
	 unsigned char off_fat;/* 计算在FAT中的偏移（以FAT大小为单位） */
    if(fl->fl_sz == 0)
    {
		bootclu = ((w_buffer_t *)fl->WRCluChainList.next)->w_s_clu;/*提取引导簇*/
		bootclu_l16 = bootclu;
		bootclu_h16 = bootclu>>16;
		/* 新文件,先修改引导簇 */
		usr_read(buffer1,fl->fdi_info_t.fdi_sec,1);
		Value2Byte2((unsigned short *)&bootclu_h16,buffer1+fl->fdi_info_t.fdi_off+20);/* 修改文件引导簇的高16位 */
		Value2Byte2((unsigned short *)&bootclu_l16,buffer1+fl->fdi_info_t.fdi_off+26);/* 修改文件引导簇的低16位 */
		usr_write(buffer1,fl->fdi_info_t.fdi_sec,1);
		fl->EndClu = bootclu;
		/* 如果头节点中首尾簇相同那么删除头节点，否则头节点w_s_clu加1 */
        if(bootclu == ((w_buffer_t *)(fl->WRCluChainList.next))->w_e_clu)
        {
			list_del(fl->WRCluChainList.next);
        }
        else
        {
			((w_buffer_t *)(fl->WRCluChainList.next))->w_s_clu += 1;
        }
    }
	
	temp = fl->EndClu;
    if(!list_empty(&fl->WRCluChainList))
	    usr_read(buffer1,CLU_TO_FATSEC(temp),1);/* 将本节点头簇FAT所在扇区读出来 */
    /* 遍历所有的簇链节点 */
    list_for_each_safe(pos, next, &fl->WRCluChainList)
    {
		temp1 = ((w_buffer_t *)pos)->w_s_clu;
		temp2 = ((w_buffer_t *)pos)->w_e_clu;
		for(;;)
		{	
			t_clu = temp-TAKE_FAT_OFF(temp)+(PER_SECSIZE/FAT_SIZE)-1;
			*(unsigned int *)(buffer1+TAKE_FAT_OFF(temp)*4) = temp1;
			/* 前往下一节点 */
			if(temp1 == temp2) {
				usr_write(buffer1,CLU_TO_FATSEC(temp),1);
				temp = temp2;
				break;
			}
			//temp = temp1; 
			/* 到达当前FAT所能表达的最大簇号时进行下一个循环 */
			if(temp1 > t_clu){
				usr_write(buffer1,CLU_TO_FATSEC(temp),1);
				usr_read(buffer1,CLU_TO_FATSEC(temp1),1);/* 将本节点头簇FAT所在扇区读出来 */
				temp = temp1;temp1++;
				continue;//换扇区
			}
			temp = temp1;temp1++;
		}
    }
	/* 尾簇单独处理 */
	YC_FAT_ExpandCluChain(temp,0x0fffffff);
}

/* 写文件，在文件末尾追加数据 */
//对于多文件并发写入时，采用一些策略（如锁机制，信号量机制等）来优化簇的分配，确保并发写入的正确性，裸机程序不需要考虑这类情况
//除了写文件外，调用其他任何与线程安全相关的代码必须使用锁机制，裸机程序不需要考虑这类情况
/* 此函数不是最简单的编写方法，涉及大量的边界处理比较复杂，执行效率较低，设计思想却比较简单，建议不必深入研读源码，有能力的可以重写此函数或独创一种写机制 */
static int YC_WriteDataCheck(FILE1* fileInfo,unsigned char * d_buf,unsigned int len)
{
    if(NULL == fileInfo)
        return WRITE_FILE_PARAM_ERR;
    if(FILE_OPEN != fileInfo->file_state)
        return WRITE_FILE_CLOSED_ERR;
    if(0 == len)
        return WRITE_FILE_LENGTH_WARN;

    unsigned int i,j,k;
    k = 0;
    struct list_head *pos,*tmp;

    /*保证文件不大于4G*/
    if((len + fileInfo->fl_sz) < fileInfo->fl_sz)
    {
        len = FILE_MAX_SIZE - fileInfo->fl_sz;
    }
    /* 记录剩余大小 */
    unsigned int wr_size = len;unsigned int bkl = len;
	int to_alloc_num = 0;
	
    /* 计算需要的空闲簇数 */
    if(fileInfo->fl_sz == 0)/* 新文件 */
    {
        if(wr_size%(PER_SECSIZE*g_dbr[0].secPerClus))
		{
			to_alloc_num = wr_size/(PER_SECSIZE*g_dbr[0].secPerClus)+1;
		}else if(0 == wr_size%(PER_SECSIZE*g_dbr[0].secPerClus))
		{
			to_alloc_num = wr_size/(PER_SECSIZE*g_dbr[0].secPerClus);
		}
    }
	else/* 旧文件 */
	{
        if(wr_size <= fileInfo->EndCluLeftSize)
        {
            to_alloc_num = 0;
        }else {
            if((wr_size - fileInfo->EndCluLeftSize)%(PER_SECSIZE*g_dbr[0].secPerClus))
            {
                to_alloc_num = (wr_size - fileInfo->EndCluLeftSize)/(PER_SECSIZE*g_dbr[0].secPerClus)+1;
            }else if(0 == (wr_size - fileInfo->EndCluLeftSize)%(PER_SECSIZE*g_dbr[0].secPerClus))
            {
                to_alloc_num = (wr_size - fileInfo->EndCluLeftSize)/(PER_SECSIZE*g_dbr[0].secPerClus);
            }
        }
	}

    /* 预生成文件簇链 */
    if(to_alloc_num >= 1)
    {
        if(0 > YC_FAT_CreateFileCluChain(fileInfo,to_alloc_num))
        {
            /* 簇链分配失败 */
            return -1;
        }
    }

    /* 计算出需要写入多少个扇区 */
    unsigned int sec2wr,sec2wr1;
    /* 锚定尾簇内扇区偏移，扇区内字节偏移 */
    unsigned char off_sec;
    unsigned short off_byte;
	
    /* 灌数据 */ 
    if(0 == fileInfo->fl_sz)/* 新文件需要分配簇链 */
    {
		fileInfo->CurClus_R = fileInfo->FirstClu = \
        fileInfo->EndClu = ((w_buffer_t *)fileInfo->WRCluChainList.next)->w_s_clu;
        sec2wr1 = sec2wr = wr_size/PER_SECSIZE;
        if(wr_size%PER_SECSIZE){
            sec2wr ++;
        }
        list_for_each_safe(pos, tmp, &fileInfo->WRCluChainList)
        {
            if(list_empty(&fileInfo->WRCluChainList)) break;
            if(list_is_last(pos,&fileInfo->WRCluChainList))
            {
                /* 尾节点簇链单独处理 */
                i = START_SECTOR_OF_FILE(((w_buffer_t *)pos)->w_s_clu);
                j = ((w_buffer_t *)pos)->w_e_clu-((w_buffer_t *)pos)->w_s_clu+1;
                if(sec2wr1 == sec2wr)
                {
                    usr_write(d_buf+(k*PER_SECSIZE*g_dbr[0].secPerClus),i,sec2wr-k*PER_SECSIZE*g_dbr[0].secPerClus);
                }
                else
                {
                    usr_write(d_buf+(k*PER_SECSIZE*g_dbr[0].secPerClus),i,sec2wr1-k*PER_SECSIZE*g_dbr[0].secPerClus);
                    /* 剩余不足一扇区的数据 */
                    YC_Memset(buffer1,0,sizeof(buffer1));//已经写完的扇区数为 k*g_dbr[0].secPerClus+sec2wr1
                    YC_MemCpy(buffer1,d_buf+(k*PER_SECSIZE*g_dbr[0].secPerClus)+sec2wr1*PER_SECSIZE,wr_size-(k*g_dbr[0].secPerClus+sec2wr1)*PER_SECSIZE);
                    usr_write(buffer1,i+sec2wr1-k*g_dbr[0].secPerClus,1);
                }
            }
            else
            {
                /* 尾节点簇前的簇链可以全写 */
                i = START_SECTOR_OF_FILE(((w_buffer_t *)pos)->w_s_clu);
                j = ((w_buffer_t *)pos)->w_e_clu-((w_buffer_t *)pos)->w_s_clu+1;
                usr_write(d_buf+(k*PER_SECSIZE*g_dbr[0].secPerClus),i,j*g_dbr[0].secPerClus);
                k = k + j;/* 写完的簇数 */
            }
        }
    }
    else
    {
        if(to_alloc_num == 0)/* 旧文件不需要分配簇链 */
        {
            /* 起始参数 */
            off_sec = (PER_SECSIZE*g_dbr[0].secPerClus - fileInfo->EndCluLeftSize)/PER_SECSIZE;
            off_byte = (PER_SECSIZE*g_dbr[0].secPerClus - fileInfo->EndCluLeftSize)%PER_SECSIZE;
            i = START_SECTOR_OF_FILE(fileInfo->EndClu);
            if((PER_SECSIZE-off_byte)>=wr_size)/* 如果数据不足起始偏移扇区 */
            {
                /* 先将原始数据读出来 */
                usr_read(buffer1,i+off_sec,1);
                /* 将要写入的数据添加到缓冲区末尾 */
                YC_MemCpy(buffer1+off_byte,d_buf,wr_size);
                /* 重新写入数据 */
                usr_write(buffer1,i+off_sec,1);
            }
            else{
                sec2wr1 = sec2wr = (wr_size-(PER_SECSIZE-off_byte))/PER_SECSIZE;//补完一扇区后需要的额外扇区数
                if((wr_size-(PER_SECSIZE-off_byte))%PER_SECSIZE)
                    sec2wr ++;
                /*先补一扇区*/
                usr_read(buffer1,i+off_sec,1);
                YC_MemCpy(buffer1+off_byte,d_buf,PER_SECSIZE-off_byte);
                usr_write(buffer1,i+off_sec,1);

                if(sec2wr1==sec2wr)
                    usr_write(d_buf,i+off_sec+1,sec2wr);
                else
                {
                    usr_write(d_buf,i+off_sec+1,sec2wr1);
                    /* 剩余不足一扇区的数据 */
                    YC_Memset(buffer1,0,sizeof(buffer1));
                    YC_MemCpy(buffer1,d_buf+sec2wr1*PER_SECSIZE+(PER_SECSIZE-off_byte),wr_size-sec2wr1*PER_SECSIZE-(PER_SECSIZE-off_byte));
                    usr_write(buffer1,sec2wr1+i+off_sec+1,1);
                }
            }
            /* 更新文件尾簇和文件大小和文件末簇未写大小 */
            fileInfo->EndClu = TakeFileClusList_Eftv(fileInfo->EndClu);
            fileInfo->fl_sz = fileInfo->fl_sz+bkl;
            fileInfo->EndCluLeftSize = PER_SECSIZE*g_dbr[0].secPerClus-(fileInfo->fl_sz)%(PER_SECSIZE*g_dbr[0].secPerClus);
			fileInfo->left_sz += wr_size;
            if(fileInfo->EndCluLeftSize == PER_SECSIZE*g_dbr[0].secPerClus)/* 临界处理 */
                fileInfo->EndCluLeftSize = 0;			
            /* 更新文件目录项FDI中的文件大小 */
            usr_read(buffer1,fileInfo->fdi_info_t.fdi_sec,1);
            Value2Byte4((unsigned int *)&fileInfo->fl_sz,buffer1+fileInfo->fdi_info_t.fdi_off+28);
            usr_write(buffer1,fileInfo->fdi_info_t.fdi_sec,1);
            /* 无需修改簇链，直接返回即可 */
            return 0;
        }
        else/* 旧文件需要分配簇链 */
        {
            /* 起始参数 */
			if(!fileInfo->EndCluLeftSize) off_sec = 0;
			else off_sec = (PER_SECSIZE*g_dbr[0].secPerClus - fileInfo->EndCluLeftSize)/PER_SECSIZE;
			off_byte = (PER_SECSIZE*g_dbr[0].secPerClus - fileInfo->EndCluLeftSize)%PER_SECSIZE;
			if((off_sec != 0)||(off_byte != 0) )
			{
				/* 先补一扇区 */
				i = START_SECTOR_OF_FILE(fileInfo->EndClu);
				usr_read(buffer1,i+off_sec,1);
				YC_MemCpy(buffer1+off_byte,d_buf,PER_SECSIZE-off_byte);
				usr_write(buffer1,i+off_sec,1);
				/* 再将当前簇剩余扇区补满 */
				usr_write(d_buf+PER_SECSIZE-off_byte,i+off_sec+1,g_dbr[0].secPerClus-off_sec-1);
			}

            sec2wr1 = sec2wr = (wr_size-fileInfo->EndCluLeftSize)/PER_SECSIZE;
			if((wr_size-fileInfo->EndCluLeftSize)%PER_SECSIZE){
				sec2wr ++;
			}
            /* 最后处理簇链 */
            list_for_each_safe(pos, tmp, &fileInfo->WRCluChainList)
			{
                if(list_empty(&fileInfo->WRCluChainList)) break;
                if(list_is_last(pos,&fileInfo->WRCluChainList))
                {
                    /* 尾节点簇链单独处理 */
                    i = START_SECTOR_OF_FILE(((w_buffer_t *)pos)->w_s_clu);
                    j = ((w_buffer_t *)pos)->w_e_clu-((w_buffer_t *)pos)->w_s_clu+1;
                    if(sec2wr1 == sec2wr)
                    {
                        usr_write(d_buf+fileInfo->EndCluLeftSize+(k*PER_SECSIZE*g_dbr[0].secPerClus),i,sec2wr);
                    }
                    else
                    {
                        usr_write(d_buf+fileInfo->EndCluLeftSize+(k*PER_SECSIZE*g_dbr[0].secPerClus),i,sec2wr1);
                        /* 剩余不足一扇区的数据 */
                        YC_Memset(buffer1,0,sizeof(buffer1));
                        YC_MemCpy(buffer1,d_buf+fileInfo->EndCluLeftSize+(k*PER_SECSIZE*g_dbr[0].secPerClus)+sec2wr1*PER_SECSIZE,\
                                            wr_size-(k*g_dbr[0].secPerClus+sec2wr1)*PER_SECSIZE-fileInfo->EndCluLeftSize);
                        usr_write(buffer1,i+sec2wr1-k*g_dbr[0].secPerClus,1);
                    }
                }
                else
                {
                    /* 尾节点簇前的簇链可以全写 */
                    i = START_SECTOR_OF_FILE(((w_buffer_t *)pos)->w_s_clu);
                    j = ((w_buffer_t *)pos)->w_e_clu-((w_buffer_t *)pos)->w_s_clu+1;
                    usr_write(d_buf+fileInfo->EndCluLeftSize+(k*PER_SECSIZE*g_dbr[0].secPerClus),i,j*g_dbr[0].secPerClus);
                    k = k + j;/* 写完的簇数 */
                }
            }
        }
    }
	/* 缝合簇链，宁缺勿滥写法，不容易出现磁盘泄露 */
	/* 缝合簇链阶段是最容易造成磁盘损坏的阶段，唯一原因是在这个过程中设备断电 */
    YC_FAT_SewCluChain(fileInfo);

	/* 更新文件尾簇和文件大小和文件末簇未写大小 */
	fileInfo->EndClu = TakeFileClusList_Eftv(fileInfo->EndClu);
	fileInfo->fl_sz = fileInfo->fl_sz+bkl;
	fileInfo->EndCluLeftSize = PER_SECSIZE*g_dbr[0].secPerClus-(fileInfo->fl_sz)%(PER_SECSIZE*g_dbr[0].secPerClus);
	fileInfo->left_sz += wr_size;
	if(fileInfo->EndCluLeftSize == PER_SECSIZE*g_dbr[0].secPerClus)/* 临界处理 */
		fileInfo->EndCluLeftSize = 0;	
	/* 更新文件目录项FDI中的文件大小 */
	usr_read(buffer1,fileInfo->fdi_info_t.fdi_sec,1);
	Value2Byte4((unsigned int *)&fileInfo->fl_sz,buffer1+fileInfo->fdi_info_t.fdi_off+28);
	usr_write(buffer1,fileInfo->fdi_info_t.fdi_sec,1);

#if FAT2_ENABLE
    /* 备份FAT1至FAT2 */
    YC_FAT_BackedUpFAT2(fileInfo);
#endif
    /* 删除写压缩缓冲簇链，释放内存 */
    list_for_each_safe(pos, tmp, &fileInfo->WRCluChainList)
    {
        list_del(pos);
        tFreeHeapforeach((void *)pos);
    }
    INIT_LIST_HEAD(&fileInfo->WRCluChainList);
    FatInitArgs_a[0].FreeClusNum -= to_alloc_num;
    YC_FAT_UpdateFSInfo();/* 更新FSINFO扇区 */
    return 0;
}

unsigned int YC_FAT_TakeFileSize(FILE1 * fl)
{
    return fl->fl_sz;
}

int YC_FAT_flseek0(FILE1* fileInfo)
{
    if(fileInfo->file_state == FILE_OPEN)
    {
        fileInfo->CurClus_R = 1;
//        fileInfo->CurOffByte = 0;
        //fileInfo->CurOffSec = 0;
        return 0;
    }
    return 1;
}

/* 写文件 */
int YC_FAT_Write(FILE1* fileInfo,unsigned char * d_buf,unsigned int len)
{
    if(1)
	    YC_WriteDataCheck(fileInfo,d_buf,len);//追加数据
	return 0;
}

/* 查找文件，返回文件对象 */
static FILE1 YC_FAT_SeekFile(unsigned char * filepath)
{
    FILE1 file = {0};
    unsigned char fp[50];
    unsigned int file_clu = 0;
	unsigned char len1,len2;
    DelexcSpace(filepath,fp);
	len1 =  YC_StrLen(fp);
    unsigned char f_n[50] = {0};
    if(!YC_FAT_TakeFN(fp,f_n)) return file;
	len2 =  YC_StrLen(f_n);
    unsigned char f_p[50] = {0};
	if(len1-len2) YC_StrCpy_l(f_p,fp,len1-len2);
    if(!IS_FILENAME_ILLEGAL(f_n)) return file;
    /* 进入文件目录，这里假设是标准绝对路径寻找文件 */
    file_clu = YC_FAT_EnterDir(f_p);
    YC_FAT_MatchFile(file_clu,&file,f_n);
    return file;
}

/* 销毁簇链 */
static int YC_FAT_DestroyCluChain(unsigned int bootclu)
{
	/* 销毁簇链 */
    unsigned int clu = bootclu;
	unsigned int bk1,bk2,bk3;
    unsigned int *p = NULL;unsigned int *this = NULL;
    unsigned char off_fat = ((clu * FAT_SIZE) % PER_SECSIZE)/FAT_SIZE;/* 计算在FAT中的偏移（以FAT大小为单位） */
    do
    {
		unsigned int h_clu = clu-off_fat;//当前FAT表内约束1
		unsigned int t_clu = h_clu+(PER_SECSIZE/FAT_SIZE)-1;//当前FAT表内约束2
        this = p = (unsigned int *)buffer3 + off_fat;
		/* 读出段簇所在扇区数据 */
		usr_read(buffer3,CLU_TO_FATSEC(clu),1);
        bk1 = bk2 = *(unsigned int*)this;
		bk3 = clu;
		*this = 0;
        for(;;)
        {
            *this = 0;
			/* 在当前FAT扇区内逐个遍历，碰到段尾簇就寻找下一个段簇，继续读出下一段簇所在FAT扇区 */
            if( (bk1 > t_clu) || (bk1 < h_clu) )
            {
				usr_write(buffer3,CLU_TO_FATSEC(clu),1);
                clu = bk2;
                break;
            }
			bk3 = bk1 = bk2;
			p = ((unsigned int *)buffer3+TAKE_FAT_OFF(bk2));
			this = p;bk1 = bk2 = *p;
        }
		off_fat = (((unsigned int)(clu) * FAT_SIZE) % PER_SECSIZE)/FAT_SIZE;
    }while(!IS_EOF(clu));
#if YC_FAT_DEBUG
	printf("deleted file tail clu is%d\r\n",bk3);
#endif
    return bk3;
}

/* 删除文件 */
int YC_FAT_Del_File(unsigned char *file_path)
{
    if(NULL == file_path) return 0;
    FILE1 file = YC_FAT_SeekFile(file_path);
    if(file.FirstClu <= 2){
#if YC_FAT_DEBUG
		printf("需要删除的文件不存在\r\n");
#endif
		return DEL_FILE_OPENED_ERR;
	}
	if(1 == update_matchInfo(&file,2,2))
	{
#if YC_FAT_DEBUG
		printf("文件已打开，删除失败\r\n");
#endif
		return -2;
	}
    if(!file.FirstClu){
        /* 修改此文件的文件目录项的部分字段 */
        usr_read(buffer1,file.fdi_info_t.fdi_sec,1);
		*(buffer1+file.fdi_info_t.fdi_off) = 0xE5;//FDI第一个字节标记为0xE5
#if 1 /* 这一步不清楚需不需要 */
		*(buffer1+file.fdi_info_t.fdi_off+20) = *(buffer1+file.fdi_info_t.fdi_off+21) = 0;//FDI高位簇两字节标记为0x00
#endif
        usr_write(buffer1,file.fdi_info_t.fdi_sec,1);
        return 0;
    }
	/* 修改此文件的文件目录项的部分字段 */
    usr_read(buffer1,file.fdi_info_t.fdi_sec,1);
	*(buffer1+file.fdi_info_t.fdi_off) = 0xE5;//FDI第一个字节标记为0xE5
	*(buffer1+file.fdi_info_t.fdi_off+20) = *(buffer1+file.fdi_info_t.fdi_off+21) = 0;//FDI高位簇两字节标记为0x00
    usr_write(buffer1,file.fdi_info_t.fdi_sec,1);
	/* 销毁簇链 */
    YC_FAT_DestroyCluChain(file.FirstClu);
	return 0;
}

/* 重命名文件,file_name路径可以是任意路径 */
int YC_FAT_RenameFile(unsigned char *file_path,unsigned char *file_name)
{
	if(NULL == file_path) return -1;
	FILE1 file = YC_FAT_SeekFile(file_path);
	    if(file.fdi_info_t.fdi_sec == 0){
#if YC_FAT_DEBUG
		printf("需要重命名的文件不存在\r\n");
#endif
		return -1;
	}
	unsigned char fn[11] = {0};
	/* file_name预处理 */
	unsigned char fp[50] = {0};
	DelexcSpace(file_name,fp);
	unsigned char f_n[20] = {0};
	if(!YC_FAT_TakeFN(fp,f_n)) return -2;
	if(!IS_FILENAME_ILLEGAL(f_n)) return -3;
	/* 修改文件目录项中的文件名 */
    usr_read(buffer1,file.fdi_info_t.fdi_sec,1);
	Genfilename_s(f_n,fn);
    YC_StrCpy_l((unsigned char *)buffer1+file.fdi_info_t.fdi_off,fn,sizeof(fn));/* re-fill file name */
    usr_write(buffer1,file.fdi_info_t.fdi_sec,1);
	return 0;
}

/* 重命名目录,newdir路径可以是任意路径 */
int YC_FAT_RenameDir(unsigned char *dir_path,unsigned char *newdir)
{
    unsigned int dir_clu;
    unsigned char len1,len2;
    FILE1 file;
	/* dir_path预处理 */
	unsigned char dp[50] = {0};
	DelexcSpace(dir_path,dp);
    len1 = YC_StrLen(dp);
	unsigned char d_n[20] = {0};
	if(!YC_FAT_TakeFN(dp,d_n)) return -2;
    len2 = YC_StrLen(d_n);
	if(!IS_FILENAME_ILLEGAL(d_n)) return -3;

    /* 从路径匹配目录名 */
    unsigned char d_p[50] = {0};
	if(len1-len2)
		YC_StrCpy_l(d_p,dp,len1-len2);
    dir_clu = YC_FAT_EnterDir(d_p);
    if(FOUND == YC_FAT_MatchFile(dir_clu,&file,d_n))
    {
        unsigned char dp1[11] = {0};
        /* newdir预处理 */
        YC_Memset(dp,0,sizeof(dp));YC_Memset(d_n,0,sizeof(d_n));
        DelexcSpace(newdir,dp);
        if(!YC_FAT_TakeFN(dp,d_n)) return -2;
        if(!IS_FILENAME_ILLEGAL(d_n)) return -3;

        /* 修改文件目录项中的文件名 */
        usr_read(buffer1,file.fdi_info_t.fdi_sec,1);
        Genfilename_s(d_n,dp1);
        YC_StrCpy_l((unsigned char *)buffer1+file.fdi_info_t.fdi_off,dp1,sizeof(dp1));/* re-fill dir name */
        usr_write(buffer1,file.fdi_info_t.fdi_sec,1);
        return 0;
    }
#if YC_FAT_DEBUG
    printf("需要重命名的目录不存在\r\n");
#endif
	return -1;
}

#if YC_FAT_MKFS
/* 获取每簇扇区数推荐值,默认一扇区时PER_SECSIZE */
static unsigned char Get_Recommand_SecPerClu(unsigned int DiskSecNum)
{
	if(DiskSecNum<=32767) return 0;											//小于16M------->不支持
	else if((DiskSecNum>32767)&&(DiskSecNum<=65535)) return 1;				//16M-32M------->512B
	else if((DiskSecNum>65536)&&(DiskSecNum<=131071)) return 1;				//32M-64M------->512B
	else if((DiskSecNum>131071)&&(DiskSecNum<=262143)) return 2;			//64M-128M------->1K
	else if((DiskSecNum>262143)&&(DiskSecNum<=524287)) return 4;			//128M-256M------->2K
	else if((DiskSecNum>524287)&&(DiskSecNum<=16777215)) return 8;			//256M-8G------->4K
	else if((DiskSecNum>16777215)&&(DiskSecNum<=33554431)) return 16;		//8G-16G------->8K
	else if((DiskSecNum>33554431)&&(DiskSecNum<=67108864)) return 32;		//16G-32G------->16K
	else if((DiskSecNum>67108864)&&(DiskSecNum<=4294967295UL)) return 64;	//32G-2T------->32K
	else if(DiskSecNum>4294967295UL) return 0;								//大于2T------->不支持
}

/* 格式化磁盘 */
int YC_FAT_MakeFS(unsigned int DiskSecNum,enum PERCLUSZ perclusz)
{
    DBR_t *dbr;unsigned short tmp_rsvd = FS_RSVDSEC_NUM;
    unsigned int SecPerClu,temp,temp1;
    if(tmp_rsvd<1) tmp_rsvd = 32;
    /* 计算有效扇区数 */
    DiskSecNum /= NSECPERCYLINDER;
    DiskSecNum *= NSECPERCYLINDER;
    if(_DEFAULT == perclusz)
        SecPerClu = Get_Recommand_SecPerClu(DiskSecNum);
    else
        SecPerClu = perclusz/PER_SECSIZE;
    if(!SecPerClu)
        return NOTSUPPORTED_SIZE;
    unsigned int per_fatsz = GET_RCMD_FATSZ(DiskSecNum,SecPerClu);/* 每个fat表所占的扇区数 */
    /* 修改并写入dbr参数 */
    usr_clear(DBR1_SEC_OFF,1);/* DBR扇区清零 */
    YC_ConstMem_l(buffer4,temp_fs_dbr,PER_SECSIZE);
    dbr = (DBR_t *)buffer4;
    dbr->secPerClus = SecPerClu;/* 修改每簇扇区数 */
    Value2Byte4(&per_fatsz,(unsigned char *)&dbr->FATSz32);/* 修改每个fat表所占的扇区数 */
    Value2Byte2(&tmp_rsvd,(unsigned char *)&dbr->rsvdSecCnt);/* 修改保留扇区数 */
    Value2Byte4(&DiskSecNum,(unsigned char *)&dbr->totSec32);/* 修改总扇区数 */
    usr_write(buffer4,DBR1_SEC_OFF,1);
    /* FAT表格式化 */
#if FAT2_ENABLE
    usr_clear(DBR1_SEC_OFF+tmp_rsvd,2*per_fatsz);/* FAT表清零 */
#else
    usr_clear(DBR1_SEC_OFF+tmp_rsvd,per_fatsz);/* FAT表清零 */
#endif
    YC_Memset(buffer4,0,sizeof(buffer4));
    YC_ConstMem_l(buffer4,temp_fattable,sizeof(temp_fattable));/* 写入FAT表模板 */
    usr_write(buffer4,DBR1_SEC_OFF+tmp_rsvd,1);
#if FAT2_ENABLE
    usr_write(buffer4,DBR1_SEC_OFF+tmp_rsvd+per_fatsz,1);
#endif
    /* 根目录簇清零并写入模板 */
    usr_clear(DBR1_SEC_OFF+tmp_rsvd+2*per_fatsz,SecPerClu);/* 根目录清零 */
    YC_Memset(buffer4,0,sizeof(buffer4));
    YC_ConstMem_l(buffer4,temp_rootdir,sizeof(temp_rootdir));
    usr_write(buffer4,DBR1_SEC_OFF+tmp_rsvd+2*per_fatsz,1);
    /* FSINFO扇区格式化 */
    usr_clear(DBR1_SEC_OFF+1,1);/* FSINFO扇区清零 */
    YC_Memset(buffer4,0,sizeof(buffer4));
    YC_ConstMem_l(buffer4,temp_fsinfo1,sizeof(temp_fsinfo1));
    YC_ConstMem_l(buffer4+484,temp_fsinfo2,sizeof(temp_fsinfo2));
    Value2Byte4(&temp,buffer4+484);/* 修改当前分区剩余总空闲簇数 */
    Value2Byte4(&temp1,buffer4+488);/* 修改当前分区下一个空闲簇 */
    usr_write(buffer4,DBR1_SEC_OFF+1,1);
    /* 新建回收站 */
#if YC_FAT_RECYCLE
    /* 在根目录下创建回收站目录 */
#endif
	return 0;
}
#endif
/*以下代码未测*/
#if YC_FAT_CROP
/* 从尾部裁剪文件 */
int YC_FAT_FileCrop(FILE1 * fl,unsigned int len)
{
	if(!len) return 0;
	if(fl->fl_sz == 0) return 0;
	if(len == fl->fl_sz) return 0;
    int cl;
	if(len < g_dbr[0].secPerClus*PER_SECSIZE-fl->EndCluLeftSize){
		goto update_fdi;
	}
	else{
		cl = (len - g_dbr[0].secPerClus*PER_SECSIZE-fl->EndCluLeftSize)/g_dbr[0].secPerClus*PER_SECSIZE;
        if((len - g_dbr[0].secPerClus*PER_SECSIZE-fl->EndCluLeftSize)%g_dbr[0].secPerClus*PER_SECSIZE)
            cl++;
        /* 根据cl裁剪尾部簇链 */
        
        
	}
update_fdi:
    /* 更新一些内存参数 */
    fl->fl_sz = fl->fl_sz - len;
    /* 修改FDI文件大小参数 */
    usr_read(buffer4,fl->fdi_info_t.fdi_off,1);
    Value2Byte4(&fl->fl_sz,buffer4+fl->fdi_info_t.fdi_sec+28);/* 修改文件大小 */
    usr_write(buffer4,fl->fdi_info_t.fdi_off,1);
    /* 更新FSINFO */
    
    return 0;
}
#endif

/* 挂载文件系统 */
/* 参数1：驱动号 参数2：硬件设备 */
/* 本质是把物理层面上的硬件磁盘虚拟化成扇区结构的软件层面上的数据块 */
/* 重要说明：用户在对文件进行读写等操作之前，只需要挂载就可以了 */
/* 例如：
    int main() 
    { 
        ioopr_t  ioopr1; 
        ioopr1.DeviceOpr_WR = SDCardWrite;
        ioopr1.DeviceOpr_RD = SDCardRead;
        ioopr1.DeviceOpr_CLR = SDCardErase;

        ioopr_t  ioopr2;
        ioopr2.DeviceOpr_WR = NorFlashWrite;
        ioopr2.DeviceOpr_RD = NorFlashRead;
        ioopr2.DeviceOpr_CLR = NorFlashErase;
        ......(硬件初始化)......
        YC_FAT_Mount("C盘",&ioopr1,0);
        YC_FAT_Mount("JYC的U盘",&ioopr1,1);
        (挂载完了。下面就可以进行文件读写了)
        read operations;
        write operations;
        ......
    } 
*/
/* 如果你的设备比如SD卡已经在电脑上格式化成FAT32了，那么YC_FAT_Mount第三个参数就传0，防止数据丢失 */
/* 如果你的设备比如SD卡还不是FAT32格式，那么YC_FAT_Mount第三个参数就传1 */
int YC_FAT_Mount(unsigned char *drvn,ioopr_t *usrdev,char if_mkfs)
{
	if(NULL == drvn) return -1;
	int a = YC_StrLen(drvn);
	if((a==0)||(a>YC_FAT_PERDDN_MAXSZIE)) return -2;
	ycfat_t * fatobj;
	if(NULL == (fatobj = (ycfat_t *)tAllocHeapforeach(sizeof(ycfat_t)))) return -1;
	YC_Memset((unsigned char *)&fatobj->mountNode,0,sizeof(ycfat_t));
	YC_StrCpy_l((unsigned char*)fatobj->ddn,drvn,a);
	fatobj->DirOpr_Create = YC_FAT_CreateDir;
	fatobj->DirOpr_Enter = YC_FAT_UsrEnterDir;
	fatobj->DirOpr_GetCWD = YC_FAT_GetCurWorkDir;
	fatobj->DirOpr_Rename = YC_FAT_RenameDir;
	fatobj->fileOpr_Close = YC_FAT_Close;
	fatobj->fileOpr_Create = YC_FAT_CreateFile;
	fatobj->fileOpr_Del = YC_FAT_Del_File;
	fatobj->fileOpr_Open = YC_FAT_OpenFile;
	fatobj->fileOpr_Rename = YC_FAT_RenameFile;
	fatobj->fileOpr_Write = YC_FAT_Write;
	fatobj->fsOpr_Init = YC_FAT_Init;
#if YC_FAT_CROP
	fatobj->fileOpr_Crop = YC_FAT_FileCrop;
#endif
#if YC_FAT_MKFS
	fatobj->diskOpr_Format = YC_FAT_MakeFS;
#endif
	/* 添加至挂载链 */
	if(!fatobjNodeNum)/* 如果挂载链为空 */
		INIT_LIST_HEAD(&ycfatBlockHead);
	list_add_tail((struct list_head *)&fatobj->mountNode,&ycfatBlockHead);
	fatobjNodeNum ++;
	/* 用于回调 */
	fatobj->ioopr.DeviceOpr_WR = usrdev->DeviceOpr_WR;
	fatobj->ioopr.DeviceOpr_RD = usrdev->DeviceOpr_RD;
	fatobj->ioopr.DeviceOpr_CLR = usrdev->DeviceOpr_CLR;
    /* 大小端检测 */
    endian_checker();
#if YC_FAT_MKFS
	if(if_mkfs)
        fatobj->diskOpr_Format(114514,114514);
#endif
	/* 初始化文件系统 */
	if(0 == fatobj->fsOpr_Init(fatobj)) fatobj->hay = MOUNT_SUCCESS;/* 标记成功挂载 */
	else fatobj->hay = FAULTY_DISK;/* 标记坏盘 */
	return 0;
}

/* 卸载文件系统 */
int YC_FAT_Unmount(unsigned char *drvn)
{
	/* 从挂载链删除 */
	struct list_head *pos;
	if(NULL != (pos = YC_FAT_MatchDdn(drvn))){
		list_del(pos);
		tFreeHeapforeach((void *)pos);
		fatobjNodeNum --;
	}
	return 0;
}
#if YC_FAT_ENCODE

// 定义一个枚举
enum CharacterEncoding  {
    ASCII,
    UTF_8,
    GBK
};
/* 以下是关于字符编码的一些处理 */
typedef enum CHARA_ENCODING_SET 
{
    E_UTF8 = 0,
    E_GB2312,
    E_GBK,
}Char_sets_t;

/* 用来匹配Unicode字符，一般中文字符的Utf8编码为3字节 */
/* 定义了一个UTF-8到GBK的映射表 */
typedef J_UINT32 (*BufToUint_t)(J_UINT8 *);
J_UINT32 BEBufToUint32(J_UINT8 *_pBuf)
{
    return (((J_UINT32)_pBuf[0] << 24) | ((J_UINT32)_pBuf[1] << 16) | ((J_UINT32)_pBuf[2] << 8) | _pBuf[3]);
}
J_UINT32 BEBufToUint24(J_UINT8 *_pBuf)
{
    return  ((J_UINT32)_pBuf[0] << 16) | ((J_UINT32)_pBuf[1] << 8) | _pBuf[2];
}
J_UINT32 BEBufToUint16(J_UINT8 *_pBuf)
{
    return (((J_UINT16)_pBuf[0] << 8) | _pBuf[1]);
}
J_UINT32 BEBufToUint8(J_UINT8 *_pBuf)
{
    return _pBuf[0];
}

static BufToUint_t b2val_table[] = { NULL,\
    BEBufToUint8,\
    BEBufToUint16,\
    BEBufToUint24,\
    BEBufToUint32,\
    NULL,\
    NULL\
};

/* 二分查表 */
/* table：表起始地址 */
/* unit_total_num：单元数量即表共多少行 */
/* per_unit_size：每单元大小表每一行字节数*/
/* per_unit_off：从每单元偏移多少开始查*/
/* data_size：被查单元大小，就是一次查几个字节*/
/* mode：查询的表是大端还是小端 */
static int MappingTableSearch(unsigned int NumToSearch,const unsigned char *table,unsigned int unit_total_num,\
unsigned char per_unit_size,unsigned char per_unit_off,unsigned char data_size)
{
    if(per_unit_off + data_size > per_unit_size) return -1;/* 混叠错误 */
    if( (data_size < 1) || (data_size > 4) )
        return -2;

    int b1 = 0,b2 = unit_total_num-1;/* 最小和最大索引 */
    int index;int k = 0;/* 当前索引和索引次数 */
    unsigned int offb,index_val;/* 偏移字节和索引值 */
    while(b1<=b2){
        /* 最多查32次强制退出，因为到第33次已经排除40多亿数据了 */
        if(k > 32) return -3;
        k++;
        index = b1+(b2-b1)/2;
#if YC_FAT_DEBUG
        printf("当前索引是 %d(以每单元大小为单位)\r\n",index);
#endif
        offb = index*per_unit_size+per_unit_off;
        index_val = b2val_table[data_size]( (unsigned char *)table + offb );/* 计算索引值 */
        if( index_val < NumToSearch ) 
            b1 = index+1;
        else if( index_val > NumToSearch ) 
            b2 = index-1;
        if(index_val == NumToSearch){
#if YC_FAT_DEBUG
            printf("第%d个字节是需要查找的值, 共%d个字节 ",offb,data_size);
            printf("最终索引值是 %d\r\n",NumToSearch);
#endif
            return index;
        }
    }
    return -4;
}

bool YC_FAT_IsTextAscii(unsigned char *pbuf)
{
    return ((*pbuf >= 0) && (*pbuf <= 0X7F));
}

/* 检查文本是不是utf8或gbk编码，如果是其他编码可能会出错 */
/* 此功能有很大的漏洞 */
static bool IsTextUTF8_Uncarefully(const unsigned char *str, int strlength)
{
#ifndef OS_WINDOWS
    typedef unsigned int DWORD;
    typedef unsigned char UCHAR;
#endif
    int i;int j = 0,k = 0;
    DWORD nBytes = 0;
    UCHAR chr;
    bool isUTF8 = true; 
    for(i = 0; i < strlength; i++)
    {
        chr = (UCHAR) * (str + i);
        if(nBytes == 1)
        {
            if( (chr & 0x80) == 0 ){/* 如果是ASCII码 */
                    k = i;
                break;/* k是第二个ASCII码段的第一个ASCII码的索引 */
            }
            if(i == strlength - 1) k = strlength;
        }
        else{
            if( (chr & 0x80) == 0 ) /* 如果是ASCII码 */
                continue;
            else{
                j = i;nBytes = 1;/* j是第一个不是ASCII码的索引 */
            }
        }
    }
    if(k-j)
    {
        /* 保证至少有6个字节的数据参与检测 */
        if((k-j)%6 == 0)
        {
            if ( ((*(str + j) & 0xE0) == 0xE0) && ((*(str + j + 3 ) & 0xE0) == 0xE0) );
            else{
                isUTF8 = false;/* GBK */
            }
        }
        else if((k-j)%2 == 0) {
            isUTF8 = false;/* GBK */
        }
    }
    return isUTF8;
}

#define U2G_TBUNIN (sizeof(utf8togbk_table)/5)
//#define G2U_TBUNIN (sizeof(gbk2utf8_table)/5)
/* 一个UTF8编码的Unicode字符转GBK字符 */
/* 返回编码后的GBK字符长度 */
static char YC_FAT_utf8_to_gbk(unsigned char *p_uni,unsigned short *p_gbk)
{
    *p_gbk = 0;
    if(YC_FAT_IsTextAscii(p_uni)) {
        *(unsigned char *)p_gbk = *p_uni;
        return 1;
    }
    else
    {
        /* 如果不是ASCII码就默认是2字节长度的GBK汉字或符号 */
        unsigned int uni_val = BEBufToUint24(p_uni);/* 转成大端格式 */
        int index;
        if(0 > (index = MappingTableSearch(uni_val,utf8togbk_table,U2G_TBUNIN,5,0,3)))
        {
            /* 没查到 */
            return 0;
        }
        else
        {
            *(unsigned char *)p_gbk = *(utf8togbk_table+index*5+3);
            *((unsigned char *)p_gbk+1) = *(utf8togbk_table+index*5+4);
            return 2;
        }
    }
}
/* 一个gbk字符转UTF8编码的Unicode字符 */
/* 返回编码后的unicode字符长度 */
static char YC_FAT_gbk_to_utf8(unsigned char *s_gbk,unsigned int *t_uni)
{
    *t_uni = 0;
    if(YC_FAT_IsTextAscii(s_gbk)) {
        *(unsigned char *)t_uni = *s_gbk;
        return 1;
    }
    else
    {
//        unsigned int gbk_val = BEBufToUint16(s_gbk);/* 转成大端格式 */
//        /* 在gbk--->utf8表中查找对应的utf8编码 */
//        int index;
//        if(0 > (index = MappingTableSearch(gbk_val,gbk2utf8_table,G2U_TBUNIN,5,0,2)))
//        {
//            /* 没查到 */
//            return 0;
//        }
//        else
//        {
//            *(unsigned char *)t_uni = *(gbk2utf8_table+index*5+2);
//            *((unsigned char *)t_uni+1) = *(gbk2utf8_table+index*5+3);
//            *((unsigned char *)t_uni+2) = *(gbk2utf8_table+index*5+4);
//            return 3;
//        }
    }
}

/* 单UTF-8编码转Unicode字符 */
/* 返回可能的Unicode长度，备用 */
char YC_FAT_Utf8toUni(J_UINT8 * unic,unic32 * uecd)
{
    * uecd = 0;
    J_UINT8 *ufst = unic;
    unsigned char * rp = (unsigned char *)uecd;
    /* 解码逻辑看字符编码相关的逻辑图 */
    /* 判断第一个字节是否可能为Utf-8编码 */
    if(*ufst >= 0xF0)
    {
        * rp = ( ( *(ufst+2) << 6 ) & 0xC0 ) | ( *(ufst+3) & 0x3F );
		*(rp+1) = ((*(ufst+2) >> 2) & 0x0F) | ( (*(ufst+1) << 4) & 0xF0 );
        *(rp+2) = ( (*ufst << 2) & 0x1C ) | ( (*(ufst+1) >> 4) & 0x03 );
		return 4;
    }
    else if(*ufst >= 0xE0)
    {
        * rp = ( ( *(ufst+1) << 6 ) & 0xC0 ) | ( *(ufst+2) & 0x3F );
        *(rp+1) = ((*(ufst+1) >> 2) & 0x0F) | (*ufst << 4);
		return 2;
    }
    else if(*ufst >= 0xC0)
    {
        *(rp+1) = (*ufst >> 2) & 0x07;
        *rp = ( (*ufst << 6) & 0xC0 ) | ( *(ufst+1) & 0x3F );
        return 2;
    }
    else if(*ufst <= 0x7F)
    {
        *rp = *ufst;
        return 1;
    }
    /* 这里应该加一个逆向的过程，防止错码 */
    else return 0;
}

/* 将Unicode字符编码成UTF-8 */
/* pUtf8至少预留4字节大小空间 */
char YC_FAT_Uni2Utf8(unsigned int Uni,void *pUtf8)
{
    if(pUtf8 == NULL) return -1;
    unsigned char *p = pUtf8;
    if( (Uni > 0x00000000) && (Uni <= 0x0000007F) )
    {
        *p = Uni & 0x7F;
        return 1;
    }
    else if( (Uni >= 0x00000080) && (Uni <= 0x000007FF) )
    {
		*p = (Uni >> 6) & 0x1F;
        *p = *p | 0xC0;
		*(p+1) = Uni & 0x3F;
        *(p+1) = *(p+1) | 0x80;
        return 2;
    }
    else if( (Uni >= 0x00000800) && (Uni <= 0x0000FFFF) )
    {
		*p = (Uni >> 12) & 0x0F;
        *p = *p | 0xE0;
        *(p+1) =  (Uni >> 6) & 0x3F;
        *(p+1) = *(p+1) | 0x80;
		*(p+2) = Uni & 0x3F;
        *(p+2) = *(p+2) | 0x80;
        return 3;
    }
    /* 最大支持到0x10ffff */
    else if( (Uni >= 0x00010000) && (Uni <= 0x0010FFFF) )
    {
		*p = (Uni >> 18) & 0x07;
        *p = *p | 0xF0;
		*(p+1) = (Uni >> 12) & 0x3F;
        *(p+1) = *(p+1) | 0x80;
        *(p+2) = (Uni >> 6) & 0x3F;
        *(p+2) = *(p+2) | 0x80;
        *(p+3) = Uni & 0x3F;
        *(p+3) = *(p+3) | 0x80;
        return 4;
    }else return 0;
}

/* 用户中间变量转ascii,返回ascii长度 */
char YC_FAT_int_to_str(int _iNumber, char *_pBuf)
{
    char i = 0,j = 0,k;
    int bk = _iNumber;
    /* 用于位匹配的字符串 */
    char *matchtable = "0123456789";
    if(!_iNumber) {
        _pBuf[0] = matchtable[0];
        return 1;
    }
    if(_iNumber < 0){
         _pBuf[i] = '-'; i++;
         _iNumber = -_iNumber;
    }
    for(;_iNumber;i++,_iNumber /= 10)
    {
        j = _iNumber%10;
        _pBuf[i] = matchtable[j];
    }
    if(bk<0) {j = (i-1)/2;for(k = 0;k<j;k++) SWAP_TWO_BYTES(_pBuf+k+1,_pBuf+i-1-k);}
    else {j = i/2;for(k=0;k<j;k++) SWAP_TWO_BYTES(_pBuf+k,_pBuf+i-1-k);}
    return i;
}

/* 向文件追加字符串,指定编码方式，这个过程执行效率较低,平均每个字符写入耗时约1ms */
int YC_FAT_puts(FILE1 *file,const unsigned char * str,Char_sets_t Encode_mode)
{
    int str_len = YC_StrLen((unsigned char *)str);
    int index = str_len;char once_len = 0;
    unsigned char * to_ecd;
    bool utf8 = IsTextUTF8_Uncarefully(str,str_len);
    if(utf8){
        /* 如果是utf8编码 */
        if(E_UTF8 == Encode_mode){
            YC_FAT_Write(file,(unsigned char *)str,str_len);/* 直接写入 */
        }else{
            unsigned short e_gbk;index = 0;
            /* UTF8--->GBK */
            while(str_len - index)
            {
                if(0 == (once_len = YC_FAT_utf8_to_gbk((unsigned char *)(str+index),&e_gbk)))
                    break;
                index = index + ((2 == once_len)?3:1);
                YC_FAT_Write(file,(unsigned char *)&e_gbk,once_len);
            }
        }
    }else{
        /* 文本是GBK编码 */
        if((E_GB2312 == Encode_mode) || (E_GBK == Encode_mode)){
            YC_FAT_Write(file,(unsigned char *)str,str_len);/* 直接写入 */
        }else{
            unsigned int e_unic;index = 0;
            /* GBK--->UTF8 */
//            while(str_len - index)
//            {
//                if(0 == (once_len = YC_FAT_gbk_to_utf8((unsigned char *)(str+index),&e_unic)))
//                    break;
//                index = index + ((3 == once_len)?2:1);
//                YC_FAT_Write(file,(unsigned char *)&e_unic,once_len);
//            }
        }
    }
	return (str_len-index);/* 返回未写入的大小，如果返回0就表示全写入成功了 */
}
//{
//	YC_FAT_Init();
//	YC_FAT_CreateFile((unsigned char *)"./5.TXT");
//	YC_FAT_OpenFile(&file1,(unsigned char *)"./5.TXT");
//	YC_FAT_CreateFile((unsigned char *)"./6.TXT");
//	YC_FAT_OpenFile(&file2,(unsigned char *)"./6.TXT");
//	printf("writing character set... now\r\n");
////	for(int i = 0;i<5000;i++){
////		if(i%100 == 0)
////			printf("i = %d\r\n",i);
////		YC_FAT_Write(&file1,d_buff,sizeof(d_buff));
////	}
//	if (0<YC_FAT_puts(&file1,"我是神里绫华的狗",E_UTF8))
//		printf("file1 write err\r\n");
//	if (YC_FAT_puts(&file2,"我是神里绫华的狗",E_GBK))
//		printf("file2 write err\r\n");
//	printf("writing ok\r\n");
//	YC_FAT_Close(&file1);
//	YC_FAT_Close(&file2);
//}
#endif

