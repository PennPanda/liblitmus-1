#include <sys/time.h>
#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <limits.h>


#include "litmus.h"
#include "common.h"
#include "cache_common.h"

#define PAGE_SIZE (4096)

#define CACHELINE_SIZE 32
#define INTS_IN_CACHELINE (CACHELINE_SIZE/sizeof(int))
#define ARENA_SIZE_KB 64
#define CACHELINES_IN_1KB (1024 / sizeof(cacheline_t))
#define INTS_IN_1KB	(1024 / sizeof(int))
#define NUM_ARENA_ELEM	((ARENA_SIZE_KB * 1024)/sizeof(cacheline_t))

//static int wss = ARENA_SIZE_KB;
static int loops = 10;

static cacheline_t* arena = NULL;

struct timeval tm1, tm2;

typedef int (*walk_t)(cacheline_t *mem, int wss, int write_cycle);
typedef cacheline_t* (*walk_start_t)(int wss);

struct walk_method
{
	const walk_t walk;
	const walk_start_t walk_start;
};

static int sequential_walk(cacheline_t *_mem, int wss, int write_cycle)
{
	int sum = 0, i;
	int* mem = (int*)_mem; /* treat as raw buffer of ints */
	int num_ints = wss * INTS_IN_1KB;

	if (write_cycle > 0) {
		for (i = 0; i < num_ints; i++) {
			if (i % write_cycle == (write_cycle - 1))
				mem[i]++;
			else
				sum += mem[i];
		}
	} else {
		/* sequential access, pure read */
		for (i = 0; i < num_ints; i++)
			sum += mem[i];
	}
	return sum;
}

static cacheline_t* sequential_start(int wss)
{
	static int pos = 0;

	int num_cachelines = wss * CACHELINES_IN_1KB;

	cacheline_t *mem;

	/* Don't allow re-use between allocations.
	 * At most half of the arena may be used
	 * at any one time.
	 */
	//if (num_cachelines * 2 > NUM_ARENA_ELEM)
		//die("static memory arena too small");

	if (pos + num_cachelines > ((wss * 1024)/sizeof(cacheline_t))) {
		/* wrap to beginning */
		mem = arena;
		pos = num_cachelines;
	} else {
		mem = arena + pos;
		pos += num_cachelines;
	}

	return mem;
}

static const struct walk_method sequential_method =
{
	.walk = sequential_walk,
	.walk_start = sequential_start
};


/* Random walk around the arena in cacheline-sized chunks.
   Cacheline-sized chucks ensures the same utilization of each
   hit line as sequential read. (Otherwise, our utilization
   would only be 1/INTS_IN_CACHELINE.) */
static int random_walk(cacheline_t *mem, int wss, int write_cycle)
{
	/* a random cycle among the cache lines was set up by init_arena(). */
	int sum, i, j, next, which_line;

	int numlines = wss * CACHELINES_IN_1KB;

	sum = 0;

	/* contents of arena is structured s.t. offsets are all
	   w.r.t. to start of arena, so compute the initial offset */
	next = mem - arena;

	if (write_cycle == 0) {
		for (i = 0; i < numlines; i++) {
			which_line = next;
			/* every element in the cacheline has the same value */
			for(j = 0; j < INTS_IN_CACHELINE; j++) {
				next = arena[which_line].line[j];
				sum += next;
			}
		}
	}
	
	else {
		int w;
		for (i = 0, w = 0; i < numlines; i++) {
			which_line = next;
			/* count down s.t. next has value of 0th int
			   when the loop exits */
			for(j = 0; j < INTS_IN_CACHELINE; j++) {
				next = arena[which_line].line[j];
				if((w % write_cycle) != (write_cycle - 1)) {
					sum += next;
				}
				else {
					/* Write back what we just read. We can't write back a
					   different value without destroying the walk-cycle, so
					   cast the write to volatile to ensure the write is
					   performed. Note: Volatiles are still cached. */
					((volatile cacheline_t*)arena)[which_line].line[j] = next;
				}
			}
		}
	}
	return sum;
}

