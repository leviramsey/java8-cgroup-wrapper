#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* You can take the coder out of Perl, but... */
#define UNLESS(x) if (!(x))

#define MAXPATH 4096
#define DEFAULT_RAMPERCENTAGE 90
#define ONE_GIG_IN_KB 1048576
#define TWO_GIG_IN_KB 2097152

/* 12 digits plus terminating null */
#define MAX_MEMORY_LEN 13

/* 3 digits plus terminating null */
#define MAX_CPU_LEN 4

#define MAX_CFS_LEN 16

#define MIN_HEAP_ALLOWED 1024

extern char **environ;

static char* orig_java_path();
static int use_cgroup();
static char** unmunge_args(int, char**);
static void print_args(char**);
static void exec_java(char*, int, char**);
static float ram_percentage();
static int kb_ram_cgroup();
static int heap_size_specified(int, char**);
static int arg_is_heap_size(char*);
static int inject_arg(int, char**, char*);
static void inject_heap_size(int, char**);
static void inject_gc(int, char**);
static int arg_is_gc_threads(char*);
static int gc_threads_specified(int, char**);
static int cpus_from_cgroup();

int main(int argc, char **argv) {
	char *origjavapath;
	char **munged_args;
	// -Xms -Xmx -XX:ParallelGCThreads= might be added
	int munged_argc_delta = 3;
	int munged_argc = argc + munged_argc_delta;

	origjavapath = orig_java_path();

	UNLESS (use_cgroup()) {
		execv(origjavapath, argv);
	}

	printf("Deferring to java executable at %s\n", origjavapath);

	// ANSI C requires that the last arg (argv[argc]) be NULL
	munged_args = calloc(munged_argc + 1, sizeof(char *));
	munged_args[0] = argv[0];
	// Copy the rest (including the trailing NULL) at the back of the arg list...
	memcpy(munged_args + munged_argc_delta + 1, argv + 1, argc * sizeof(char *));

	inject_heap_size(munged_argc, munged_args);
	inject_gc(munged_argc, munged_args);

	exec_java(origjavapath, munged_argc, munged_args);
}

static char * orig_java_path() {
	ssize_t maxlen = MAXPATH - 9;
	char *ret = calloc(MAXPATH, sizeof(char));
	ssize_t res;
	ssize_t offset;
	char *cur;

	UNLESS(ret) {
		exit(1);
	}

	res = readlink("/proc/self/exe", ret, maxlen);
	if (res < 0) {
		int error = errno;
		fprintf(stderr, "Could not read /proc/self/exe (error %d), aborting\n", error);
		exit(2);
	}

	if (res > maxlen) {
		fprintf(stderr, "Path too long, aborting\n");
		exit(3);
	}

	offset = MAXPATH - 1;
	cur = ret + offset;
	while (*cur != '/') {
		*cur = 0;
		cur--;
		offset--;
	}

	strncat(ret, "origjava", 8);

	return ret;
}

static void inject_heap_size(int argc, char **munged_args) {
	UNLESS (heap_size_specified(argc, munged_args)) {
		int memory_kb = kb_ram_cgroup();

		// If we couldn't get a cgroup (e.g. running in a non-memory-limited container),
		// let the JVM ergonomics work their magic
		if (memory_kb > 0) {
			float percentage = ram_percentage();
			int heap_kb;
			char *min_heap = calloc(MAX_MEMORY_LEN + 4, sizeof(char));
			char *max_heap = calloc(MAX_MEMORY_LEN + 4, sizeof(char));

			printf("Detected %d KB of memory from cgroup\n", memory_kb);
			// heuristic here is:
			//   heap is percentage of the cgroup memory less JVM overhead.
			//   for cgroup memory greater than 2G, the overhead is a constant 1G
			//   for cgroup memory less than 1G, the overhead is half of the cgroup
			//    memory
			//
			//   Note that this combination is increasing: increasing the cgroup memory
			//   will always increase the heap for a given percentage.
			if (memory_kb < TWO_GIG_IN_KB) {
				heap_kb = (int)(0.50 * percentage * (float)(memory_kb));
			} else {
				heap_kb = (int)(percentage * (float)(memory_kb - ONE_GIG_IN_KB));
			}
			heap_kb = (heap_kb < MIN_HEAP_ALLOWED) ? MIN_HEAP_ALLOWED : heap_kb;

			printf("Setting heap to %d KB\n", heap_kb);

			snprintf(min_heap, MAX_MEMORY_LEN + 4, "-Xms%dk", heap_kb);
			snprintf(max_heap, MAX_MEMORY_LEN + 4, "-Xmx%dk", heap_kb);

			if ((-1 == inject_arg(argc, munged_args, min_heap)) ||
			    (-1 == inject_arg(argc, munged_args, max_heap))) {
				fprintf(stderr, "Failed to inject heap args\n");
				assert(0);
			}
		} else {
			printf("No cgroup memory limit detected.  Using JVM ergonomics\n");
		}
	} else {
		printf("Using found heap arguments\n");
	}
}

