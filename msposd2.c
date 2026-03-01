#include "version.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/util.h>

#include "osd/msp/msp.h"
#include "osd/msp/vtxmenu.h"

#define UART_FCR_TRIGGER_RX_L3 0x10000
#define MAX_BUFFER_SIZE 50

bool vtxMenuActive = false;
bool armed = true;
bool AbortNow = false;
bool verbose = false;
bool ParseMSP = true;
bool DrawOSD = true;
bool mspVTXenabled = false;
bool vtxMenuEnabled = false;
bool monitor_wfb = false;
extern char *recording_dir;

struct event_base *base = NULL;

int serial_fd = -1;
int in_sock = 0;
int MSPUDPPort = 0;
int MSP_PollRate = 20;
int MSP_RequestRate = 20;
int OSD_RenderRate = 20;
int OSD_MessageRate = 2;
int matrix_size = 0;
int AHI_Enabled = 0;
bool enable_simple_uart = false;
typedef enum {
	UART_MODE_EVENT = 0,
	UART_MODE_SIMPLE = 1,
	UART_MODE_DRAIN = 2,
} uart_mode_t;
static uart_mode_t uart_mode = UART_MODE_EVENT;

const char *default_master = "/dev/ttyAMA0";
const int default_baudrate = 115200;

msp_state_t *rx_msp_state;

struct bufferevent *serial_bev = NULL;
struct sockaddr_in sin_out = {
	.sin_family = AF_INET,
};

int out_sock = 0;
int last_board_temp = -100;
static int shutdown_exit_code = EXIT_SUCCESS;

// Include the renderer implementation directly, same as msposd.
#include "osd.c"

uint16_t channels[18];

enum {
	OPT_SERIAL_HZ = 1000,
	OPT_MSP_HZ,
	OPT_RENDER_HZ,
	OPT_MSG_HZ,
};

static int clamp_rate_hz(int hz, int min_hz, int max_hz) {
	if (hz < min_hz)
		return min_hz;
	if (hz > max_hz)
		return max_hz;
	return hz;
}

static void set_render_rate_hz(int hz) {
	OSD_RenderRate = clamp_rate_hz(hz, 1, 50);
	MinTimeBetweenScreenRefresh = 1000 / OSD_RenderRate;
}

static int timer_interval_us(int hz) {
	return 1000000 / clamp_rate_hz(hz, 1, 50);
}

static void print_usage(void) {
	printf("Usage: msposd2 [OPTIONS]\n"
		   "Where:\n"
		   "  -m --master      Serial port to receive MSP (%s by default)\n"
		   "  -b --baudrate    Serial port baudrate (%d by default)\n"
		   "  -r --fps         Max OSD refresh rate (event mode: 5..50). Use 1xxx for simple UART mode,\n"
		   "                   and 2xxx for high-throughput UART drain mode (both 1..50, e.g. 2010 => 10Hz)\n"
		   "     --serial-hz   Serial poll rate for simple/drain UART modes (1..50)\n"
		   "     --msp-hz      MSP request cadence (1..50)\n"
		   "     --render-hz   Max OSD render cadence (1..50)\n"
		   "     --msg-hz      MSPOSD.msg / text refresh cadence (1..50, %d by default)\n"
		   "  -a --ahi         Draw graphic AHI, mode (0:off,1:ladder,2:simple,3:ladderEx)\n"
		   "  -x --matrix      OSD matrix (0:53x20, 1:50x18, 11+:variable font templates)\n"
		   "  -z --size        OSD resolution WxH (e.g. 1920x1080)\n"
		   "     --mspvtx      Enable mspvtx support\n"
		   "  -d --osd         Enabled by default (accepted for compatibility)\n"
		   "  -v --verbose     Show debug info\n"
		   "  -h --help        Display this help\n",
		default_master, default_baudrate, OSD_MessageRate);
}