static cacheline_t* random_start(int wss)
{
	return arena + randrange(0, ((wss * 1024)/sizeof(cacheline_t)));
}

static const struct walk_method random_method =
{
	.walk = random_walk,
	.walk_start = random_start
};

static volatile int dont_optimize_me = 0;

static void usage(char *error) {
	fprintf(stderr, "Error: %s\n", error);
	fprintf(stderr,
		"Usage:\n"
		"	rt_spin [COMMON-OPTS] WCET PERIOD DURATION\n"
		"	rt_spin [COMMON-OPTS] -f FILE [-o COLUMN] WCET PERIOD\n"
		"	rt_spin -l\n"
		"\n"
		"COMMON-OPTS = [-w] [-s SCALE]\n"
		"              [-p PARTITION/CLUSTER [-z CLUSTER SIZE]] [-c CLASS] [-m CRITICALITY LEVEL]\n"
		"              [-X LOCKING-PROTOCOL] [-L CRITICAL SECTION LENGTH] [-Q RESOURCE-ID]\n"
		"              [-i [start,end]:[start,end]...]\n"
		"\n"
		"WCET and PERIOD are milliseconds, DURATION is seconds.\n"
		"CRITICAL SECTION LENGTH is in milliseconds.\n");
	exit(EXIT_FAILURE);
}

/*
 * returns the character that made processing stop, newline or EOF
 */
static int skip_to_next_line(FILE *fstream)
{
	int ch;
	for (ch = fgetc(fstream); ch != EOF && ch != '\n'; ch = fgetc(fstream));
	return ch;
}

static void skip_comments(FILE *fstream)
{
	int ch;
	for (ch = fgetc(fstream); ch == '#'; ch = fgetc(fstream))
		skip_to_next_line(fstream);
	ungetc(ch, fstream);
}

static void get_exec_times(const char *file, const int column,
			   int *num_jobs,    double **exec_times)
{
	FILE *fstream;
	int  cur_job, cur_col, ch;
	*num_jobs = 0;

	fstream = fopen(file, "r");
	if (!fstream)
		bail_out("could not open execution time file");

	/* figure out the number of jobs */
	do {
		skip_comments(fstream);
		ch = skip_to_next_line(fstream);
		if (ch != EOF)
			++(*num_jobs);
	} while (ch != EOF);

	if (-1 == fseek(fstream, 0L, SEEK_SET))
		bail_out("rewinding file failed");

	/* allocate space for exec times */
	*exec_times = calloc(*num_jobs, sizeof(*exec_times));
	if (!*exec_times)
		bail_out("couldn't allocate memory");

	for (cur_job = 0; cur_job < *num_jobs && !feof(fstream); ++cur_job) {

		skip_comments(fstream);

		for (cur_col = 1; cur_col < column; ++cur_col) {
			/* discard input until we get to the column we want */
			int unused __attribute__ ((unused)) = fscanf(fstream, "%*s,");
		}

		/* get the desired exec. time */
		if (1 != fscanf(fstream, "%lf", (*exec_times)+cur_job)) {
			fprintf(stderr, "invalid execution time near line %d\n",
					cur_job);
			exit(EXIT_FAILURE);
		}

		skip_to_next_line(fstream);
	}

	assert(cur_job == *num_jobs);
	fclose(fstream);
}

static char* progname;

static int loop_once(int wss)
{
	cacheline_t *mem;
	int temp;
	
	//mem = random_method.walk_start(wss);
	//temp = random_method.walk(mem, wss, 0);
	mem = sequential_method.walk_start(wss);
	temp = sequential_method.walk(mem, wss, 0);
	dont_optimize_me = temp;
	
	return dont_optimize_me;
}

static int loop_for(int wss, double exec_time, double emergency_exit)
{
	double last_loop = 0, loop_start;
	int tmp = 0;
	int cur_loop = 0;

	double start = cputime();
	double now = cputime();

	//while (now + last_loop < start + exec_time) {
	while(cur_loop++ < loops) {
		loop_start = now;
		tmp = loop_once(wss);
		now = cputime();
		last_loop = now - loop_start;
		if (emergency_exit && wctime() > emergency_exit) {
			/* Oops --- this should only be possible if the execution time tracking
			 * is broken in the LITMUS^RT kernel. */
			fprintf(stderr, "!!! rtspin/%d emergency exit!\n", getpid());
			fprintf(stderr, "Something is seriously wrong! Do not ignore this.\n");
			break;
		}
	}

	return tmp;
}


