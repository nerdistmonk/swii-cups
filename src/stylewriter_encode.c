#include <stddef.h>

#include "stylewriter.h"

#define MAX_RUN 0x3E
#define MAX_BLOCK 0x3E
#define RUN_THRESH 0x01
#define DATA_WHITE 0x00
#define DATA_BLACK 0xFF
#define DATA_OTHER 0x0A
#define MASK_RUNWHT 0x80
#define MASK_RUNBLK 0xC0

size_t swii_encode_row(const unsigned char *src, size_t srcLen,
		unsigned char *dst, size_t rowBytes)
{
	size_t i, s = 0, d = 0;

	for(i = 0; i < srcLen && src[i] == DATA_WHITE; i++)
		;
	if(i == srcLen)
	{
		dst[0] = MASK_RUNWHT;
		return 1;
	}

	while(s < srcLen)
	{
		size_t runStart = 0, runLen = 0;
		unsigned char runChar = DATA_OTHER;

		for(i = s; i < srcLen; i++)
		{
			if(runChar == DATA_WHITE || runChar == DATA_BLACK)
			{
				if(src[i] != runChar)
				{
					if(i - runStart >= RUN_THRESH)
						break;
					runChar = DATA_OTHER;
				}
				else if(i - runStart >= MAX_RUN)
					break;
			}
			else if(src[i] == DATA_WHITE || src[i] == DATA_BLACK)
			{
				runChar = src[i];
				runStart = i;
			}
			else if(i - s >= MAX_BLOCK)
				break;
		}

		if(runChar != DATA_OTHER)
			runLen = i - runStart;
		else
		{
			runStart = i;
			runLen = 0;
		}

		if(runStart != s)
		{
			dst[d++] = (unsigned char)(runStart - s);
			while(s < runStart)
				dst[d++] = src[s++];
		}

		if(runLen > 0)
		{
			if(runChar == DATA_BLACK)
				dst[d++] = (unsigned char)(MASK_RUNBLK + runLen);
			else
			{
				/* A white run reaching the end is left to the padding. */
				if(s + runLen < srcLen)
					dst[d++] = (unsigned char)(MASK_RUNWHT + runLen);
				else
					break;
			}
			s += runLen;
		}
	}

	while(s < rowBytes)
	{
		size_t padLen = rowBytes - s;
		if(padLen > MAX_RUN)
			padLen = MAX_RUN;
		dst[d++] = (unsigned char)(MASK_RUNWHT + padLen);
		s += padLen;
	}

	return d;
}
