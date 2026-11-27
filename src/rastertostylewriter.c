/*
 * CUPS raster filter for the Apple StyleWriter family.
 *
 * Reads CUPS raster pages (K, or banded KCMY, 1 bit per color) and writes
 * StyleWriter frames (see stylewriter.h) to stdout for the stylewriter backend.
 * The backend owns the serial port; this filter never touches the device.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cups/cups.h>
#include <cups/raster.h>

#include "stylewriter.h"

/* Band height used by the Mac driver's traces. */
#define SWII_BAND_ROWS 256

enum swii_model
{
	MODEL_SW1,
	MODEL_SW2,
	MODEL_SW3,
	MODEL_SW1500,
	MODEL_SW2200,
	MODEL_SW2400,
	MODEL_SW2500
};

struct swii_options
{
	enum swii_model model;
	int colorCartridge;
	int quality;
};

struct swii_geometry
{
	long printWidth;
	long leftMargin;
	long topMargin;
	long bottomMargin;
};

struct swii_page
{
	unsigned long width;
	unsigned long height;
	unsigned long bytesPerLine;
	unsigned long stride;	/* bytes per color plane within a line */
	int planes;		/* 1 for K, 4 for KCMY */
	unsigned char *data;
};

struct swii_layout
{
	long top;		/* first printed input row */
	long end;		/* one past the last printed input row */
	long leftBytes;		/* bytes trimmed from the left of each plane */
	long printwidth;	/* bytes of each plane that are sent */
	long printRowBytes;	/* encoded row length, padded to this */
	long printWidth;	/* pixels sent to the printer */
	int planes[4];		/* raster plane index for each encoded plane */
	int nPlanes;
	int delta;		/* XOR each row against the previous row */
	int color;		/* bands carry KCMY data */
	int combine;		/* OR every ink plane into K (Black cartridge) */
	int flags;
	int poll;
};

static const struct
{
	const char *name;
	enum swii_model model;
} model_names[] = {
	{"SW1", MODEL_SW1},
	{"SW2", MODEL_SW2},
	{"SW3", MODEL_SW3},
	{"SW1500", MODEL_SW1500},
	{"SW2200", MODEL_SW2200},
	{"SW2400", MODEL_SW2400},
	{"SW2500", MODEL_SW2500}
};

static int is_color_model(enum swii_model m)
{
	return m == MODEL_SW1500 || m == MODEL_SW2200 ||
		m == MODEL_SW2400 || m == MODEL_SW2500;
}