static void inject_gc(int argc, char **munged_args) {
	UNLESS (gc_threads_specified(argc, munged_args)) {
		int cpus = cpus_from_cgroup();

		// If we couldn't get a cgroup (e.g. running in a non-CPU limited container),
		// let the JVM ergonomics work their magic
		if (cpus > 0) {
			int gc_threads;
			char *parallel_gc_threads = calloc(MAX_CPU_LEN + 21, sizeof(char));

			printf("Detected %d cores from cgroup\n", cpus);
			// OpenJDK heuristic (5/8 of cores if at least 8, number of cores otherwise)
			// modified to be (non-strictly) increasing based on core count
			if (cpus > 7) {
				gc_threads = (5 * cpus) / 8;
			} else {
				switch (cpus) {
					case 7:
					case 6:
					case 5: gc_threads = 5; break;
					case 4: gc_threads = 4; break;
					case 3: gc_threads = 3; break;
					case 2:	gc_threads = 2; break;
					case 1: gc_threads = 1;	break;
					default:
						printf("Should not have fewer than 1 core detected\n");
						assert(0);
				}
			}

			printf("Using %d GC threads\n", gc_threads);
			
			snprintf(parallel_gc_threads, MAX_CPU_LEN + 21, "-XX:ParallelGCThreads=%d", gc_threads);

			if (-1 == inject_arg(argc, munged_args, parallel_gc_threads)) {
				fprintf(stderr, "Failed to inject GC thread args\n");
				assert(0);
			}
		} else {
			printf("No cgroup CPU limit detected.  Using JVM ergonomics\n");
		}
	} else {
		printf("Using found GC thread arguments\n");
	}
}

static int use_cgroup() {
	char *usecgroupval = getenv("JAVA_USE_CGROUP");

	return (usecgroupval != NULL) && (0 == strncmp(usecgroupval, "yes", 4));
}

static char ** unmunge_args(int argc, char **munged_args) {
	char **cur = munged_args;
	int keep = 0;
	int drop = 0;
	char **ret = NULL;

	assert(munged_args[argc] == NULL);
	while ((keep + drop) < argc) {
		assert(cur == munged_args + (keep + drop));
		if ((*cur == NULL) || (0 == strlen(*cur))) {
			int newdrop = drop + 1;
			if (keep > 0) {
				memmove(munged_args + newdrop, munged_args + drop, keep * sizeof(char *));
			}
			drop = newdrop;
		} else {
			keep++;
		}
		cur++;
	}

	ret = munged_args + drop;

	return ret;
}

static void print_args(char **args) {
	char **cur = args;

	while ((cur != NULL) && (*cur != NULL)) {
		printf("%s ", *cur);
		cur++;
	}
	printf("\n");
}

static void exec_java(char *javaexepath, int argc, char **args) {
	char **unmunged = unmunge_args(argc, args);
	print_args(unmunged);

	execv(javaexepath, unmunged);
}

static float ram_percentage() {
	char *rampercentage = getenv("JAVA_HEAP_PERCENTAGE");
	int percentage;
	if (rampercentage == NULL) {
		printf("Using default RAM percentage of %d%%\n", DEFAULT_RAMPERCENTAGE);
		percentage = DEFAULT_RAMPERCENTAGE;
	} else {
		percentage = atoi(rampercentage);
	}

	percentage = (percentage > 100) ? 100 : percentage;
	percentage = (percentage < 1) ? 1 : percentage;

	return (float)percentage / 100.0;
}

static int kb_ram_cgroup() {
	FILE *memory_file = fopen("/sys/fs/cgroup/memory/memory.limit_in_bytes", "r");
	char buf[MAX_MEMORY_LEN];
	int nr;
	int ret;

	if (memory_file == NULL) {
		return -1;
	}

	memset(buf, 0, MAX_MEMORY_LEN);
	nr = fread(buf, 1, MAX_MEMORY_LEN, memory_file);
	if (buf[nr-1] == '\n') {
		buf[nr-1] = '\0';
		ret = (int)(atol(buf) / 1024L);
	} else {
		printf("Detected at least 1 TB of RAM, cap is kicking in\n");
		ret = 1000000000;
	}

	ret = (ret > 976562499) ? 976562500 : ret;
	ret = (ret < 1024) ? 1024 : ret;

	fclose(memory_file);

	return ret;
}

