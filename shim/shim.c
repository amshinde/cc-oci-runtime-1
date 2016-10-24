// Copyright (c) 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <assert.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <inttypes.h>
#include <getopt.h>
#include <stdbool.h>

#include "log.h"
#include "shim.h"

/* globals */

/* Pipe used for capturing signal occurence */
int signal_pipe_fd[2];

static char *program_name;

/*!
 * Add file descriptor to the array of polled descriptors
 *
 * \param poll_fds Array of polled fds
 * \param nfds Number of fds present in the array
 * \param fd File descriptor to add
 * \param events Events for the fd that should be polled
 */
void add_pollfd(struct pollfd *poll_fds, nfds_t *nfds, int fd,  short events) {
	struct pollfd pfd = { 0 };

	assert(*nfds < MAX_POLL_FDS);
	pfd.fd = fd;
	pfd.events = events;
	poll_fds[*nfds] = pfd;
	(*nfds)++;
}

/*!
 * Signal handler for the signals that should be caught and 
 * forwarded to the proxy
 *
 * \param signal_no Signal number of the signal caught
 */
static void
signal_handler(int signal_no)
{
	int savedErrno;                     /* In case we change 'errno' */
 
	savedErrno = errno;
	/* Write signal number to pipe, so that the signal can be identfied later */
	if (write(signal_pipe_fd[1], &signal_no, sizeof(signal_no)) == -1 && errno != EAGAIN) {
		return;
	}
	errno = savedErrno;
}

/*!
 * Print formatted message to stderr and exit with EXIT_FAILURE
 *
 * \param format Format that specifies how subsequent arguments are
 *  converted for output
 */
void
err_exit(const char *format, ...)
{
	va_list	args;

	if ( !format) {
		return;
	}
	fprintf(stderr, "%s: ", program_name);
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	exit(EXIT_FAILURE);
}

/*!
 * Set file descriptor as non-blocking
 *
 * \param fd File descriptor to set as non-blocking
 */
void
set_fd_nonblocking(int fd)
{
	int flags;

	if (fd < 0) {
		return;
	}

	flags = fcntl(fd, F_GETFL);
	if (flags == -1) {
		err_exit("Error getting status flags for fd: %s\n", strerror(errno));
	}
	flags |= O_NONBLOCK;

	if (fcntl(fd, F_SETFL, flags) == -1) {
		err_exit("Error setting fd as nonblocking: %s\n", strerror(errno));
	}
}

/*!
 * Store integer as big endian in buffer
 *
 * \param buf Buffer to store the value in
 * \param val Integer to be converted to big endian
 */
void
set_big_endian_32(uint8_t *buf, uint32_t val)
{
        buf[0] = (uint8_t)(val >> 24);
        buf[1] = (uint8_t)(val >> 16);
        buf[2] = (uint8_t)(val >> 8);
        buf[3] = (uint8_t)val;
}

/*!
 * Convert the value stored in buffer to little endian
 *
 * \param buf Buffer storing the big endian value
 */
uint32_t get_big_endian_32(char *buf) {
	return (uint32_t)(buf[0] >> 24 | buf[1] >> 16 | buf[2] >> 8 | buf[3]);
}

/*!
 * Construct message in the hyperstart control format
 * Hyperstart control message format:
 *
 * | ctrl id | length  | payload (length-8)      |
 * | . . . . | . . . . | . . . . . . . . . . . . |
 * 0         4         8                         length
 *
 * \param json Json Payload
 * \param hyper_cmd_type Hyperstart control id
 * \param len Length of the message
 */  
char*
get_hypertart_msg(char *json, int hyper_cmd_type, size_t *len) {
	char *hyperstart_msg = NULL;

	*len = strlen(json) + 8 + 1;
	hyperstart_msg = malloc(sizeof(char) * *len);
	if (! hyperstart_msg) {
		abort();
	}
	set_big_endian_32((uint8_t*)hyperstart_msg, (uint32_t)hyper_cmd_type);
	set_big_endian_32((uint8_t*)hyperstart_msg+4, (uint32_t)(strlen(json) + 8));
	strcpy(hyperstart_msg+8, json);

	return hyperstart_msg;
 }

/*!
 * Send "hyper" payload to cc-proxy. This will be forwarded to hyperstart.
 *
 * \param fd File descriptor to send the message to(should be proxy ctl socket fd)
 * \param Hyperstart cmd id
 * \param json Json payload
 */
