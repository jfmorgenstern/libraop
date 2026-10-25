/*
 * RAOP : Client to control an AirPlay device, cli simple example
 *
 * Copyright (C) 2004 Shiro Ninomiya <shiron@snino.com>
 * Philippe <philippe_44@outlook.com>
 *
 * See LICENSE
 *
 */

#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "platform.h"

#if WIN
#include <conio.h>
#include <time.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/param.h>
#include <sys/time.h>
#if OSX || FREEBDS
#include <sys/resource.h>
#endif
#endif

#include "raop_client.h"
#include "cross_net.h"
#include "cross_ssl.h"
#include "cross_util.h"
#include "cross_log.h"

#define RAOP_SEC(ntp) ((uint32_t) ((ntp) >> 32))
#define RAOP_FRAC(ntp) ((uint32_t) (ntp))
#define RAOP_SECNTP(ntp) RAOP_SEC(ntp),RAOP_FRAC(ntp)

// debug level from tools & other elements
log_level	util_loglevel;
log_level	raop_loglevel;
log_level 	main_log;

// our debug level
log_level *loglevel =&main_log;

// track metadata set through the control pipe (T/A/L/G, sent with M), last artwork file (I)
#define META_LEN 256
static char meta_title[META_LEN], meta_artist[META_LEN], meta_album[META_LEN], meta_genre[META_LEN];
static char meta_artwork[META_LEN];

/*----------------------------------------------------------------------------*/
static void meta_set(char *dst, const char *src) {
	strncpy(dst, src, META_LEN - 1);
	dst[META_LEN - 1] = '\0';
}

/*----------------------------------------------------------------------------*/
static bool send_metadata(struct raopcl_s *raopcl) {
	// empty strings are valid DMAP values and clear the field on the receiver
	bool rc = raopcl_set_daap(raopcl, 4, "minm", 's', meta_title, "asar", 's', meta_artist,
										 "asal", 's', meta_album, "asgn", 's', meta_genre);
	if (rc) LOG_INFO("Metadata sent: %s - %s (%s)", meta_artist, meta_title, meta_album);
	else LOG_WARN("Metadata not sent (not connected, or device without text metadata capability; check -m)");
	return rc;
}

/*----------------------------------------------------------------------------*/
static bool send_artwork(struct raopcl_s *raopcl, const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		LOG_WARN("Artwork file %s cannot be opened: %s", path, strerror(errno));
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > 2 * 1024 * 1024) {
		LOG_WARN("Artwork file %s has an unusable size (%ld bytes)", path, size);
		fclose(f);
		return false;
	}
	char *data = malloc(size);
	if (!data || fread(data, 1, size, f) != (size_t) size) {
		LOG_WARN("Artwork file %s cannot be read", path);
		free(data);
		fclose(f);
		return false;
	}
	fclose(f);

	char *type = NULL;
	if (size >= 2 && (uint8_t) data[0] == 0xFF && (uint8_t) data[1] == 0xD8) type = "image/jpeg";
	else if (size >= 4 && !memcmp(data, "\x89PNG", 4)) type = "image/png";
	if (!type) {
		LOG_WARN("Artwork file %s is neither JPEG nor PNG", path);
		free(data);
		return false;
	}

	bool rc = raopcl_set_artwork(raopcl, type, (int) size, data);
	if (rc) LOG_INFO("Artwork sent: %s (%ld bytes, %s)", path, size, type);
	else LOG_WARN("Artwork not sent (not connected, or device without artwork capability; check -m)");
	free(data);
	return rc;
}

// different combination of debug levels per channel
struct debug_s {
	int main, raop, util;
} debug[] = { { lSILENCE, lSILENCE, lSILENCE },
			{ lERROR, lERROR, lERROR },
			{ lINFO, lERROR, lERROR },
			{ lINFO, lINFO, lERROR },
			{ lDEBUG, lERROR, lERROR },
			{ lDEBUG, lINFO, lERROR },
			{ lDEBUG, lDEBUG, lERROR },
			{ lSDEBUG, lINFO, lERROR },
			{ lSDEBUG, lDEBUG, lERROR },
			{ lSDEBUG, lSDEBUG, lERROR },
		};

