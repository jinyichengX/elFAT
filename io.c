#include "sd_card.h"

extern stc_sd_handle_t stcSdhandle;

void usr_read(void * buffer,unsigned int SecIndex,unsigned int SecNum)
{
	if(0 == SecNum)
		return;
	SDCARD_ReadBlocks(&stcSdhandle, SecIndex, SecNum, (uint8_t *)buffer,100000);
}

void usr_write(void * buffer,unsigned int SecIndex,unsigned int SecNum)
{
	if(0 == SecNum)
		return;
	SDCARD_WriteBlocks(&stcSdhandle, SecIndex, SecNum, (uint8_t *)buffer,100000);
}

void usr_write(void * buffer,unsigned int SecIndex,unsigned int SecNum)
{
	SDCARD_WriteBlocks(&stcSdhandle, SecIndex, SecNum, (uint8_t *)buffer,100000);
}

void usr_clear(unsigned int SecIndex,unsigned int SecNum)
{

}