static speed_t speed_by_value(int baudrate) {
	switch (baudrate) {
	case 9600:
		return B9600;
	case 19200:
		return B19200;
	case 38400:
		return B38400;
	case 57600:
		return B57600;
	case 115200:
		return B115200;
	case 230400:
		return B230400;
	case 460800:
		return B460800;
	case 500000:
		return B500000;
	case 921600:
		return B921600;
	case 1500000:
		return B1500000;
	default:
		printf("Unsupported baudrate %d\n", baudrate);
		exit(EXIT_FAILURE);
	}
}

static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

uint64_t get_current_time_ms(void) { return now_ms(); }

static void signal_cb(evutil_socket_t fd, short event, void *arg) {
	struct event_base *event_base = arg;
	(void)event;

	if (AbortNow) {
		fprintf(stderr, "Exit request: %s signal received again, forcing exit\n", strsignal(fd));
		_exit(128 + fd);
	}

	AbortNow = true;
	shutdown_exit_code = 128 + fd;
	fprintf(stderr, "Exit request: %s signal received\n", strsignal(fd));

	if (serial_bev != NULL)
		bufferevent_disable(serial_bev, EV_READ | EV_WRITE);

	if (serial_fd >= 0) {
		close(serial_fd);
		serial_fd = -1;
	}

	event_base_loopbreak(event_base);
}

int SendWfbLogToGround(void) {
	(void)monitor_wfb;
	return 0;
}

int GetTempSigmaStar(void) {
	static uint64_t last_read_ms = 0;
	if (now_ms() - last_read_ms < 1000)
		return last_board_temp;
	last_read_ms = now_ms();

	FILE *file = fopen("/sys/devices/virtual/mstar/msys/TEMP_R", "r");
	if (file == NULL)
		return -100;

	char buff[200];
	int temp = -100;
	if (fgets(buff, sizeof(buff), file) != NULL) {
		char *temperature_str = strstr(buff, "Temperature");
		if (temperature_str != NULL)
			temp = atoi(temperature_str + 12);
	}
	fclose(file);
	last_board_temp = temp;
	return temp;
}

int GetTempGoke(void) {
	char buffer[6] = {0};
	int temp = 0;
	FILE *file = fopen("/tmp/board_temperature.msg", "r");
	if (file != NULL) {
		if (fgets(buffer, sizeof(buffer), file) != NULL)
			temp = atoi(buffer);
		fclose(file);
	}
	return temp;
}

int GetTXTemp(void) {
	static int cached_temp = 0;
	static uint64_t last_read_ms = 0;
	if (now_ms() - last_read_ms < 1000)
		return cached_temp;
	last_read_ms = now_ms();

	glob_t glob_result;
	memset(&glob_result, 0, sizeof(glob_result));
	FILE *stat = NULL;
	int temperature = 0;

	if (glob("/proc/net/*/*/thermal_state", GLOB_NOSORT, NULL, &glob_result) == 0) {
		if (glob_result.gl_pathc > 0) {
			stat = fopen(glob_result.gl_pathv[0], "r");
			if (stat != NULL) {
				char buffer[128];
				if (fgets(buffer, sizeof(buffer), stat) != NULL) {
					if (sscanf(buffer,
							"rf_path: %*d, thermal_value: %*d, offset: %*d, temperature: %d",
							&temperature) != 1) {
						temperature = cached_temp;
					}
				}
				fclose(stat);
			}
		}
	}
	globfree(&glob_result);
	cached_temp = temperature;
	return temperature;
}

void showchannels(int count) {
	if (!verbose)
		return;
	printf("Channels :");
	for (int i = 0; i < count; i++)
		printf("| %02d", channels[i]);
	printf("\n");
}

void ProcessChannels(void) {
	if (!armed)
		handle_stickcommands(channels);
}

static long ttl_packets = 0;
static long ttl_bytes = 0;
static long stat_bytes = 0;
static long stat_pckts = 0;
static long last_stat = 0;

