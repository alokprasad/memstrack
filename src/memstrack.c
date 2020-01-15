#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <malloc.h>

#include <sys/resource.h>

#include "perf-handler.h"
#include "ftrace-handler.h"
#include "memstrack.h"
#include "tracing.h"
#include "proc.h"

int m_debug;
int m_human;
int m_perf;
int m_ftrace;
int m_json;
int m_slab;
int m_page;
int m_show_misc;
int m_throttle = 100;
int m_summary;
int m_sort_alloc = 1;
int m_sort_peak = 0;

unsigned int page_size;

char* m_perf_base;

int m_log (int level, const char *__restrict fmt, ...){
	if (!m_debug && level <= LOG_LVL_DEBUG) {
		return 0;
	}
	int ret;
	va_list args;
	va_start (args, fmt);
	if (level >= LOG_LVL_WARN) {
		ret = vfprintf(stderr, fmt, args);
	} else {
		ret = vfprintf(stdout, fmt, args);
	}
	va_end (args);
	return ret;
}

static void do_exit() {
	if (m_ftrace) {
		ftrace_handling_clean();
	}
	if (m_perf) {
		perf_handling_clean();
	}
	final_report(&TaskMap, 0);
	exit(0);
}

static void on_signal(int signal) {
	log_debug("Exiting on signal %d\n", signal);
	do_exit();
}

static void do_process_perf() {
	perf_handling_start();
	while (1) {
		perf_handling_process();
	}
}

static void do_process_ftrace() {
	while (1) {
		ftrace_handling_process();
	}
}

static struct option long_options[] =
{
	/* These options set a flag. */
	{"ftrace",		no_argument,	&m_ftrace,	1},
	{"perf",		no_argument,	&m_perf,		1},
	{"slab",		no_argument,	&m_slab,		1},
	{"page",		no_argument,	&m_page,		1},
	{"json",		no_argument,	&m_json,		1},
	{"show-misc",		no_argument,	&m_show_misc,	1},
	{"summary",		no_argument,	&m_summary,	1},
	{"debug",		no_argument,		0,		'd'},
	// {"human-readable",	no_argument,		0,		'h'},
	// {"trace-base",	required_argument,	0,		'b'},
	{"throttle",		required_argument,	0,		't'},
	{"sort-by",		required_argument,	0,		's'},
	{"help",		no_argument,		0,		'?'},
	{0, 0, 0, 0}
};


static void display_usage() {
	log_info("Usage: memstrack [OPTION]... \n");
	log_info("    --debug		Print debug messages. \n");
	log_info("    --ftrace		Use ftrace for tracing, poor performance but should always work. \n");
	log_info("    --perf		Use binary perf for tracing, may require CONFIG_FRAME_POINTER enabled on older kernel (before 5.1). \n");
	log_info("    --page		Collect page usage statistic. \n");
	log_info("    --slab		Collect slab cache usage statistic. \n");
	// log_info("    --human-readable	Print sizes in a human reable way, eg bytes_alloc: 1048576 => 1M \n");
	log_info("    --json		Format result as json. \n");
	// log_info("    --trace-base [DIR]	Use a different tracing mount path. \n");
	log_info("    --show-misc	Generate a current memory usage summary report on start. \n");
	log_info("    --throttle [PERCENTAGE] \n");
	log_info("    			Only print callsites consuming [PERCENTAGE] percent of total memory consumed. \n");
	log_info("    			expects a number between 0 to 100. Useful to filter minor noises. \n");
	log_info("    --sort-by {peak|alloc} \n");
	log_info("    			How should the stack be sorted, by the peak usage or allocation statuc on tracer exit. \n");
	log_info("    			Defaults to peak. \n");
	log_info("    --summary \n");
	log_info("    			Generate a summary instead of detailed stack info. \n");
	log_info("    --help 		Print this message. \n");
}

static void tune_glibc() {
	mallopt(M_TOP_PAD, 4096);
	mallopt(M_TRIM_THRESHOLD, 4096);
}

static void set_high_priority() {
	int which = PRIO_PROCESS;
	int priority = -20;
	int ret;
	id_t pid;

	pid = getpid();
	ret = setpriority(which, pid, priority);

	if (ret) {
		log_error("Failed to set high priority with %s.\n", strerror(ret));
	}
}


int main(int argc, char **argv) {
	tune_glibc();
	page_size = getpagesize();

	mem_tracing_init();

	while (1) {
		int opt;
		int option_index = 0;

		opt = getopt_long(argc, argv, "db:h", long_options, &option_index);

		/* Detect the end of the options. */
		if (opt == -1)
			break;

		switch (opt)
		{
			case 0:
				// Flag setted, nothing to do
				break;
			case 'd':
				m_debug = 1;
				break;
			case 'h':
				m_human = 1;
				break;
			case 'b':
				m_perf_base = (char*)calloc(sizeof(char), strlen(optarg) + 1);
				strcpy(m_perf_base, optarg);
				break;
			case 't':
				m_throttle = atoi(optarg);
				if (m_throttle < 0 || m_throttle > 100) {
					log_error("--throttle expects an integer between 0 - 100!\n");
					exit(1);
				}
				break;
			case 's':
				if (strcmp(optarg, "peak")) {
					m_sort_peak = 1;
					m_sort_alloc = 0;
				} else if (strcmp(optarg, "alloc")) {
					m_sort_peak = 0;
					m_sort_alloc = 1;
				}
				break;
			case '?':
				display_usage();
				exit(0);
			default:
				display_usage();
				exit(1);
		}
	}

	if (m_show_misc) {
		print_slab_usage();
	}

	if (m_perf && m_ftrace) {
		log_error("Can't have --ftrace and --perf set together!\n");
		exit(EINVAL);
	}

	if (!m_perf && !m_ftrace) {
		m_perf = 1;  // Use perf by default
	}

	if (!m_page && !m_slab) {
		log_error("At least one of --page and --slab is required.\n");
		exit(EINVAL);
	}

	if (getuid() != 0) {
		log_error("This tool requires root permission to work.\n");
		exit(EPERM);
	}

	int err;
	if (m_perf) {
		err = perf_handling_init();
		if (err) {
			log_error("Failed initializing perf event buffer: %s!", strerror(err));
			exit(err);
		}
		signal(SIGINT, on_signal);
		do_process_perf();
	} else if (m_ftrace) {
		err = ftrace_handling_init();
		if (err) {
			log_error("Failed to open ftrace: %s!", strerror(err));
			exit(err);
		}
		signal(SIGINT, on_signal);
		do_process_ftrace();
	}
	do_exit();
}