static int cpus_from_cgroup() {
	FILE *cgroup_file;
	char buf[MAX_CFS_LEN];
	int nr;
	int ret = -1;

	// First try CFS
	cgroup_file = fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r");
	if (cgroup_file != NULL) {
		long quota;
		long period;
		memset(buf, 0, MAX_CFS_LEN);
		nr = fread(buf, 1, MAX_CFS_LEN, cgroup_file);
		if (buf[nr-1] == '\n') {
			buf[nr-1] = '\0';
			quota = atol(buf);
		} else {
			printf("Detected very large quota, cap is kicking in\n");
			quota = 100000000000000L;
		}

		fclose(cgroup_file);

		if (quota > 0) {
			cgroup_file = fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r");
			memset(buf, 0, MAX_CFS_LEN);
			nr = fread(buf, 1, MAX_CFS_LEN, cgroup_file);
			if (buf[nr-1] == '\n') {
				buf[nr-1] = '\0';
				period = atol(buf);
			} else {
				printf("Detected very large period, cap is kicking in\n");
				period = 100000000000000L;
			}

			fclose(cgroup_file);
			ret = (int)(quota/period);
		}
	}

	if (ret == -1) {
		// CFS didn't work, so fall back on shares -- convention is 1024 * cpus
		cgroup_file = fopen("/sys/fs/cgroup/cpu/cpu.shares", "r");

		if (cgroup_file == NULL) {
			// Give up...
			return -1;
		}

		memset(buf, 0, MAX_CFS_LEN);
		nr = fread(buf, 1, 8, cgroup_file);
		if (buf[nr-1] == '\n') {
			buf[nr-1] = '\0';
			ret = atoi(buf) / 1024;
		} else {
			printf("Detected very large CPU share, cap is kicking in\n");
			ret = 10000;
		}
		fclose(cgroup_file);
	}

	ret = (ret > 999) ? 999 : ret;
	ret = (ret < 1) ? 1 : ret;

	return ret;
}

static int heap_size_specified(int argc, char **args) {
	int i = 1;
	char **cur = args + i;

	for (; i < argc; i++, cur++) {
		if (arg_is_heap_size(*cur)) {
			return 1;
		}

		if ((NULL != *cur) && (**cur != '-')) {
			break;
		}
	}

	return 0;
}

static int arg_is_heap_size(char *arg) {
	if (arg != NULL) {
		if ((arg[0] == '-') &&
	    	    (arg[1] == 'X') &&
		    (arg[2] == 'm') &&
	    	    ((arg[3] == 's') || (arg[3] == 'x'))) {
			return 1;
		}
	}
	return 0;
}

static int gc_threads_specified(int argc, char **args) {
	int i = 1;
	char **cur = args + i;

	for (; i < argc; i++, cur++) {
		if (arg_is_gc_threads(*cur)) {
			return 1;
		}

		if ((NULL != *cur) && ('-' != **cur)) {
			break;
		}
	}
	
	return 0;
}

static int arg_is_gc_threads(char *arg) {
	if (arg != NULL) {
		if ((arg[0] ==  '-') &&
		    (arg[1] ==  'X') &&
		    (arg[2] ==  'X') &&
		    (arg[3] ==  ':') &&
		    (arg[4] ==  'P') &&
		    (arg[5] ==  'a') &&
		    (arg[6] ==  'r') &&
		    (arg[7] ==  'a') &&
		    (arg[8] ==  'l') &&
		    (arg[9] ==  'l') &&
		    (arg[10] == 'e') &&
		    (arg[11] == 'l') &&
		    (arg[12] == 'G') &&
		    (arg[13] == 'C') &&
		    (arg[14] == 'T') &&
		    (arg[15] == 'h') &&
		    (arg[16] == 'r') &&
		    (arg[17] == 'e') &&
		    (arg[18] == 'a') &&
		    (arg[19] == 'd') &&
		    (arg[20] == 's') &&
		    (arg[21] == '=')) {
			return 1;
		    }
	}
	return 0;
}

static int inject_arg(int argc, char **argv, char *inject) {
	char **cur = argv;
	for (int i = 0; i < argc; i++, cur++) {
		if ((NULL == *cur) || (0 == strlen(*cur))) {
			printf("Adding %s to command line\n", inject);
			*cur = inject;
			return i;
		}
	}
	return -1;
}