static void debug_delay_loop(int wss)
{
	double start, end, delay;

	while (1) {
		for (delay = 0.5; delay > 0.01; delay -= 0.01) {
			start = wctime();
			loop_for(wss, delay, 0);
			end = wctime();
			printf("%6.4fs: looped for %10.8fs, delta=%11.8fs, error=%7.4f%%\n",
			       delay,
			       end - start,
			       end - start - delay,
			       100 * (end - start - delay) / delay);
		}
	}
}

static int job(int wss, double exec_time, double program_end, int lock_od, double cs_length)
{
	double chunk1, chunk2;

	if (wctime() > program_end)
		return 0;
	else {
		if (lock_od >= 0) {
			/* simulate critical section somewhere in the middle */
			chunk1 = drand48() * (exec_time - cs_length);
			chunk2 = exec_time - cs_length - chunk1;

			/* non-critical section */
			loop_for(wss, chunk1, program_end + 1);

			/* critical section */
			litmus_lock(lock_od);
			loop_for(wss, cs_length, program_end + 1);
			litmus_unlock(lock_od);

			/* non-critical section */
			loop_for(wss, chunk2, program_end + 2);
		} else {
			register unsigned long t;
			register unsigned long overhead = get_cyclecount();
			overhead = get_cyclecount() - overhead;
			
			//gettimeofday(&tm1, NULL);
			t = get_cyclecount();
			loop_for(wss, exec_time, program_end + 1);
			t = get_cyclecount() - t;
			printf("%ld cycles (%ld overhead))\n", t, overhead);
			//gettimeofday(&tm2, NULL);
			//printf("%ld\n", ((tm2.tv_sec * 1000000 + tm2.tv_usec) - (tm1.tv_sec * 1000000 + tm1.tv_usec)));
		}
		sleep_next_period();
		return 1;
	}
}