/*----------------------------------------------------------------------------*/
static int print_usage(char *argv[])
{
	char *name = strrchr(argv[0], '\\');

	name = (name) ? name + 1 :argv[0];

	printf("usage: %s <options> <server_ip> <filename ('-' for stdin)>\n"
			   "\t[-ntp <file>] write current NTP in <file> and exit\n"
			   "\t[-p <port number>]\n"
			   "\t[-v <volume> (0-100)]\n"
			   "\t[-l <latency> (frames]\n"
			   "\t[-w <wait>]  (start after <wait> milliseconds)\n"
			   "\t[-n <start>] (start at NTP <start> + <wait>)\n"
			   "\t[-nf <start>] (start at NTP in <file> + <wait>)\n"
			   "\t[-e] audio payload encryption\n"
			   "\t[-u] for authentication (only if crypto present in TXT record)\n"
   			   "\t[-a] send ALAC compressed audio\n"
			   "\t[-s <secret>] (valid secret for AppleTV)\n"
			   "\t[-P <password>] (device password)\n"
			   "\t[-r] do AppleTV pairing\n"
			   "\t[-R] ('r' does a full RTSP re-handshake (raopcl_repair) before restart)\n"
			   "\t[-t <et>] (et field in mDNS - 4 for airport-express and used to detect MFi)\n"
			   "\t[-m <[0][,1][,2]>] (md in mDNS: metadata capabilties 0=text, 1=artwork, 2=progress)\n"
			   "\t[-o <model>] (am in mDNS: model HomePods, AppleTV... selects the User-Agent Apple accepts)\n"
			   "\t[-U <user-agent>] override the RTSP User-Agent (default: iTunes/7.6.2, AirPlay/999.0.0 for HomePod/AppleTV)\n"
			   "\t[-d <debug level>] (0 = silent)\n"
			   "\t[--control-pipe <path>] named pipe for runtime control commands, one per line:\n"
			   "\t      vNN set volume, q quit, T<title> A<artist> L<album> G<genre> set text (sent with M),\n"
			   "\t      I<file> send JPEG/PNG artwork, P<elapsed_ms>/<duration_ms> progress, C clear metadata\n"
			   "\t      (metadata needs the device's md capabilities passed with -m, e.g. -m 0,1,2)\n"
			   "\t[-i] (interactive commands: 'p'=pause, 'r'=(re)start, 's'=stop, 'q'=exit, ' '=block)\n",
			   name);
	return -1;
}

#if !WIN
/*----------------------------------------------------------------------------*/
static int kbhit()
{
	struct timeval tv;

	fd_set fds;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds); //STDIN_FILENO is 0
	select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
	return FD_ISSET(STDIN_FILENO, &fds);
}

/*----------------------------------------------------------------------------*/
static void set_termio(bool canon) {
	struct termios ios = {0};

	tcgetattr(0, &ios);

	if (canon) {
		ios.c_lflag |= ICANON;
		ios.c_lflag |= ECHO;
		tcsetattr(0, TCSADRAIN, &ios);
	}
	else {
		ios.c_lflag &= ~ICANON;
		ios.c_lflag &= ~ECHO;
		ios.c_cc[VMIN] = 1;
		ios.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &ios);
	}
}

/*----------------------------------------------------------------------------*/
static char _getch() {
	char buf;

	if (read(0, &buf, 1)) return buf;

	return '\0';
}
#endif


/*----------------------------------------------------------------------------*/
static void init_platform(bool interactive) {
	netsock_init();
#if !WIN
	if (interactive) set_termio(false);
#endif
	cross_ssl_load();
}

/*----------------------------------------------------------------------------*/
static void close_platform(bool interactive) {
	netsock_close();
#if !WIN
	if (interactive) set_termio(true);
#endif
	cross_ssl_free();
}