static void reset_stats_if_needed(void) {
	if (get_time_ms() - last_stat <= 1000)
		return;

	last_stat = get_time_ms();
	if (stat_screen_refresh_count == 0)
		stat_screen_refresh_count++;

	if (verbose) {
		printf("UART Events:%lu MessagesTTL:%u AttitMSGs:%u(%dms) Bytes/S:%lu FPS:%u of %u "
		       "(skipped:%d), AvgFrameLoad ms:%d | %d | %d |\n",
			stat_pckts,
			stat_msp_msgs,
			stat_msp_msg_attitude,
			(stat_attitudeDelay / (stat_msp_msg_attitude + 1)),
			stat_bytes,
			stat_screen_refresh_count,
			stat_MSP_draw_complete_count,
			stat_skipped_frames,
			stat_draw_overlay_1 / stat_screen_refresh_count,
			stat_draw_overlay_2 / stat_screen_refresh_count,
			stat_draw_overlay_3 / stat_screen_refresh_count);
		showchannels(18);
	}

	stat_screen_refresh_count = 0;
	stat_pckts = 0;
	stat_bytes = 0;
	stat_msp_msgs = 0;
	stat_msp_msg_attitude = 0;
	stat_draw_overlay_1 = 0;
	stat_draw_overlay_2 = 0;
	stat_draw_overlay_3 = 0;
	stat_skipped_frames = 0;
	stat_MSPBytesSent = 0;
	stat_MSP_draw_complete_count = 0;
	stat_UDP_MSPframes = 0;
}

static void process_serial_bytes(const unsigned char *data, int packet_len) {
	reset_stats_if_needed();
	stat_pckts++;
	stat_bytes += packet_len;
	ttl_packets++;
	ttl_bytes += packet_len;

	for (int i = 0; i < packet_len; i++)
		msp_process_data(rx_msp_state, data[i]);
}

static void read_serial_poll_once(void) {
	unsigned char data[4096];
	int packet_len = read(serial_fd, data, sizeof(data));
	if (packet_len > 0)
		process_serial_bytes(data, packet_len);
}

static void read_serial_poll_drain(void) {
	unsigned char data[4096];
	int loops = 0;
	while (loops++ < 64) {
		int packet_len = read(serial_fd, data, sizeof(data));
		if (packet_len > 0) {
			process_serial_bytes(data, packet_len);
			if (packet_len < (int)sizeof(data))
				return;
			continue;
		}

		if (packet_len == 0)
			return;

		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;

		if (verbose)
			fprintf(stderr, "UART read error: %s\n", strerror(errno));
		return;
	}
}

static void serial_read_cb(struct bufferevent *bev, void *arg) {
	(void)arg;
	if (AbortNow)
		return;

	struct evbuffer *input = bufferevent_get_input(bev);
	int packet_len, in_len;

	while ((in_len = evbuffer_get_length(input))) {
		unsigned char *data = evbuffer_pullup(input, in_len);
		if (data == NULL)
			return;

		packet_len = in_len;
		process_serial_bytes(data, packet_len);

		evbuffer_drain(input, packet_len);
	}
}

static void serial_event_cb(struct bufferevent *bev, short events, void *arg) {
	(void)bev;
	struct event_base *event_base = arg;

	if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
		printf("Serial connection closed\n");
		AbortNow = true;
		if (shutdown_exit_code == EXIT_SUCCESS)
			shutdown_exit_code = EXIT_FAILURE;
		event_base_loopbreak(event_base);
	}
}

int VariantCounter = 0;

