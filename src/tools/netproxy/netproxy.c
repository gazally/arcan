/*
 * Simple implementation of a client/server proxy.
 */
#include <arcan_shmif.h>
#include <arcan_shmif_server.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <ctype.h>

#include <sys/socket.h>
#include <netdb.h>

#include "a12.h"
#include "a12_int.h"
#include "a12_helper.h"

/*
 * pull in from arcan codebase, chacha based CSPRNG
 */
extern void arcan_random(uint8_t* dst, size_t ntc);

/*
 * Since we pull in some functions from the main arcan codebase, we need to
 * define this symbol, used if the random function has problems with entropy etc.
 */
void arcan_fatal(const char* msg, ...)
{
	va_list args;
	va_start(args, msg);
	vfprintf(stderr, msg, args);
	va_end(args);
	exit(EXIT_FAILURE);
}

static void fork_a12srv(struct a12_state* S, int fd)
{
	pid_t fpid = fork();
	if (fpid == 0){
/* Split the log output on debug so we see what is going on */
#ifdef _DEBUG
		char buf[sizeof("cl_log_xxxxxx.log")];
		snprintf(buf, sizeof(buf), "cl_log_%.6d.log", (int) getpid());
		FILE* fpek = fopen(buf, "w+");
		if (fpek){
			a12_set_trace_level(a12_trace_targets, fpek);
		}
		fclose(stderr);
#endif
		int rc = a12helper_a12srv_shmifcl(S, NULL, fd, fd);
		exit(rc < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
	}
	else if (fpid == -1){
		a12int_trace(A12_TRACE_SYSTEM, "couldn't fork/dispatch, ulimits reached?\n");
		a12_channel_close(S);
		close(fd);
		return;
	}
	else {
/* just ignore and return to caller */
		a12_channel_close(S);
		close(fd);
	}
}

static void single_a12srv(struct a12_state* S, int fd)
{
	a12helper_a12srv_shmifcl(S, NULL, fd, fd);
}

static void a12cl_dispatch(struct a12_context_options* opts,
	struct a12_state* S, struct shmifsrv_client* cl, int fd)
{
/* note that the a12helper will do the cleanup / free */
	a12helper_a12cl_shmifsrv(S, cl, fd, fd, (struct a12helper_opts){
		.dirfd_temp = -1,
		.dirfd_cache = -1,
		.redirect_exit = opts->redirect_exit
	});
}

static void fork_a12cl_dispatch(struct a12_context_options* opts,
	struct a12_state* S, struct shmifsrv_client* cl, int fd)
{
	pid_t fpid = fork();
	if (fpid == 0){
/* missing: extend sandboxing, close stdio */
		a12helper_a12cl_shmifsrv(S, cl, fd, fd, (struct a12helper_opts){
			.dirfd_temp = -1,
			.dirfd_cache = -1,
			.redirect_exit = opts->redirect_exit
		});
		exit(EXIT_SUCCESS);
	}
	else if (fpid == -1){
		fprintf(stderr, "fork_a12cl() couldn't fork new process, check ulimits\n");
		shmifsrv_free(cl);
		a12_channel_close(S);
		return;
	}
	else {
/* just ignore and return to caller */
		a12int_trace(A12_TRACE_SYSTEM, "client handed off to %d", (int)fpid);
		a12_channel_close(S);

/* this will leak right now as the _free actually disconnects the client
 * which we don't want to do but the fix requires changes to the library
 * shmifsrv_free(cl);
 */
		close(fd);
	}
}

static int get_cl_fd(struct addrinfo* addr)
{
	int clfd;

/* there might be many possible candidates, try them all */
	for (struct addrinfo* cur = addr; cur; cur = cur->ai_next){
		clfd = socket(cur->ai_family, cur->ai_socktype, cur->ai_protocol);
		if (-1 == clfd)
			continue;

		if (connect(clfd, cur->ai_addr, cur->ai_addrlen) != -1){
			char hostaddr[NI_MAXHOST];
			char hostport[NI_MAXSERV];
			int ec = getnameinfo(cur->ai_addr, cur->ai_addrlen,
				hostaddr, sizeof(hostaddr), hostport, sizeof(hostport),
				NI_NUMERICSERV | NI_NUMERICHOST
			);

			if (!ec)
				a12int_trace(A12_TRACE_SYSTEM, "connected to: %s:%s", hostaddr, hostport);
			else
				a12int_trace(A12_TRACE_SYSTEM, "connected, couldn't resolve address");
			break;
		}

		close(clfd);
		clfd = -1;
	}

	return clfd;
}

static int a12_connect(struct a12_context_options* opts,
	const char* cpoint, const char* host_str, const char* port_str,
	void (*dispatch)(struct a12_context_options* opts,
	struct a12_state* S, struct shmifsrv_client* cl, int fd))
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);

	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	struct addrinfo* addr = NULL;

	int ec = getaddrinfo(host_str, port_str, &hints, &addr);
	if (ec){
		fprintf(stderr, "couldn't resolve address: %s\n", gai_strerror(ec));
		return EXIT_FAILURE;
	}

	int shmif_fd = -1;
	for(;;){
		struct shmifsrv_client* cl =
			shmifsrv_allocate_connpoint(cpoint, NULL, S_IRWXU, shmif_fd);

		if (!cl){
			freeaddrinfo(addr);
			fprintf(stderr, "couldn't open connection point\n");
			return EXIT_FAILURE;
		}

/* first time, extract the connection point descriptor from the connection */
		if (-1 == shmif_fd)
			shmif_fd = shmifsrv_client_handle(cl);

		struct pollfd pfd = {.fd = shmif_fd, .events = POLLIN | POLLERR | POLLHUP};

/* wait for a connection */
		for(;;){
			int pv = poll(&pfd, 1, -1);
			if (-1 == pv){
				if (errno != EINTR && errno != EAGAIN){
					freeaddrinfo(addr);
					shmifsrv_free(cl);
					fprintf(stderr, "error while waiting for a connection\n");
					return EXIT_FAILURE;
				}
				continue;
			}
			else if (pv)
				break;
		}

/* accept it (this will mutate the client_handle internally) */
		shmifsrv_poll(cl);

/* open remote connection
 *
 * this could be done inside of the dispatch to get faster burst management
 * at the cost of worse error reporting, but also allow a sleep-retry kind
 * of loop in order to have the client 'wake up' when server becomes avail. */
		int fd = get_cl_fd(addr);
		if (-1 == fd){
/* question if we should retry connecting rather than to kill the server */
			shmifsrv_free(cl);
			continue;
		}

		struct a12_state* state = a12_open(opts);
		if (!state){
			freeaddrinfo(addr);
			shmifsrv_free(cl);
			close(fd);
			fprintf(stderr, "couldn't build a12 state machine\n");
			return EXIT_FAILURE;
		}

/* wake the client */
		a12int_trace(A12_TRACE_SYSTEM, "local connection found, forwarding to dispatch");
		dispatch(opts, state, cl, fd);
	}

	return EXIT_SUCCESS;
}