static int put_all(const void *buf, size_t len)
{
	const unsigned char *p = buf;

	while(len > 0)
	{
		ssize_t n = write(STDOUT_FILENO, p, len);
		if(n < 0)
		{
			if(errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/* Sends one frame whose payload is a followed by b. */
static int send_frame(int type, const void *a, size_t alen,
		const void *b, size_t blen)
{
	unsigned char hdr[SWF_HEADER_SIZE];

	hdr[0] = (unsigned char)type;
	swf_put_le32(hdr + 1, alen + blen);
	if(put_all(hdr, sizeof hdr) < 0)
		return -1;
	if(alen > 0 && put_all(a, alen) < 0)
		return -1;
	if(blen > 0 && put_all(b, blen) < 0)
		return -1;
	return 0;
}

static int frame_text(const char *s)
{
	return send_frame(SWF_WRITE, s, strlen(s), NULL, 0);
}

static int frame_query(unsigned char cmd)
{
	return send_frame(SWF_QUERY, &cmd, 1, NULL, 0);
}

static int ppd_default(const char *path, const char *key,
		char *value, size_t valueLen)
{
	char prefix[128];
	char line[512];
	size_t plen;
	int found = 0;
	FILE *f;

	snprintf(prefix, sizeof prefix, "*Default%s:", key);
	plen = strlen(prefix);

	f = fopen(path, "r");
	if(!f)
		return 0;
	while(fgets(line, sizeof line, f))
	{
		if(strncmp(line, prefix, plen) == 0)
		{
			char *p = line + plen;
			size_t n;

			while(*p == ' ' || *p == '\t')
				p++;
			n = strcspn(p, " \t\r\n");
			if(n > 0 && n < valueLen)
			{
				memcpy(value, p, n);
				value[n] = '\0';
				found = 1;
			}
			break;
		}
	}
	fclose(f);
	return found;
}

static void get_option(int num, cups_option_t *opts, const char *ppd,
		const char *key, const char *fallback, char *out, size_t outLen)
{
	const char *v = cupsGetOption(key, num, opts);

	if(v)
		snprintf(out, outLen, "%s", v);
	else if(!ppd || !ppd_default(ppd, key, out, outLen))
		snprintf(out, outLen, "%s", fallback);
}

static int load_options(const char *optstr, struct swii_options *o)
{
	cups_option_t *opts = NULL;
	int num = cupsParseOptions(optstr, 0, &opts);
	const char *ppd = getenv("PPD");
	char model[32], cart[32], quality[16];
	size_t i;
	int found = 0;

	get_option(num, opts, ppd, "Model", "SW2", model, sizeof model);
	get_option(num, opts, ppd, "InkCartridge", "Black", cart, sizeof cart);
	get_option(num, opts, ppd, "Quality", "1", quality, sizeof quality);
	cupsFreeOptions(num, opts);

	for(i = 0; i < sizeof model_names / sizeof model_names[0]; i++)
	{
		if(strcmp(model, model_names[i].name) == 0)
		{
			o->model = model_names[i].model;
			found = 1;
		}
	}
	if(!found)
	{
		fprintf(stderr, "ERROR: Unknown Model \"%s\"\n", model);
		return -1;
	}

	if(strcmp(cart, "Color") == 0)
		o->colorCartridge = 1;
	else if(strcmp(cart, "Black") == 0)
		o->colorCartridge = 0;
	else
	{
		fprintf(stderr, "ERROR: Unknown InkCartridge \"%s\"\n", cart);
		return -1;
	}

	if(strcmp(quality, "0") == 0 || strcmp(quality, "1") == 0 ||
			strcmp(quality, "2") == 0)
		o->quality = quality[0] - '0';
	else
	{
		fprintf(stderr, "ERROR: Unknown Quality \"%s\"\n", quality);
		return -1;
	}

	return 0;
}

static void model_geometry(const struct swii_options *o,
		struct swii_geometry *g)
{
	/* CUPS rasters already cover the PPD imageable area; nothing to skip. */
	if(is_color_model(o->model))
		g->printWidth = 2919;
	else
		g->printWidth = 2879;
	g->leftMargin = 0;
	g->topMargin = 0;
	g->bottomMargin = 0;
}

static int job_init(const struct swii_options *o)
{
	if(send_frame(SWF_RESET, NULL, 0, NULL, 0) < 0)
		return -1;

	if(o->model == MODEL_SW2)
	{
		/* No 'h' or 230400 speed switch: the job stays at the port's 57600. */
		if(frame_query('2') < 0 || frame_query('B') < 0 ||
				frame_text("u") < 0 || frame_query('H') < 0 ||
				frame_text("bDA") < 0 || frame_query('2') < 0)
			return -1;
	}

	if(is_color_model(o->model))
	{
		if(frame_text("D") < 0)
			return -1;
		if(o->quality == 2)
		{
			if(frame_text("m0sAB") < 0 || frame_query('R') < 0)
				return -1;
		}
		else if(frame_text("m0nZAH") < 0)
			return -1;
	}

	return 0;
}

static int page_start(const struct swii_options *o)
{
	if(is_color_model(o->model))
		return frame_text("L");
	if(o->model == MODEL_SW2)
		return 0;
	return frame_text("nuA");
}

static int page_end(const struct swii_layout *L)
{
	unsigned char b[2];

	b[0] = (unsigned char)L->poll;
	b[1] = (unsigned char)(L->flags & SWF_BAND_WAITPAPER);
	return send_frame(SWF_PAGEEND, b, sizeof b, NULL, 0);
}

static int page_read(cups_raster_t *ras, const cups_page_header2_t *h,
		struct swii_page *pg)
{
	unsigned long y, bpl, planes, rowbytes;
	unsigned int n;

	if(h->cupsBitsPerColor != 1)
	{
		fprintf(stderr, "ERROR: Only 1 bit per color is supported (got %u)\n",
				h->cupsBitsPerColor);
		return -1;
	}
	if(h->cupsWidth == 0 || h->cupsHeight == 0)
	{
		fprintf(stderr, "ERROR: Empty raster page\n");
		return -1;
	}

	switch(h->cupsColorSpace)
	{
	case CUPS_CSPACE_K:
		planes = 1;
		break;
	case CUPS_CSPACE_KCMY:
		if(h->cupsColorOrder != CUPS_ORDER_BANDED)
		{
			fprintf(stderr, "ERROR: KCMY raster must be banded\n");
			return -1;
		}
		planes = 4;
		break;
	default:
		fprintf(stderr, "ERROR: Unsupported raster color space %d "
				"(need K or KCMY)\n", (int)h->cupsColorSpace);
		return -1;
	}

	bpl = h->cupsBytesPerLine;
	rowbytes = (h->cupsWidth + 7) / 8;
	if(bpl % planes != 0 || bpl / planes < rowbytes)
	{
		fprintf(stderr, "ERROR: Raster line of %lu bytes cannot hold %lu planes\n",
				bpl, planes);
		return -1;
	}

	pg->width = h->cupsWidth;
	pg->height = h->cupsHeight;
	pg->bytesPerLine = bpl;
	pg->stride = bpl / planes;
	pg->planes = (int)planes;
	pg->data = malloc((size_t)bpl * pg->height);
	if(!pg->data)
	{
		fprintf(stderr, "ERROR: Out of memory for page\n");
		return -1;
	}

	for(y = 0; y < pg->height; y++)
	{
		n = cupsRasterReadPixels(ras, pg->data + y * bpl, (unsigned)bpl);
		if(n != bpl)
		{
			fprintf(stderr, "ERROR: Short raster read at line %lu\n", y);
			free(pg->data);
			pg->data = NULL;
			return -1;
		}
	}
	return 0;
}

static void layout_setup(const struct swii_options *o,
		const struct swii_geometry *g, const struct swii_page *pg,
		struct swii_layout *L)
{
	long rowbytes = ((long)pg->width + 7) / 8;

	L->top = g->topMargin;
	L->end = (long)pg->height - g->bottomMargin;
	L->leftBytes = (g->leftMargin + 7) / 8;
	L->printRowBytes = (g->printWidth + 7) / 8;
	L->printwidth = rowbytes - L->leftBytes;
	if(L->printwidth > L->printRowBytes)
		L->printwidth = L->printRowBytes;
	if(L->printwidth < 0)
		L->printwidth = 0;
	L->printWidth = g->printWidth;

	L->color = pg->planes == 4 && o->colorCartridge && is_color_model(o->model);
	if(L->color)
	{
		/* Printer order is C, M, Y, K; raster order is K, C, M, Y. */
		L->planes[0] = 1;
		L->planes[1] = 2;
		L->planes[2] = 3;
		L->planes[3] = 0;
		L->nPlanes = 4;
	}
	else
	{
		L->planes[0] = 0;
		L->nPlanes = 1;
	}

	L->combine = !L->color && pg->planes == 4;
	L->delta = o->model != MODEL_SW1;
	L->poll = o->model != MODEL_SW1;
	L->flags = (L->color ? SWF_BAND_COLOR : 0) |
		(is_color_model(o->model) ? SWF_BAND_WAITPAPER : 0);
}

/* Copies the printed bytes of row y for one plane into dst. When combining,
 * dst is the OR of every ink plane instead. */
static void load_plane(const struct swii_page *pg, const struct swii_layout *L,
		long y, int plane, unsigned char *dst)
{
	const unsigned char *row = pg->data + (size_t)y * pg->bytesPerLine +
		(size_t)L->leftBytes;
	long i;
	int k;

	if(!L->combine)
	{
		memcpy(dst, row + (size_t)plane * pg->stride, (size_t)L->printwidth);
		return;
	}

	memset(dst, 0, (size_t)L->printwidth);
	for(k = 0; k < pg->planes; k++)
		for(i = 0; i < L->printwidth; i++)
			dst[i] |= row[(size_t)k * pg->stride + (size_t)i];
}

/* Encodes printed row y of every plane into out. Returns the encoded length. */
static size_t encode_row(const struct swii_page *pg, const struct swii_layout *L,
		long y, int delta, unsigned char *cur, unsigned char *prev,
		unsigned char *out)
{
	size_t total = 0;
	int k;

	for(k = 0; k < L->nPlanes; k++)
	{
		long i;

		load_plane(pg, L, y, L->planes[k], cur);
		if(delta)
		{
			load_plane(pg, L, y - 1, L->planes[k], prev);
			for(i = 0; i < L->printwidth; i++)
				cur[i] ^= prev[i];
		}

		total += swii_encode_row(cur, (size_t)L->printwidth, out + total,
				(size_t)L->printRowBytes);
	}
	return total;
}

static int emit_band(const struct swii_layout *L, long first, long last,
		const unsigned char *data, size_t len)
{
	unsigned char hdr[SWF_BAND_HDR_SIZE];
	int flags = L->flags;

	if(first == L->top)
		flags |= SWF_BAND_FIRST;

	hdr[0] = (unsigned char)flags;
	swf_put_le16(hdr + 1, (unsigned long)(first - L->top));
	swf_put_le16(hdr + 3, (unsigned long)(last - L->top));
	swf_put_le16(hdr + 5, (unsigned long)L->printWidth);
	return send_frame(SWF_BAND, hdr, sizeof hdr, data, len);
}

static int emit_page(const struct swii_options *o, const struct swii_geometry *g,
		const struct swii_page *pg, unsigned int copies)
{
	struct swii_layout L;
	unsigned char *band = NULL, *rowbuf = NULL, *tmp = NULL, *prevbuf = NULL;
	size_t rowMax, bandLen;
	long y, bandTop, bandEnd;
	unsigned int c;
	int ret = -1;

	layout_setup(o, g, pg, &L);

	rowMax = (size_t)L.nPlanes * swii_encode_max((size_t)L.printwidth,
			(size_t)L.printRowBytes);
	rowbuf = malloc(rowMax);
	band = malloc((size_t)SWII_BAND_ROWS * rowMax);
	tmp = malloc((size_t)L.printwidth + 8);
	prevbuf = malloc((size_t)L.printwidth + 8);
	if(!rowbuf || !band || !tmp || !prevbuf)
	{
		fprintf(stderr, "ERROR: Out of memory for encoding\n");
		goto out;
	}

	for(c = 0; c < copies; c++)
	{
		if(page_start(o) < 0)
			goto out;

		for(bandTop = L.top; bandTop < L.end; bandTop = bandEnd)
		{
			bandEnd = bandTop + SWII_BAND_ROWS;
			if(bandEnd > L.end)
				bandEnd = L.end;

			bandLen = 0;
			for(y = bandTop; y < bandEnd; y++)
			{
				/* The first row of each band is absolute, not a delta. */
				size_t rowLen = encode_row(pg, &L, y,
						L.delta && y > bandTop, tmp, prevbuf, rowbuf);

				memcpy(band + bandLen, rowbuf, rowLen);
				bandLen += rowLen;
			}
			if(emit_band(&L, bandTop, bandEnd - 1, band, bandLen) < 0)
				goto out;
		}

		if(page_end(&L) < 0)
			goto out;
	}
	ret = 0;

out:
	free(rowbuf);
	free(band);
	free(tmp);
	free(prevbuf);
	return ret;
}

/*
 * jobCopies is argv[4] when this filter is the first program in the chain
 * (the only case where CUPS expects it to generate copies), else 1.
 */
static int process_job(const struct swii_options *o, cups_raster_t *ras,
		unsigned int jobCopies)
{
	cups_page_header2_t header;
	struct swii_geometry g;
	int jobStarted = 0;

	model_geometry(o, &g);

	while(cupsRasterReadHeader2(ras, &header))
	{
		struct swii_page pg;
		unsigned int copies = header.NumCopies > jobCopies ? header.NumCopies : jobCopies;

		if(page_read(ras, &header, &pg) < 0)
			goto fail;
		if(!jobStarted)
		{
			if(job_init(o) < 0)
			{
				free(pg.data);
				goto fail;
			}
			jobStarted = 1;
		}
		if(emit_page(o, &g, &pg, copies) < 0)
		{
			free(pg.data);
			goto fail;
		}
		free(pg.data);
	}
	return 0;

fail:
	if(jobStarted)
		send_frame(SWF_ABORT, NULL, 0, NULL, 0);
	return -1;
}

int main(int argc, char *argv[])
{
	struct swii_options o;
	cups_raster_t *ras;
	unsigned int jobCopies = 1;
	int fd = STDIN_FILENO;
	int status;

	if(argc != 6 && argc != 7)
	{
		fprintf(stderr, "ERROR: Usage: %s job user title copies options [file]\n",
				argv[0]);
		return 1;
	}

	if(load_options(argv[5], &o) < 0)
		return 1;

	if(argc == 7)
	{
		char *end;
		unsigned long n = strtoul(argv[4], &end, 10);

		if(*argv[4] && !*end && n > 1)
			jobCopies = n > 9999 ? 9999 : (unsigned int)n;
		fd = open(argv[6], O_RDONLY);
		if(fd < 0)
		{
			fprintf(stderr, "ERROR: Unable to open %s: %s\n", argv[6],
					strerror(errno));
			return 1;
		}
	}

	ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
	if(!ras)
	{
		fprintf(stderr, "ERROR: Unable to read raster stream\n");
		if(fd != STDIN_FILENO)
			close(fd);
		return 1;
	}

	status = process_job(&o, ras, jobCopies) < 0 ? 1 : 0;

	cupsRasterClose(ras);
	if(fd != STDIN_FILENO)
		close(fd);
	return status;
}