static void send_variant_request2(int fd) {
	uint8_t buffer[6];
	if (AbortNow)
		return;

	if (VariantCounter % 5 == 1 || vtxMenuActive) {
		construct_msp_command(buffer, MSP_RC, NULL, 0, MSP_OUTBOUND);
		write(fd, &buffer, sizeof(buffer));
	} else if (AHI_Enabled) {
		if (AHI_Enabled == 3 && VariantCounter % 13 == 1) {
			construct_msp_command(buffer, MSP_COMP_GPS, NULL, 0, MSP_OUTBOUND);
			write(fd, &buffer, sizeof(buffer));
		} else {
			construct_msp_command(buffer, MSP_ATTITUDE, NULL, 0, MSP_OUTBOUND);
			write(fd, &buffer, sizeof(buffer));
			last_MSP_ATTITUDE = get_time_ms();
		}
	}

	if (MSP_RequestRate <= ++VariantCounter) {
		construct_msp_command(buffer, MSP_CMD_FC_VARIANT, NULL, 0, MSP_OUTBOUND);
		write(fd, &buffer, sizeof(buffer));
		if (bitmapFnt.pData != NULL && mspVTXenabled) {
			construct_msp_command(buffer, MSP_GET_VTX_CONFIG, NULL, 0, MSP_OUTBOUND);
			write(fd, &buffer, sizeof(buffer));
		}
		VariantCounter = 0;
	}

	if (MSP_RequestRate > 2 && MSP_RequestRate - 2 == VariantCounter) {
		construct_msp_command(buffer, MSP_CMD_STATUS, NULL, 0, MSP_OUTBOUND);
		write(fd, &buffer, sizeof(buffer));
	}
}

static void poll_serial(evutil_socket_t sock, short event, void *arg) {
	(void)sock;
	(void)event;
	(void)arg;

	if (uart_mode == UART_MODE_SIMPLE)
		read_serial_poll_once();
	else if (uart_mode == UART_MODE_DRAIN)
		read_serial_poll_drain();
}

static void poll_msp(evutil_socket_t sock, short event, void *arg) {
	(void)sock;
	(void)event;
	int fd = *((int *)arg);

	if (matrix_size == 99)
		draw_screenBMP();

	send_variant_request2(fd);
}

static int validate_master_path(const char *port_name) {
	if (port_name == NULL || port_name[0] == '\0')
		return -1;

	if (strchr(port_name, ':') != NULL)
		return -1;

	if (port_name[0] != '/')
		return -1;

	return 0;
}