static int a12_listen(struct a12_context_options* opts, const char* addr_str,
	const char* port_str, void (*dispatch)(struct a12_state* S, int fd))
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);

/* normal address setup foreplay */
	struct addrinfo* addr = NULL;
	struct addrinfo hints = {
		.ai_flags = AI_PASSIVE
	};
	int ec = getaddrinfo(addr_str, port_str, &hints, &addr);
	if (ec){
		fprintf(stderr, "couldn't resolve address: %s\n", gai_strerror(ec));
		return EXIT_FAILURE;
	}

	char hostaddr[NI_MAXHOST];
	char hostport[NI_MAXSERV];
	ec = getnameinfo(addr->ai_addr, addr->ai_addrlen,
		hostaddr, sizeof(hostaddr), hostport, sizeof(hostport),
		NI_NUMERICSERV | NI_NUMERICHOST
	);

	if (ec){
		fprintf(stderr, "couldn't retrieve name: %s\n", gai_strerror(ec));
		freeaddrinfo(addr);
		return EXIT_FAILURE;
	}

/* bind / listen */
	int sockin_fd = socket(addr->ai_family, SOCK_STREAM, 0);
	if (-1 == sockin_fd){
		fprintf(stderr, "couldn't create socket: %s\n", strerror(ec));
		freeaddrinfo(addr);
		return EXIT_FAILURE;
	}

	int optval = 1;
	setsockopt(sockin_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	setsockopt(sockin_fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

	ec = bind(sockin_fd, addr->ai_addr, addr->ai_addrlen);
	if (ec){
		fprintf(stderr,
			"error binding (%s:%s): %s\n", hostaddr, hostport, strerror(errno));
		freeaddrinfo(addr);
		close(sockin_fd);
		return EXIT_FAILURE;
	}

	ec = listen(sockin_fd, 5);
	if (ec){
		fprintf(stderr,
			"couldn't listen (%s:%s): %s\n", hostaddr, hostport, strerror(errno));
		close(sockin_fd);
		freeaddrinfo(addr);
	}
	else
		fprintf(stdout, "listening on: %s:%s\n", hostaddr, hostport);

/* build state machine, accept and dispatch */
	for(;;){
		struct sockaddr_storage in_addr;
		socklen_t addrlen = sizeof(addr);

		int infd = accept(sockin_fd, (struct sockaddr*) &in_addr, &addrlen);
		struct a12_state* ast = a12_build(opts);
		if (!ast){
			fprintf(stderr, "Couldn't allocate client state machine\n");
			close(infd);
			continue;
		}

		dispatch(ast, infd);
	}

	return EXIT_SUCCESS;
}

static int show_usage(const char* msg)
{
	fprintf(stderr, "%s%sUsage:\n"
	"\tForward local arcan applications: arcan-net -s connpoint host port\n"
	"\tBridge remote arcan applications: arcan-net -l port [ip]\n\n"
	"Forward-local options:\n"
	"\t-X        \t Disable EXIT-redirect to ARCAN_CONNPATH env (if set)\n\n"
	"Options:\n"
	"\t-t single- client (no fork/mt)\n"
	"\t-d bitmap \t set trace bitmap (see below)\n"
	"\nTrace groups (stderr):\n"
	"\tvideo:1      audio:2      system:4    event:8      transfer:16\n"
	"\tdebug:32     missing:64   alloc:128  crypto:256    vdetail:512\n"
	"\tbtransfer:1024\n\n"
/*
 * "Special:\n"
	"\tInherit-bridge (ARCAN_SOCKIN_FD env, ARCAN_ARG env, -t ignored)\n\n"
 */
		, msg, msg ? "\n" : ""
	);
	return EXIT_FAILURE;
}

enum mt_mode {
	MT_SINGLE = 0,
	MT_FORK = 1
};

int main(int argc, char** argv)
{
	const char* cp = NULL;
	const char* listen_port = NULL;
	int server_mode = -1;
	enum mt_mode mt_mode = MT_FORK;

	struct a12_context_options* opts =
		a12_sensitive_alloc(sizeof(struct a12_context_options));

/* set this as default, so the remote side can't actually close */
	opts->redirect_exit = getenv("ARCAN_CONNPATH");

/* setup default / junk authentication key */
	a12_plain_kdf(NULL, opts);

	size_t i = 1;
/* mode defining switches and shared switches */
	for (; i < argc; i++){
		if (argv[i][0] != '-')
			break;

		if (strcmp(argv[i], "-d") == 0){
			if (i == argc - 1)
				return show_usage("-d without trace value argument");
			unsigned long val = strtoul(argv[++i], NULL, 10);
			a12_set_trace_level(val, stderr);
		}

/* a12 client, shmif server */
		if (strcmp(argv[i], "-s") == 0){
			if (server_mode != -1)
				return show_usage("Mixed -s and -l or multiple -s or -l arguments");

			server_mode = 1;
			if (i >= argc - 1)
				return show_usage("Invalid arguments, -s without room for ip");
			cp = argv[++i];
			for (size_t ind = 0; cp[ind]; ind++)
				if (!isalnum(cp[ind]))
					return show_usage("Invalid character in connpoint [a-Z,0-9]");
			continue;
		}
/* a12 server, shmif client */
		if (strcmp(argv[i], "-l") == 0){
			if (server_mode != -1)
				return show_usage("Mixed -s and -l or multiple -s or -l arguments");
			server_mode = 0;

			if (i == argc - 1)
				return show_usage("-l without room for port argument");

			listen_port = argv[++i];
			for (size_t ind = 0; listen_port[ind]; ind++)
				if (listen_port[ind] < '0' || listen_port[ind] > '9')
					return show_usage("Invalid values in port argument");
		}

		if (strcmp(argv[i], "-t") == 0){
			mt_mode = MT_SINGLE;
		}

		if (strcmp(argv[i], "-X") == 0){
			opts->redirect_exit = NULL;
		}
	}

/* parsing done, route to the right connection mode */
	if (server_mode == -1)
		return show_usage("No mode specified, please use -s or -l form");

/* Special "chain-exec" from remoting / encode where the shmif connection
 * is provided for us. In this mode we need to flip who is initiating the
 * connection (client vs. server), and thus requires us to finish output-
 * segment support and two shmifsrv wrappers - one that can construct an
 * arcan_shmif_cont from a shmifsrv_client, and one that can construct a
 * shmifsrv_client/arcan_shmif_cont from malloc/socketpair(), or refactor
 * a12int_helper_srv to take hooks for the vbuffer/abuffer/enqueue/dequeue
	if (getenv("ARCAN_SOCKIN_FD")){
	}
 */

	if (server_mode == 0){
		char* host = i < argc ? argv[i] : NULL;
		switch (mt_mode){
		case MT_SINGLE:
			return a12_listen(opts, host, listen_port, single_a12srv);
		case MT_FORK:
			return a12_listen(opts, host, listen_port, fork_a12srv);
		break;
		default:
			return EXIT_FAILURE;
		break;
		}
	}

	if (i != argc - 2)
		return show_usage("last two arguments should be host and port");

	switch (mt_mode){
	case MT_SINGLE:
		return a12_connect(opts, cp, argv[i], argv[i+1], a12cl_dispatch);
	break;
	case MT_FORK:
		return a12_connect(opts, cp, argv[i], argv[i+1], fork_a12cl_dispatch);
	break;
	default:
		return EXIT_FAILURE;
	break;
	}
}