/*----------------------------------------------------------------------------*/
/*																			  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[]) {
	struct raopcl_s *raopcl;
	char *fname = NULL;
	int port = 5000;
	char* passwd = NULL;
	int volume = 50, wait = 0, latency = MS2TS(1000, 44100);
	struct {
		struct hostent *hostent;
		char *name;
		struct in_addr addr;
	} player = { 0 };
	int infile;
	uint8_t *buf;
	int i, n = -1, level = 2;
	enum {STOPPED, PAUSED, PLAYING } status;
	raop_crypto_t crypto = RAOP_CLEAR;
	uint64_t start = 0, start_at = 0, last = 0, frames = 0;
	bool interactive = false, alac = false, pairing = false, repair_resume = false;
	char *secret = NULL, *md = NULL, *et = NULL, *am = NULL, *user_agent = NULL;
	bool auth = false;
	struct in_addr host = { INADDR_ANY };
	char *control_pipe = NULL;
	int control_pipe_fd = -1;
	int current_volume;
	char cmd_buffer[256] = {0};
	int cmd_len = 0;

	for(i = 1; i < argc; i++){
		if(!strcmp(argv[i],"-ntp")){
			FILE *out;

			out = fopen(argv[2], "w");
			fprintf(out, "%" PRIu64, raopcl_get_ntp(NULL));
			fclose(out);
			exit(0);
		}

		if (!strcmp(argv[i],"-p")) {
			port=atoi(argv[++i]);
		} else if (!strcmp(argv[i],"-v")) {
			volume=atoi(argv[++i]);
		} else if (!strcmp(argv[i],"-w")) {
			wait=atoi(argv[++i]);
		} else if(!strcmp(argv[i],"-l")) {
			latency=atoi(argv[++i]);
		} else if (!strcmp(argv[i],"-i")) {
			interactive = true;
		} else if(!strcmp(argv[i],"-s")) {
			secret = argv[++i];
		} else if (!strcmp(argv[i],"-m")) {
			md = argv[++i];
		} else if (!strcmp(argv[i], "-o")) {
			am = argv[++i];
		} else if (!strcmp(argv[i], "-U")) {
			user_agent = argv[++i];
		} else if(!strcmp(argv[i],"-t")) {
			am = argv[++i];
		} else if (!strcmp(argv[i], "-u")) {
			auth = true;
		} else if (!strcmp(argv[i],"-a")) {
			alac = true;
		} else if (!strcmp(argv[i], "-r")) {
			pairing = true;
		} else if (!strcmp(argv[i], "-R")) {
			repair_resume = true;
		} else if(!strcmp(argv[i],"-n")) {
			sscanf(argv[++i], "%" PRIu64, &start);
		} else if (!strcmp(argv[i],"-nf")) {
			FILE *in;

			in = fopen(argv[++i], "r");
			if (!in || !fscanf(in, "%" PRIu64, &start)) {
				LOG_ERROR("Cannot read NTP from file %s", argv[i]);
			}
			fclose(in);
		} else if(!strcmp(argv[i],"-d")) {
			level = atoi(argv[++i]);
			if (level >= sizeof(debug) / sizeof(struct debug_s)) {
				level = sizeof(debug) / sizeof(struct debug_s) - 1;
			}
		} else if(!strcmp(argv[i],"-e")) {
			crypto = RAOP_RSA;
			continue;
		}
		else if (!strcmp(argv[i], "-P")) {
			passwd = argv[++i];
			continue;
		} else if (!strcmp(argv[i], "--control-pipe")) {
			control_pipe = argv[++i];
		} else if(!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) {
			return print_usage(argv);
		} else if (!player.name) {
			player.name = argv[i];
		} else if (!fname) {
			fname=argv[i];
		}
	}

	util_loglevel = debug[level].util;
	raop_loglevel = debug[level].raop;
	main_log = debug[level].main;

	if (!player.name && !pairing) return print_usage(argv);
	if (!fname && !pairing) return print_usage(argv);

	if (!strcmp(fname, "-")) {
		infile = fileno(stdin);
		interactive = false;
	} else if ((infile = open(fname, O_RDONLY)) == -1) {
		LOG_ERROR("cannot open file %s", fname);
		close_platform(interactive);
		exit(1);
	}

#if WIN
	setmode(infile, O_BINARY);
#endif

	init_platform(interactive);

	// Initialize volume tracking
	current_volume = volume;

	// Open control pipe if specified
	if (control_pipe) {
		control_pipe_fd = open(control_pipe, O_RDONLY | O_NONBLOCK);
		if (control_pipe_fd < 0) {
			LOG_ERROR("Cannot open control pipe %s: %s", control_pipe, strerror(errno));
			// Continue without control pipe
		} else {
			LOG_INFO("Control pipe opened: %s", control_pipe);
		}
	}

	// if required, pair with appleTV
	if (pairing) AppleTVpairing(NULL, NULL, &secret);
	
	// create the raop context
	if ((raopcl = raopcl_create(host, 0, 0, NULL, NULL, alac ? RAOP_ALAC : RAOP_PCM, DEFAULT_FRAMES_PER_CHUNK,
								latency, crypto, auth, secret, passwd, et, md, am,
								44100, 16, 2,
								raopcl_float_volume(volume))) == NULL) {
		LOG_ERROR("Cannot init RAOP %p", raopcl);
		close_platform(interactive);
		exit(1);
	}

	if (user_agent) raopcl_set_useragent(raopcl, user_agent);

	// get player's address
	player.hostent = gethostbyname(player.name);
	if (!player.hostent) {
		LOG_ERROR("Cannot resolve name %s", player.name);
		goto exit;
	}

	memcpy(&player.addr.s_addr, player.hostent->h_addr_list[0], player.hostent->h_length);

	// connect to player
	if (!raopcl_connect(raopcl, player.addr, port, true)) {
		LOG_ERROR("Cannot connect to AirPlay device %s:%hu, check firewall & port", inet_ntoa(player.addr), port);
		goto exit;
	}

	latency = raopcl_latency(raopcl);

	LOG_INFO("connected to %s on port %d, player latency is %d ms", inet_ntoa(player.addr),
			 port, (int) TS2MS(latency, raopcl_sample_rate(raopcl)));

	if (start || wait) {
		uint64_t now = raopcl_get_ntp(NULL);

		start_at = (start ? start : now) + MS2NTP(wait) -
					TS2NTP(latency, raopcl_sample_rate(raopcl));

		LOG_INFO("now %u.%u, audio starts at NTP %u.%u (in %u ms)", RAOP_SECNTP(now), RAOP_SECNTP(start_at),
				 (start_at + TS2NTP(latency, raopcl_sample_rate(raopcl)) > now) ?
				  (uint32_t) NTP2MS(start_at - now + TS2NTP(latency, raopcl_sample_rate(raopcl))) :
				  0);

		raopcl_start_at(raopcl, start_at);
	}

	start = raopcl_get_ntp(NULL);
	status = PLAYING;

	buf = malloc(DEFAULT_FRAMES_PER_CHUNK * 4);
	
	do {
		uint64_t playtime, now;

		now = raopcl_get_ntp(NULL);

		if (now - last > MS2NTP(1000)) {
			last = now;
			if (frames && frames > raopcl_latency(raopcl)) {
				LOG_INFO("at %u.%u (%" PRIu64 " ms after start), played %" PRIu64 " ms",
						  RAOP_SECNTP(now), NTP2MS(now - start),
						  TS2MS(frames - raopcl_latency(raopcl), raopcl_sample_rate(raopcl)));
			}
		}

		if (status == PLAYING && raopcl_accept_frames(raopcl)) {
			n = read(infile, buf, DEFAULT_FRAMES_PER_CHUNK * 4);
			if (!n)	continue;
			raopcl_send_chunk(raopcl, buf, n / 4, &playtime);
			frames += n / 4;
		}

		// Check for commands from control pipe
		if (control_pipe_fd >= 0) {
			char c;
			ssize_t bytes_read = read(control_pipe_fd, &c, 1);

			if (bytes_read > 0) {
				if (c == '\n') {
					// Process complete command
					cmd_buffer[cmd_len] = '\0';

					// Volume command: vNN
					if (cmd_buffer[0] == 'v' && cmd_len > 1) {
						int new_volume = atoi(&cmd_buffer[1]);

						if (new_volume >= 0 && new_volume <= 100) {
							if (raopcl_set_volume(raopcl, raopcl_float_volume(new_volume))) {
								current_volume = new_volume;
								LOG_INFO("Volume set to %d via control pipe", new_volume);
							} else {
								LOG_ERROR("Failed to set volume to %d", new_volume);
							}
						} else {
							LOG_ERROR("Invalid volume %d (must be 0-100)", new_volume);
						}
					}
					// Quit command
					else if (cmd_buffer[0] == 'q') {
						LOG_INFO("Quit command received via control pipe");
						goto cleanup;
					}
					// Metadata: T/A/L/G store text, M sends it, I sends an artwork file,
					// P sets progress, C clears everything on the receiver
					else if (cmd_buffer[0] == 'T') meta_set(meta_title, cmd_buffer + 1);
					else if (cmd_buffer[0] == 'A') meta_set(meta_artist, cmd_buffer + 1);
					else if (cmd_buffer[0] == 'L') meta_set(meta_album, cmd_buffer + 1);
					else if (cmd_buffer[0] == 'G') meta_set(meta_genre, cmd_buffer + 1);
					else if (cmd_buffer[0] == 'M') send_metadata(raopcl);
					else if (cmd_buffer[0] == 'I' && cmd_len > 1) {
						meta_set(meta_artwork, cmd_buffer + 1);
						send_artwork(raopcl, meta_artwork);
					}
					else if (cmd_buffer[0] == 'P') {
						unsigned elapsed = 0, duration = 0;
						if (sscanf(cmd_buffer + 1, "%u/%u", &elapsed, &duration) >= 1) {
							if (!raopcl_set_progress_ms(raopcl, elapsed, duration))
								LOG_DEBUG("Progress not sent (not streaming or no progress capability)");
						}
					}
					else if (cmd_buffer[0] == 'C') {
						meta_title[0] = meta_artist[0] = meta_album[0] = meta_genre[0] = meta_artwork[0] = '\0';
						send_metadata(raopcl);
						raopcl_set_progress_ms(raopcl, 0, 0);
					}
					else if (cmd_len > 0) LOG_WARN("Unknown control pipe command '%c'", cmd_buffer[0]);

					cmd_len = 0;  // Reset buffer
				} else if (cmd_len < 255) {
					// Buffer the character
					cmd_buffer[cmd_len++] = c;
				}
			} else if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				LOG_ERROR("Error reading from control pipe: %s", strerror(errno));
				close(control_pipe_fd);
				control_pipe_fd = -1;
			}
		}

		if (interactive && kbhit()) {
			char c = _getch();

			switch (c) {
			case 'p':
				if (status == PLAYING) {
					raopcl_pause(raopcl);
					raopcl_flush(raopcl);
					status = PAUSED;
					LOG_INFO("Pause at : %u.%u", RAOP_SECNTP(raopcl_get_ntp(NULL)));
				}
				break;
			case 's':
				raopcl_stop(raopcl);
				raopcl_flush(raopcl);
				status = STOPPED;
				LOG_INFO("Stopped at : %u.%u", RAOP_SECNTP(raopcl_get_ntp(NULL)));
				break;
			case 'r': {
				uint64_t now = raopcl_get_ntp(NULL);
				uint64_t start_at = now + MS2NTP(200) - TS2NTP(latency, raopcl_sample_rate(raopcl));

				// resume with a full RTSP re-handshake instead of the bare start_at
				if (repair_resume) raopcl_repair(raopcl, true);

				status = PLAYING;
				raopcl_start_at(raopcl, start_at);

				LOG_INFO("Re-started at : %u.%u", RAOP_SECNTP(start_at));

				// the receiver forgot the metadata with the re-handshake
				if (*meta_title || *meta_artist || *meta_album) send_metadata(raopcl);
				if (*meta_artwork) send_artwork(raopcl, meta_artwork);
				}
				break;
			case 'q':
				raopcl_disconnect(raopcl);
				raopcl_destroy(raopcl);
				free(buf);
				close_platform(interactive);
				exit(0);
			case ' ':
				_getch();
				break;
			default: break;
			}
		}

	} while (n || raopcl_is_playing(raopcl));

cleanup:
	free(buf);
	raopcl_disconnect(raopcl);

exit:
	if (control_pipe_fd >= 0) {
		close(control_pipe_fd);
	}
	raopcl_destroy(raopcl);
	close_platform(interactive);
	return 0;
}