void
send_proxy_hyper_message(int fd, int hyper_cmd_type, char *json) {
	char *proxy_hyper_msg = NULL;
	size_t len, offset = 0;
	ssize_t ret;
	char *proxy_command_id = "hyper";

	/* cc-proxy has the following format for "hyper" payload:
	 * {
	 *    "id": "hyper",
	 *    "data": {
	 *        "hyperName": "ping",
	 *        "data": "json payload",
	 *    }
	 * }
	*/

	ret = asprintf(&proxy_hyper_msg,
			"{\"id\":\"%s\",\"data\":{\"hyperName\":\"%d\",\"data\":\"%s\"",
			proxy_command_id, hyper_cmd_type, json);

	if (ret == -1) {
		abort();
	}

	len = strlen(proxy_hyper_msg + 1);

	while (offset < len) {
		ret = write(fd, proxy_hyper_msg + offset, len-offset);
		if (ret == EINTR) {
			continue;
		}
		if (ret <= 0 ) {
			free(proxy_hyper_msg);
			return;
		}
		offset += (size_t)ret;
	}
	free(proxy_hyper_msg);
}

/*!
 * Read signals received and send message in the hyperstart protocol
 * format to outfd
 *
 * \param container_id Container id
 * \param outfd File descriptor to send the message to
 */
void
handle_signals(char *container_id, int outfd) {
	int            sig;
	char          *buf;
	int            ret;
	int            cmd_type;
	struct winsize ws;

	if (! container_id || outfd < 0) {
		return;
	}

	while (read(signal_pipe_fd[0], &sig, sizeof(sig)) != -1) {
		if (sig == SIGWINCH ) {
			cmd_type = WINSIZE;
			if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1) {
				shim_warning("Error getting the current window size: %s\n",
					strerror(errno));
				continue;
			}
			ret = asprintf(&buf, "{\"container_id\":\"%s\", \"row\":\"%d\", \"col\":\"%d\"}",
                                                        container_id, ws.ws_row, ws.ws_col);
			shim_debug("handled SIGWINCH for container %s (row=%d, col=%d)\n",
				container_id, ws.ws_row, ws.ws_col);
		} else {
			cmd_type = KILLCONTAINER;
			ret = asprintf(&buf, "{\"container_id\":\"%s\", \"signal\":\"%d\"}",
                                                        container_id, sig);
			shim_debug("Killed container %s with signal %d\n", container_id, sig);
		}
		if (ret == -1) {
			abort();
		}

		send_proxy_hyper_message(outfd, cmd_type, buf);
		free(buf);
        }
}

/*!
 * Read data from infd and write to outfd
 *
 * \param infd File descriptor to read from
 * \param outfd File descriptor to write to
 */
void
handle_data(int infd, int outfd)
{
	ssize_t    nread;
	char       buf[BUFSIZ];
	int        ret;

	if ( infd < 0 || outfd < 0) {
		return;
	}

	while ((nread = read(infd, buf, BUFSIZ))!= -1) {
		ret = (int)write(outfd, buf, (size_t)nread);
		if (ret == -1) {
			shim_warning("Error writing from fd %d to fd %d: %s\n", infd, outfd, strerror(errno));
			return;
		}
	}
}

long long
parse_numeric_option(char *input) {
	char       *endptr;
	long long   num;

	errno = 0;
	num = strtoll(input, &endptr, 10);
	if ( errno || *endptr ) {
		return -1;
	}
	return num;
}

/*!
 * Print program usage
 */
void
print_usage(void) {
	printf("%s: Usage\n", program_name);
        printf("  -c,  --container-id   Container id\n");
        printf("  -p,  --proxy-sock-fd  File descriptor of the socket connected to cc-proxy\n");
        printf("  -o,  --proxy-io-fd    File descriptor of I/0 fd sent by the cc-proxy\n");
        printf("  -s,  --seq-no         Sequence no for stdin and stdout\n");
        printf("  -e,  --err-seq-no     Sequence no for stderr\n");
        printf("  -d,  --debug          Enable debug output\n");
        printf("  -h,  --help           Display this help message\n");
}