static int handle_data(const char *port_name, int baudrate) {
	struct event *sig_int = NULL, *sig_term = NULL, *sig_hup = NULL, *sig_quit = NULL;
	struct event *serial_tmr = NULL, *msp_tmr = NULL;
	int ret = EXIT_SUCCESS;

	AbortNow = false;
	shutdown_exit_code = EXIT_SUCCESS;

	if (validate_master_path(port_name) != 0) {
		fprintf(stderr, "Local renderer mode requires a serial device path, got: %s\n", port_name);
		return EXIT_FAILURE;
	}

	serial_fd = open(port_name, O_RDWR | O_NOCTTY);
	if (serial_fd < 0) {
		fprintf(stderr, "Error while opening port %s: %s\n", port_name, strerror(errno));
		return EXIT_FAILURE;
	}

	evutil_make_socket_nonblocking(serial_fd);
	printf("Listening UART on %s...\n", port_name);

	struct termios options;
	tcgetattr(serial_fd, &options);
	cfsetspeed(&options, speed_by_value(baudrate));

	options.c_cflag &= ~CSIZE;
	options.c_cflag |= CS8;
	options.c_cflag &= ~PARENB;
	options.c_cflag &= ~PARODD;
	options.c_cflag &= ~CSTOPB;
	options.c_cflag |= (CLOCAL | CREAD);
	options.c_oflag &= ~OPOST;
	options.c_lflag &= 0;
	options.c_iflag &= 0;
	options.c_oflag &= 0;

	if (enable_simple_uart || uart_mode == UART_MODE_DRAIN)
		options.c_iflag |= UART_FCR_TRIGGER_RX_L3;

	cfmakeraw(&options);
	tcsetattr(serial_fd, TCSANOW, &options);
	tcflush(serial_fd, TCIOFLUSH);

	if (mspVTXenabled) {
		printf("Setup mspVTX ...\n");
		msp_set_vtx_config(serial_fd);
	}

	base = event_base_new();

	sig_int = evsignal_new(base, SIGINT, signal_cb, base);
	event_add(sig_int, NULL);
	signal(SIGPIPE, SIG_IGN);

	sig_term = evsignal_new(base, SIGTERM, signal_cb, base);
	event_add(sig_term, NULL);

	sig_hup = evsignal_new(base, SIGHUP, signal_cb, base);
	event_add(sig_hup, NULL);

	sig_quit = evsignal_new(base, SIGQUIT, signal_cb, base);
	event_add(sig_quit, NULL);

	if (uart_mode == UART_MODE_EVENT) {
		serial_bev = bufferevent_socket_new(base, serial_fd, 0);
		bufferevent_setwatermark(serial_bev, EV_READ, 16, 0);
		bufferevent_setcb(serial_bev, serial_read_cb, NULL, serial_event_cb, base);
		bufferevent_enable(serial_bev, EV_READ);
	}

	if (uart_mode != UART_MODE_EVENT) {
		serial_tmr = event_new(base, -1, EV_PERSIST, poll_serial, &serial_fd);
		struct timeval serial_interval = {
			.tv_sec = 0,
			.tv_usec = timer_interval_us(MSP_PollRate),
		};
		evtimer_add(serial_tmr, &serial_interval);
	}

	msp_tmr = event_new(base, -1, EV_PERSIST, poll_msp, &serial_fd);
	struct timeval msp_interval = {
		.tv_sec = 0,
		.tv_usec = timer_interval_us(MSP_RequestRate),
	};
	evtimer_add(msp_tmr, &msp_interval);

	event_base_dispatch(base);

	if (serial_tmr) {
		event_del(serial_tmr);
		event_free(serial_tmr);
	}
	if (msp_tmr) {
		event_del(msp_tmr);
		event_free(msp_tmr);
	}
	if (serial_bev) {
		bufferevent_free(serial_bev);
		serial_bev = NULL;
	}
	if (serial_fd >= 0) {
		close(serial_fd);
		serial_fd = -1;
	}
	if (sig_int)
		event_free(sig_int);
	if (sig_term)
		event_free(sig_term);
	if (sig_hup)
		event_free(sig_hup);
	if (sig_quit)
		event_free(sig_quit);
	if (base)
		event_base_free(base);

	base = NULL;
	libevent_global_shutdown();
	CloseMSP();
	ret = shutdown_exit_code;
	shutdown_exit_code = EXIT_SUCCESS;
	return ret;
}

static void set_resolution(int width, int height) {
	if (width < 1280 || width > 3840)
		width = 1280;
	majestic_width = width;

	if (height < 720 || height > 2160)
		height = 720;
	majestic_height = height;
}