#define OPTSTR "p:c:wl:veo:f:s:q:X:L:Q:vh:k:"
int main(int argc, char** argv)
{
	int ret, i;
	lt_t wcet;
	lt_t period;
	double wcet_ms, period_ms;
	unsigned int priority = LITMUS_NO_PRIORITY;
	int migrate = 0;
	int cluster = 0;
	int opt;
	int wait = 0;
	int test_loop = 0;
	int column = 1;
	const char *file = NULL;
	int want_enforcement = 0;
	double duration = 0, start = 0;
	double *exec_times = NULL;
	double scale = 1.0;
	task_class_t class = RT_CLASS_HARD;
	int cur_job = 0, num_jobs = 0;
	struct rt_task param;
	int n_str, num_int = 0;
	size_t arena_sz;
	int verbose = 0;
	unsigned int job_no;
	int wss;

	/* locking */
	int lock_od = -1;
	int resource_id = 0;
	const char *lock_namespace = "./rtspin-locks";
	int protocol = -1;
	double cs_length = 1; /* millisecond */

	progname = argv[0];

	while ((opt = getopt(argc, argv, OPTSTR)) != -1) {
		switch (opt) {
		case 'w':
			wait = 1;
			break;
		case 'p':
			cluster = atoi(optarg);
			migrate = 1;
			break;
		case 'q':
			priority = atoi(optarg);
			if (!litmus_is_valid_fixed_prio(priority))
				usage("Invalid priority.");
			break;
		case 'c':
			class = str2class(optarg);
			if (class == -1)
				usage("Unknown task class.");
			break;
		case 'e':
			want_enforcement = 1;
			break;
		case 'l':
			loops = atoi(optarg);
			break;
		case 'k':
			wss = atoi(optarg);
			break;
		case 'o':
			column = atoi(optarg);
			break;
		case 'f':
			file = optarg;
			break;
		case 's':
			scale = atof(optarg);
			break;
		case 'X':
			protocol = lock_protocol_for_name(optarg);
			if (protocol < 0)
				usage("Unknown locking protocol specified.");
			break;
		case 'L':
			cs_length = atof(optarg);
			if (cs_length <= 0)
				usage("Invalid critical section length.");
			break;
		case 'Q':
			resource_id = atoi(optarg);
			if (resource_id <= 0 && strcmp(optarg, "0"))
				usage("Invalid resource ID.");
			break;
		case 'v':
			verbose = 1;
			break;
		case ':':
			usage("Argument missing.");
			break;
		case '?':
		default:
			usage("Bad argument.");
			break;
		}
	}

	if (test_loop) {
		debug_delay_loop(wss);
		return 0;
	}

	srand(getpid());

	if (file) {
		get_exec_times(file, column, &num_jobs, &exec_times);

		if (argc - optind < 2)
			usage("Arguments missing.");

		for (cur_job = 0; cur_job < num_jobs; ++cur_job) {
			/* convert the execution time to seconds */
			duration += exec_times[cur_job] * 0.001;
		}
	} else {
		/*
		 * if we're not reading from the CSV file, then we need
		 * three parameters
		 */
		if (argc - optind < 3)
			usage("Arguments missing.");
	}

	wcet_ms   = atof(argv[optind + 0]);
	period_ms = atof(argv[optind + 1]);

	wcet   = ms2ns(wcet_ms);
	period = ms2ns(period_ms);
	if (wcet <= 0)
		usage("The worst-case execution time must be a "
				"positive number.");
	if (period <= 0)
		usage("The period must be a positive number.");
	if (!file && wcet > period) {
		usage("The worst-case execution time must not "
				"exceed the period.");
	}

	if (!file)
		duration  = atof(argv[optind + 2]);
	else if (file && num_jobs > 1)
		duration += period_ms * 0.001 * (num_jobs - 1);

	if (migrate) {
		ret = be_migrate_to_domain(cluster);
		if (ret < 0)
			bail_out("could not migrate to target partition or cluster.");
	}
	
	init_rt_task_param(&param);
	param.exec_cost = wcet;
	param.period = period;
	param.priority = priority;
	param.cls = class;
	param.budget_policy = (want_enforcement) ?
			PRECISE_ENFORCEMENT : NO_ENFORCEMENT;
	if (migrate) {
		param.cpu = domain_to_first_cpu(cluster);
	}
	ret = set_rt_task_param(gettid(), &param);
	if (ret < 0)
		bail_out("could not setup rt task params");
	
	
	arena_sz = wss*1024;
	arena = alloc_arena(arena_sz, 0, 0);
	init_arena(arena, arena_sz);
	
	lock_memory();
	
	ret = init_litmus();
	if (ret != 0)
		bail_out("init_litmus() failed\n");

	start = wctime();
	ret = task_mode(LITMUS_RT_TASK);
	if (ret != 0)
		bail_out("could not become RT task");

	if (protocol >= 0) {
		lock_od = litmus_open_lock(protocol, resource_id, lock_namespace, &cluster);
		if (lock_od < 0) {
			perror("litmus_open_lock");
			usage("Could not open lock.");
		}
	}


	if (wait) {
		ret = wait_for_ts_release();
		if (ret != 0)
			bail_out("wait_for_ts_release()");
		start = wctime();
	}

	if (file) {
		for (cur_job = 0; cur_job < num_jobs; ++cur_job) {
			job(wss, exec_times[cur_job] * 0.001 * scale,
			    start + duration,
			    lock_od, cs_length * 0.001);
		}
	} else {
		do {
			if (verbose) {
				get_job_no(&job_no);
				printf("rtspin/%d:%u @ %.4fms\n", gettid(),
					job_no, (wctime() - start) * 1000);
			}
		} while (job(wss, wcet_ms * 0.001 * scale, start + duration,
			   lock_od, cs_length * 0.001));
	}

	ret = task_mode(BACKGROUND_TASK);
	if (ret != 0)
		bail_out("could not become regular task (huh?)");

	if (file)
		free(exec_times);

	dealloc_arena(arena, arena_sz);
	return 0;
}
