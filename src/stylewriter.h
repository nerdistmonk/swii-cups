#ifndef STYLEWRITER_H
#define STYLEWRITER_H

#include <stddef.h>

/*
 * Frames sent from the CUPS filter (stdout) to the CUPS backend (stdin).
 * Each frame is: 1 byte type, 4 bytes little-endian payload length, payload.
 */
#define SWF_HEADER_SIZE 5

/* Raw bytes for the printer. No reply is read. */
#define SWF_WRITE 'W'
/* One byte command. Sent as FF FF FF cmd and the reply is discarded. */
#define SWF_QUERY 'Q'
/* LE32 bits per second. Drain output, then reconfigure to 8N1 at that rate. */
#define SWF_SPEED 'S'
/* Empty payload. Eject, reset the printer, and wait until it is ready. */
#define SWF_RESET 'I'
/* Band: SWF_BAND_HDR_SIZE header bytes followed by encoded row data. */
#define SWF_BAND 'B'
/* 3 bytes: poll flag (1 = poll status '1' after the form feed), drain code, flags. */
#define SWF_PAGEEND 'P'
/* Empty payload. Eject and fail the job. */
#define SWF_ABORT 'A'

/* Band header: u8 drain code, u8 flags, LE16 top, LE16 bottom, LE16 right. */
#define SWF_BAND_HDR_SIZE 8
#define SWF_BAND_COLOR 0x01 /* 'c' rectangle and KCMY data instead of 'R' */
#define SWF_BAND_FIRST 0x02 /* first band of a page: check for paper-out */
#define SWF_BAND_WAITPAPER 0x04 /* color model: wait in place for paper mid-page */

static inline void swf_put_le16(unsigned char *p, unsigned long v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
}

static inline void swf_put_le32(unsigned char *p, unsigned long v)
{
	p[0] = v & 0xFF;
	p[1] = (v >> 8) & 0xFF;
	p[2] = (v >> 16) & 0xFF;
	p[3] = (v >> 24) & 0xFF;
}

static inline unsigned long swf_get_le16(const unsigned char *p)
{
	return p[0] | ((unsigned long)p[1] << 8);
}

static inline unsigned long swf_get_le32(const unsigned char *p)
{
	return p[0] | ((unsigned long)p[1] << 8) |
		((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/*
 * Run-length encode one row for the StyleWriter. Rows are XORed against the
 * previous row by the caller when delta coding. The output is padded with
 * white to rowBytes. dst must hold at least swii_encode_max(srcLen, rowBytes)
 * bytes. Returns the number of bytes written.
 */
size_t swii_encode_row(const unsigned char *src, size_t srcLen,
		unsigned char *dst, size_t rowBytes);

/* Upper bound on the output of swii_encode_row. */
static inline size_t swii_encode_max(size_t srcLen, size_t rowBytes)
{
	return 2 * (srcLen + rowBytes) + 16;
}

#endif