int
main(int argc, char **argv)
{
	struct cc_shim shim = {
		.container_id   =  NULL,
		.proxy_sock_fd  = -1,
		.proxy_io_fd    = -1,
		.io_seq_no      =  0,
		.err_seq_no     =  0,
	};
	struct             sockaddr_un proxy_address;
	char              *container_id = NULL;
	char              *proxy_sock_path;
	struct pollfd      poll_fds[MAX_POLL_FDS] = {{-1}};
	nfds_t             nfds = 0;
	int                ret;
	struct sigaction   sa;
	int                c;
	bool               debug = false;

	program_name = argv[0];

	struct option prog_opts[] = {
		{"container-id", required_argument, 0, 'c'},
		{"proxy-sock-fd", required_argument, 0, 'p'},
		{"proxy-io-fd", required_argument, 0, 'o'},
		{"seq-no", required_argument, 0, 's'},
		{"err-seq-no", required_argument, 0, 'e'},
		{"debug", no_argument, 0, 'd'},
		{"help", no_argument, 0, 'h'},
		{ 0, 0, 0, 0},
	};

	while ((c = getopt_long(argc, argv, "c:p:h", prog_opts, NULL))!= -1) {
		switch (c) {
			case 'c':
				shim.container_id = strdup(optarg);
				break;
			case 'p':
				shim.proxy_sock_fd = (int)parse_numeric_option(optarg);
				if (shim.proxy_sock_fd < 0) {
					err_exit("Invalid value for proxy socket fd\n");
				}
				break;
			case 'o':
				shim.proxy_io_fd = (int)parse_numeric_option(optarg);
				if (shim.proxy_io_fd < 0) {
					err_exit("Invalid value for proxy IO fd\n");
				}
				break;
			case 's':
				shim.io_seq_no = (uint64_t)parse_numeric_option(optarg);
				if (shim.io_seq_no < 0) {
					err_exit("Invalid value for I/O seqence no\n");
				}
				break;
			case 'e':
				shim.err_seq_no = (uint64_t)parse_numeric_option(optarg);
				if (shim.err_seq_no < 0) {
					err_exit("Invalid value for error sequence no\n");
				}
				break;
			case 'd':
				debug = true;
				break;
			case 'h':
				print_usage();
				exit(EXIT_SUCCESS);
			default:
				print_usage();
				exit(EXIT_FAILURE);
		}
	}

	if ( !shim.container_id) {
		err_exit("Missing container id\n");
	}

	if ( shim.proxy_sock_fd == -1) {
		err_exit("Missing proxy socket file descriptor\n");
	}

	if ( shim.proxy_io_fd == -1) {
		err_exit("Missing proxy I/O file descriptor\n");
	}

	if (shim.io_seq_no == 0) {
		err_exit("Missing I/O sequence number\n");
	}

	shim_log_init(debug);

	/* Using self pipe trick to handle signals in the main loop, other strategy
	 * would be to clock signals and use signalfd()/ to handle signals synchronously
	 */
	if (pipe2(signal_pipe_fd, O_NONBLOCK) == -1) {
		err_exit("Error creating pipe\n");
	}

	// Add read end of pipe to pollfd list
	add_pollfd(poll_fds, &nfds, signal_pipe_fd[0], POLLIN | POLLPRI);

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;           /* Restart interrupted reads()s */
	sa.sa_handler = signal_handler;

	/* TODO: Add signal handler for all signals */
	if (sigaction(SIGINT, &sa, NULL) == -1)
		err_exit("sigaction");
	if (sigaction(SIGTERM, &sa, NULL) == -1)
		err_exit("sigaction");
	if (sigaction(SIGWINCH, &sa, NULL) == -1)
		err_exit("sigaction");

	set_fd_nonblocking(STDIN_FILENO);
	add_pollfd(poll_fds, &nfds, STDIN_FILENO, POLLIN | POLLPRI);

#if 0
	// Connect to the cc-proxy AF_UNIX server
	proxy_sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	strcpy(proxy_address.sun_path, proxy_sock_path);
	//block waiting to connect to the cc_proxy
	connect(proxy_sockfd, (struct sockaddr *)&proxy_address, sizeof(proxy_address));

	// Add the proxy socket to the list of pollfds
	add_pollfd(poll_fds, &nfds, proxy_sockfd, POLLIN | POLLPRI);
#endif

	/*	0 =>signal_pipe_fd[0]
		1 =>stdin
		2 =>sockfd
	*/

	while (1) {
		ret = poll(poll_fds, nfds, -1);
		if (ret == -1 && errno != EINTR) {
			err_exit("Error in poll : %s\n", strerror(errno));
		}

		/* check if signal was received first */
		if (poll_fds[0].revents != 0) {
			// TODO:  send to proxy socket fd instead
			handle_signals(container_id, (int)STDOUT_FILENO);
		}

		// check stdin fd
		if (poll_fds[1].revents != 0) {
			// Sending stdin to stdout, this should go to proxy tty fd later
			handle_data((int)STDIN_FILENO, (int)STDOUT_FILENO);
		}

#if 0
		// check for proxy sockfd
		if (poll_fds[2].revents != 0) {
		}
#endif
	}

	free(shim.container_id);
	return 0;
}
