#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/stylewriter.h"

#define ROW_BYTES 360

/*
 * Decode the StyleWriter run-length format. A row must decode to exactly
 * rowBytes, except the single 0x80 byte that encodes a blank row.
 */
static int decode_row(const unsigned char *in, size_t inLen,
		unsigned char *out, size_t rowBytes)
{
	size_t i = 0, o = 0;

	memset(out, 0, rowBytes);
	while(i < inLen)
	{
		unsigned c = in[i++];
		size_t n;

		if(c < 0x80)
		{
			if(i + c > inLen || o + c > rowBytes)
				return -1;
			memcpy(out + o, in + i, c);
			i += c;
			o += c;
		}
		else if(c < 0xC0)
		{
			n = c - 0x80;
			if(o + n > rowBytes)
				return -1;
			o += n;
		}
		else
		{
			n = c - 0xC0;
			if(o + n > rowBytes)
				return -1;
			memset(out + o, 0xFF, n);
			o += n;
		}
	}
	if(o == rowBytes)
		return 0;
	return (inLen == 1 && in[0] == 0x80 && o == 0) ? 0 : -1;
}

static int check_row(const char *name, const unsigned char *src, size_t srcLen,
		size_t rowBytes)
{
	unsigned char *enc = malloc(2 * swii_encode_max(srcLen, rowBytes));
	unsigned char *dec = malloc(rowBytes);
	unsigned char *want = calloc(rowBytes, 1);
	size_t n, m;
	int failed = 0;

	if(!enc || !dec || !want)
	{
		fprintf(stderr, "out of memory\n");
		exit(2);
	}

	memcpy(want, src, srcLen);
	n = swii_encode_row(src, srcLen, enc, rowBytes);
	if(n > swii_encode_max(srcLen, rowBytes) ||
			decode_row(enc, n, dec, rowBytes) != 0 ||
			memcmp(dec, want, rowBytes) != 0)
	{
		fprintf(stderr, "FAIL: %s (srcLen %zu, encoded %zu)\n",
				name, srcLen, n);
		failed = 1;
	}

	/* Row lengths must match the encoder even when another row follows. */
	m = swii_encode_row(src, srcLen, enc + n, rowBytes);
	if(swii_row_len(enc, n + m, rowBytes) != n ||
			swii_row_len(enc + n, m, rowBytes) != m)
	{
		fprintf(stderr, "FAIL: %s row length (srcLen %zu)\n", name, srcLen);
		failed = 1;
	}

	free(enc);
	free(dec);
	free(want);
	return failed;
}

int main(void)
{
	unsigned char row[ROW_BYTES];
	unsigned char blank[ROW_BYTES];
	size_t i, k, len;
	int failures = 0;

	memset(blank, 0, sizeof blank);
	failures += check_row("blank row", blank, ROW_BYTES, ROW_BYTES);
	failures += check_row("empty source", blank, 0, ROW_BYTES);

	memset(row, 0xFF, sizeof row);
	failures += check_row("all black", row, ROW_BYTES, ROW_BYTES);

	/* Run boundaries: exactly MAX_RUN (0x3E) and one past it. */
	for(len = 61; len <= 64; len++)
	{
		memset(row, 0, sizeof row);
		memset(row + 10, 0xFF, len);
		failures += check_row("black run length", row, ROW_BYTES, ROW_BYTES);

		memset(row, 0x55, sizeof row);
		memset(row + 20, 0x00, len);
		failures += check_row("white run length", row, ROW_BYTES, ROW_BYTES);

		memset(row, 0x55, sizeof row);
		failures += check_row("literal block", row, len, ROW_BYTES);
	}

	/* A white run reaching the end of the source is left to the padding. */
	row[0] = 0x55;
	memset(row + 1, 0, 3);
	failures += check_row("trailing white", row, 4, 4);
	failures += check_row("trailing white padded", row, 4, ROW_BYTES);

	/* Alternating bytes make many short runs and literals. */
	for(i = 0; i < sizeof row; i++)
		row[i] = (i & 1) ? 0xFF : 0x00;
	failures += check_row("alternating", row, ROW_BYTES, ROW_BYTES);

	/* Random rows that mix literals and runs, including shorter widths. */
	srand(1);
	for(k = 0; k < 2000; k++)
	{
		size_t srcLen = (size_t)rand() % (ROW_BYTES + 1);
		for(i = 0; i < srcLen;)
		{
			size_t runLen = 1 + (size_t)rand() % 70;
			unsigned char value;
			int kind = rand() % 3;

			value = kind == 0 ? 0x00 : kind == 1 ? 0xFF : (unsigned char)(1 + rand() % 254);
			for(; runLen > 0 && i < srcLen; runLen--, i++)
				row[i] = value;
		}
		failures += check_row("random row", row, srcLen, ROW_BYTES);
	}

	if(failures)
	{
		fprintf(stderr, "%d encoder check(s) failed\n", failures);
		return 1;
	}
	printf("encoder round trip: all checks passed\n");
	return 0;
}
