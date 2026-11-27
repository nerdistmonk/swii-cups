/*
 * CUPS backend for the Apple StyleWriter. Reads frames from the filter
 * (see stylewriter.h) and owns the serial port: pacing, status polling,
 * paper-out handling and speed changes.
 */

#define _DEFAULT_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <cups/backend.h>

#include "stylewriter.h"

#define SWII_USLEEP_US 100000L
#define SWII_STATUS_TIMEOUT_MS 10000
#define SWII_RESET_TIMEOUT_S 60
#define SWII_PAPER_RETRY_S 5
#define SWII_QUERY_PAUSE_US 200000L
#define SWII_RESET_SETTLE_S 2
#define SWII_DEFAULT_SPEED B57600
#define SWII_MAX_PAYLOAD (0x10000 + 64)
#define SWII_URI_PREFIX "stylewriter:"

static int g_fd = -1;
static int g_in = STDIN_FILENO;
static int g_page_active;
static int g_paper_warning;
static volatile sig_atomic_t g_cancel;
static unsigned char g_payload[SWII_MAX_PAYLOAD];

static void fail_job(const char *fmt, ...)
	__attribute__((noreturn, format(printf, 1, 2)));

static int raw_write(const unsigned char *buf, size_t len)
{
	while(len > 0)
	{
		ssize_t n = write(g_fd, buf, len);

		if(n < 0 && errno == EINTR)
			continue;
		if(n <= 0)
			return -1;
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

static void eject_quiet(void)
{
	static const unsigned char eject[4] = { 0xFF, 0xFF, 0xFF, 'I' };

	if(g_fd >= 0)
		raw_write(eject, sizeof eject);
}

static void check_cancel(void)
{
	if(!g_cancel)
		return;
	fprintf(stderr, "INFO: Job canceled\n");
	if(g_page_active)
		eject_quiet();
	exit(CUPS_BACKEND_CANCEL);
}

static void fail_job(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "ERROR: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	eject_quiet();
	exit(CUPS_BACKEND_FAILED);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap(long usec)
{
	usleep(usec);
	check_cancel();
}

static void cancel_sleep(unsigned int secs)
{
	while(secs > 0 && !g_cancel)
		secs = sleep(secs);
	check_cancel();
}

/* Returns 1 when ready, 0 on timeout, -1 on error. */
static int wait_fd(int fd, short events, int timeout_ms)
{
	long long deadline = now_ms() + timeout_ms;

	for(;;)
	{
		struct pollfd p = { fd, events, 0 };
		long long left = deadline - now_ms();
		int r;

		if(left < 0)
			left = 0;
		r = poll(&p, 1, (int)left);
		if(r > 0)
			return 1;
		if(r == 0)
			return 0;
		if(errno != EINTR)
			return -1;
		check_cancel();
	}
}

static void write_all(const void *buf, size_t len)
{
	const unsigned char *p = buf;

	while(len > 0)
	{
		ssize_t n = write(g_fd, p, len);

		if(n < 0 && errno == EINTR)
		{
			check_cancel();
			continue;
		}
		if(n <= 0)
			fail_job("Write to printer failed: %s", strerror(errno));
		p += n;
		len -= (size_t)n;
	}
}

static void send_cmd(unsigned char cmd)
{
	unsigned char b[4] = { 0xFF, 0xFF, 0xFF, cmd };

	write_all(b, sizeof b);
}

static void discard_input(void)
{
	unsigned char b[64];

	while(wait_fd(g_fd, POLLIN, 0) == 1)
	{
		ssize_t n = read(g_fd, b, sizeof b);

		if(n < 0 && errno == EINTR)
			check_cancel();
		else if(n <= 0)
			return;
	}
}

/* Returns the reply byte, or -1 on timeout. */
static int read_reply(int timeout_ms)
{
	unsigned char c;

	if(wait_fd(g_fd, POLLIN, timeout_ms) != 1)
		return -1;
	for(;;)
	{
		ssize_t n = read(g_fd, &c, 1);

		if(n == 1)
			return c;
		if(n < 0 && errno == EINTR)
		{
			check_cancel();
			continue;
		}
		return -1;
	}
}

static int get_status(unsigned char cmd)
{
	discard_input();
	send_cmd(cmd);
	return read_reply(SWII_STATUS_TIMEOUT_MS);
}

static void set_paper_warning(int on)
{
	if(on == g_paper_warning)
		return;
	g_paper_warning = on;
	fprintf(stderr, "STATE: %cmedia-empty-warning\n", on ? '+' : '-');
}

/* The Mac driver only queries '2' and 'B' while bands are in flight. */
static void poll_all(int *c2, int *c)
{
	*c2 = get_status('2');
	*c = get_status('B');
}

static void wait_status(int stat, int wp, int can_handle)
{
	int c2, c;

	poll_all(&c2, &c);
	while(c != stat)
	{
		if(c < 0 || c2 < 0)
			fail_job("Printer did not respond to status query");
		nap(SWII_USLEEP_US);
		switch(c2)
		{
		case 4:
			if(can_handle)
				return;
			if(!wp)
			{
				set_paper_warning(1);
				fail_job("Out of paper at a bad time");
			}
			do
			{
				fprintf(stderr, "WARNING: Out of paper, waiting for paper\n");
				cancel_sleep(SWII_PAPER_RETRY_S);
				send_cmd('S');
				c2 = get_status('2');
			} while(c2 == 4);
			set_paper_warning(0);
			break;
		case 0x80:
		case 0:
			break;
		default:
			fail_job("Printer reported error status 0x%02x", c2);
		}
		poll_all(&c2, &c);
	}
}

static void reset_printer(void)
{
	long long deadline;
	int c1, c2, c;

	send_cmd('I');
	cancel_sleep(SWII_RESET_SETTLE_S);
	deadline = now_ms() + SWII_RESET_TIMEOUT_S * 1000LL;
	for(;;)
	{
		nap(SWII_USLEEP_US);
		c1 = get_status('1');
		c2 = get_status('2');
		c = get_status('B');
		if(c1 == 1 && c2 == 0 && c == 0xA0)
			return;
		if(c1 != 1 && c1 != -1)
			return;
		if(now_ms() > deadline)
			fail_job("Printer did not finish resetting");
	}
}

static void drain_output(void)
{
	while(tcdrain(g_fd) < 0)
	{
		if(errno != EINTR)
			fail_job("tcdrain failed: %s", strerror(errno));
		check_cancel();
	}
}

static void set_speed(unsigned long bps)
{
	struct termios t;
	speed_t sp;

	switch(bps)
	{
	case 9600:
		sp = B9600;
		break;
	case 57600:
		sp = B57600;
		break;
	case 115200:
		sp = B115200;
		break;
	case 230400:
		sp = B230400;
		break;
	default:
		fail_job("Unsupported serial speed %lu", bps);
	}
	drain_output();
	if(tcgetattr(g_fd, &t) < 0)
		fail_job("Serial port setup failed: %s", strerror(errno));
	cfsetispeed(&t, sp);
	cfsetospeed(&t, sp);
	t.c_cflag &= ~(PARENB | CSTOPB);
	t.c_cflag |= CS8;
	if(tcsetattr(g_fd, TCSADRAIN, &t) < 0)
		fail_job("Serial port setup failed: %s", strerror(errno));
}

static void send_rect(int type, unsigned long top, unsigned long bottom,
		unsigned long right)
{
	unsigned char r[9];

	r[0] = (unsigned char)type;
	swf_put_le16(r + 1, 0);
	swf_put_le16(r + 3, top);
	swf_put_le16(r + 5, right);
	swf_put_le16(r + 7, bottom);
	write_all(r, sizeof r);
}

static void send_data(const unsigned char *data, unsigned long len)
{
	unsigned char hdr[3] = { 'G', 0, 0 };
	static const unsigned char term = 0;

	swf_put_le16(hdr + 1, len);
	write_all(hdr, sizeof hdr);
	write_all(data, len);
	write_all(&term, 1);
}

static void send_band(const unsigned char *p, unsigned long len)
{
	int drain = p[0];
	int flags = p[1];
	unsigned long top = swf_get_le16(p + 2);
	unsigned long bottom = swf_get_le16(p + 4);
	unsigned long right = swf_get_le16(p + 6);
	const unsigned char *data = p + SWF_BAND_HDR_SIZE;
	unsigned long dlen = len - SWF_BAND_HDR_SIZE;
	int color = (flags & SWF_BAND_COLOR) ? 'c' : 'R';
	int wp = flags & SWF_BAND_WAITPAPER;

	g_page_active = 1;
	wait_status(drain, wp, 0);
	for(;;)
	{
		send_rect(color, top, bottom, right);
		send_data(data, dlen);
		if(!(flags & SWF_BAND_FIRST))
			return;

		wait_status(drain, wp, 1);
		if(get_status('2') != 4)
		{
			set_paper_warning(0);
			return;
		}

		set_paper_warning(1);
		if(!wp)
		{
			fprintf(stderr, "WARNING: Out of paper, trying again in %d seconds\n",
					SWII_PAPER_RETRY_S);
			send_cmd('I');
			cancel_sleep(SWII_PAPER_RETRY_S);
			continue;
		}
		do
		{
			fprintf(stderr, "WARNING: Out of paper, trying to continue in %d seconds\n",
					SWII_PAPER_RETRY_S);
			cancel_sleep(SWII_PAPER_RETRY_S);
			send_cmd('S');
		} while(get_status('2') == 4);
		set_paper_warning(0);
	}
}

static void end_page(int poll, int drain, int wp)
{
	static const unsigned char formfeed = 0x0C;

	wait_status(drain, wp, 0);
	write_all(&formfeed, 1);
	if(poll)
	{
		int c;

		do
		{
			c = get_status('1');
			nap(SWII_USLEEP_US);
		} while(c == 1);
	}
	wait_status(drain, wp, 0);
	g_page_active = 0;
}

static void check_frame_len(unsigned char type, unsigned long len)
{
	unsigned long want;

	switch(type)
	{
	case SWF_WRITE:
		return;
	case SWF_BAND:
		if(len <= SWF_BAND_HDR_SIZE || len - SWF_BAND_HDR_SIZE > 0xFFFF)
			fail_job("Invalid band frame length %lu", len);
		return;
	case SWF_RESET:
	case SWF_ABORT:
		want = 0;
		break;
	case SWF_QUERY:
		want = 1;
		break;
	case SWF_SPEED:
		want = 4;
		break;
	case SWF_PAGEEND:
		want = 3;
		break;
	default:
		fail_job("Unknown frame type 0x%02x", type);
	}
	if(len != want)
		fail_job("Frame 0x%02x has length %lu, expected %lu", type, len, want);
}

static void run_frame(unsigned char type, unsigned long len)
{
	switch(type)
	{
	case SWF_WRITE:
		write_all(g_payload, len);
		break;
	case SWF_QUERY:
		send_cmd(g_payload[0]);
		nap(SWII_QUERY_PAUSE_US);
		discard_input();
		break;
	case SWF_SPEED:
		set_speed(swf_get_le32(g_payload));
		break;
	case SWF_RESET:
		reset_printer();
		break;
	case SWF_BAND:
		send_band(g_payload, len);
		break;
	case SWF_PAGEEND:
		end_page(g_payload[0], g_payload[1], g_payload[2] & SWF_BAND_WAITPAPER);
		break;
	case SWF_ABORT:
		fail_job("Job aborted by filter");
	}
}

static size_t read_full(int fd, unsigned char *buf, size_t len)
{
	size_t got = 0;

	while(got < len)
	{
		ssize_t n = read(fd, buf + got, len - got);

		if(n < 0 && errno == EINTR)
		{
			check_cancel();
			continue;
		}
		if(n < 0)
			fail_job("Read error on frame input: %s", strerror(errno));
		if(n == 0)
			break;
		got += (size_t)n;
	}
	return got;
}

static void run_frames(void)
{
	unsigned char hdr[SWF_HEADER_SIZE];

	for(;;)
	{
		size_t got;
		unsigned long len;

		check_cancel();
		got = read_full(g_in, hdr, sizeof hdr);
		if(got == 0)
			break;
		if(got < sizeof hdr)
			fail_job("Truncated frame header from filter");

		len = swf_get_le32(hdr + 1);
		if(len > SWII_MAX_PAYLOAD)
			fail_job("Frame payload too large (%lu bytes)", len);
		if(read_full(g_in, g_payload, len) != len)
			fail_job("Truncated frame payload from filter");

		check_frame_len(hdr[0], len);
		run_frame(hdr[0], len);
	}

	if(g_page_active)
		fail_job("Filter ended in the middle of a page");
	drain_output();
}

static const char *device_path(char **argv)
{
	const char *uri = cupsBackendDeviceURI(argv);
	const char *p;

	if(!uri || strncmp(uri, SWII_URI_PREFIX, strlen(SWII_URI_PREFIX)) != 0)
		fail_job("Device URI must start with %s", SWII_URI_PREFIX);
	p = uri + strlen(SWII_URI_PREFIX);
	if(strncmp(p, "//", 2) == 0)
		p += 2;
	if(*p != '/')
		fail_job("Device URI must name an absolute path");
	return p;
}

static void open_printer(const char *path)
{
	struct termios t;
	int fd, flags;

	fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if(fd < 0)
	{
		if(errno == EBUSY)
		{
			fprintf(stderr, "WARNING: %s is busy, retrying\n", path);
			exit(CUPS_BACKEND_RETRY_CURRENT);
		}
		fail_job("Unable to open %s: %s", path, strerror(errno));
	}
	if(!isatty(fd))
		fail_job("%s is not a serial device", path);
	if(tcgetattr(fd, &t) < 0)
		fail_job("Unable to read settings of %s: %s", path, strerror(errno));

	cfmakeraw(&t);
	t.c_cflag |= CS8 | CLOCAL | CREAD;
	t.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
	cfsetispeed(&t, SWII_DEFAULT_SPEED);
	cfsetospeed(&t, SWII_DEFAULT_SPEED);
	if(tcsetattr(fd, TCSAFLUSH, &t) < 0)
		fail_job("Unable to configure %s: %s", path, strerror(errno));
	tcflush(fd, TCIOFLUSH);

	flags = fcntl(fd, F_GETFL);
	if(flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
		fail_job("Unable to configure %s: %s", path, strerror(errno));

	g_fd = fd;
	fprintf(stderr, "STATE: -media-empty-warning\n");
}

static void on_term(int sig)
{
	(void)sig;
	g_cancel = 1;
}

static void list_devices(void)
{
	static const char *const patterns[] = { "/dev/ttyUSB*", "/dev/ttyACM*" };
	char uri[sizeof SWII_URI_PREFIX + PATH_MAX];
	char info[PATH_MAX + 64];
	glob_t g;
	size_t i, j;

	for(i = 0; i < sizeof patterns / sizeof patterns[0]; i++)
	{
		if(glob(patterns[i], 0, NULL, &g) != 0)
			continue;
		for(j = 0; j < g.gl_pathc; j++)
		{
			snprintf(uri, sizeof uri, "%s%s", SWII_URI_PREFIX, g.gl_pathv[j]);
			snprintf(info, sizeof info, "Apple StyleWriter on %s", g.gl_pathv[j]);
			cupsBackendReport("direct", uri, "Apple StyleWriter", info, NULL, NULL);
		}
		globfree(&g);
	}
}

int main(int argc, char *argv[])
{
	struct sigaction sa;
	const char *path;

	if(argc == 1)
	{
		list_devices();
		return CUPS_BACKEND_OK;
	}
	if(argc != 6 && argc != 7)
	{
		fprintf(stderr, "ERROR: Usage: %s job-id user title copies options [file]\n",
				argv[0]);
		return CUPS_BACKEND_FAILED;
	}

	signal(SIGPIPE, SIG_IGN);
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_term;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);

	path = device_path(argv);
	if(argc == 7)
	{
		g_in = open(argv[6], O_RDONLY);
		if(g_in < 0)
			fail_job("Unable to open job file %s: %s", argv[6], strerror(errno));
	}
	open_printer(path);

	run_frames();
	return CUPS_BACKEND_OK;
}