int main(int argc, char **argv) {
	const struct option long_options[] = {
		{"master", required_argument, NULL, 'm'},
		{"baudrate", required_argument, NULL, 'b'},
		{"fps", required_argument, NULL, 'r'},
		{"serial-hz", required_argument, NULL, OPT_SERIAL_HZ},
		{"msp-hz", required_argument, NULL, OPT_MSP_HZ},
		{"render-hz", required_argument, NULL, OPT_RENDER_HZ},
		{"msg-hz", required_argument, NULL, OPT_MSG_HZ},
		{"osd", no_argument, NULL, 'd'},
		{"ahi", required_argument, NULL, 'a'},
		{"matrix", required_argument, NULL, 'x'},
		{"size", required_argument, NULL, 'z'},
		{"mspvtx", no_argument, NULL, '1'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	const char *port_name = default_master;
	int baudrate = default_baudrate;
	bool serial_rate_set = false;
	bool request_rate_set = false;
	bool render_rate_set = false;
	set_render_rate_hz(OSD_RenderRate);
	last_board_temp = -100;

	int opt = 0, r = 20;
	int long_index = 0;

	printf("Version: %s, compiled at: %s\n", GIT_VERSION, VERSION_STRING);
	printf("OSD mode enabled by default\n");

	while ((opt = getopt_long_only(argc, argv, "m:b:r:da:x:z:1vh", long_options, &long_index)) != -1) {
		switch (opt) {
		case 'm':
			port_name = optarg;
			printf("Listen on port: %s\n", port_name);
			break;

		case 'b':
			baudrate = atoi(optarg);
			break;

		case 'r':
			r = atoi(optarg);
			if (r <= 0)
				r = 20;

			uart_mode = UART_MODE_EVENT;
			enable_simple_uart = false;

			if (r >= 2000) {
				uart_mode = UART_MODE_DRAIN;
				enable_simple_uart = true;
				r = r % 1000;
				if (r == 0)
					r = 20;
				printf("UART drain mode: %d\n", r);
			} else if (r > 1000) {
				uart_mode = UART_MODE_SIMPLE;
				enable_simple_uart = true;
				r = r % 1000;
				if (r == 0)
					r = 20;
				printf("Simple UART reading mode: %d\n", r);
			}

			// Keep legacy event mode guard at 5..50; allow lower rates (1..50)
			// for polling modes where users may prefer very low tick rates.
			if (uart_mode == UART_MODE_EVENT) {
				if (r < 5)
					r = 5;
			} else {
				if (r < 1)
					r = 1;
			}
			if (r > 50)
				r = 50;
			if (!serial_rate_set)
				MSP_PollRate = r;
			if (!request_rate_set)
				MSP_RequestRate = r;
			if (!render_rate_set)
				set_render_rate_hz(r);
			break;

		case OPT_SERIAL_HZ:
			MSP_PollRate = clamp_rate_hz(atoi(optarg), 1, 50);
			serial_rate_set = true;
			break;

		case OPT_MSP_HZ:
			MSP_RequestRate = clamp_rate_hz(atoi(optarg), 1, 50);
			request_rate_set = true;
			break;

		case OPT_RENDER_HZ:
			set_render_rate_hz(clamp_rate_hz(atoi(optarg), 1, 50));
			render_rate_set = true;
			break;

		case OPT_MSG_HZ:
			OSD_MessageRate = clamp_rate_hz(atoi(optarg), 1, 50);
			break;

		case 'd':
			// Compatibility switch; OSD is always enabled in msposd2.
			DrawOSD = true;
			break;

		case 'a':
			AHI_Enabled = atoi(optarg);
			break;

		case 'x':
			matrix_size = atoi(optarg);
			break;

		case 'z': {
			char buffer[16];
			strncpy(buffer, optarg, sizeof(buffer));
			buffer[sizeof(buffer) - 1] = '\0';
			char *limit = strchr(buffer, 'x');
			if (limit) {
				*limit = '\0';
				set_resolution(atoi(buffer), atoi(limit + 1));
			}
			break;
		}

		case '1':
			mspVTXenabled = true;
			break;

		case 'v':
			verbose = true;
			printf("Verbose mode enabled\n");
			break;

		case 'h':
		default:
			print_usage();
			return EXIT_SUCCESS;
		}
	}

	if (verbose) {
		printf("Rates: serial=%dHz msp=%dHz render=%dHz msg=%dHz\n",
			MSP_PollRate, MSP_RequestRate, OSD_RenderRate, OSD_MessageRate);
		if (uart_mode == UART_MODE_EVENT && serial_rate_set)
			printf("Note: --serial-hz only applies to 1xxx/2xxx UART polling modes\n");
	}

	strcpy(_port_name, port_name);
	rx_msp_state = calloc(1, sizeof(msp_state_t));
	assert(rx_msp_state != NULL);
	rx_msp_state->cb = &rx_msp_callback;
	InitMSPHook();

	return handle_data(port_name, baudrate);
}
