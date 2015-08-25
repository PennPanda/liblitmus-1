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
#define WSS 		1024
#define INTS_IN_CACHELINE (CACHELINE_SIZE/sizeof(int))
#define CACHELINES_IN_1KB (1024 / sizeof(cacheline_t))
#define INTS_IN_1KB	(1024 / sizeof(int))

static cacheline_t* arena = NULL;

static int random_walk(cacheline_t *mem, int write_cycle)
{
	/* a random cycle among the cache lines was set up by init_arena(). */
	int sum, i, next;

	int numlines = WSS * CACHELINES_IN_1KB;

	sum = 0;

	next = mem - arena;

	if (write_cycle == 0) {
		for (i = 0; i < numlines; i++) {
			/* every element in the cacheline has the same value */
			next = arena[next].line[0];
			sum += next;
		}
	}
	
	else {
		int w, which_line;
		for (i = 0, w = 0; i < numlines; i++) {
			which_line = next;
			next = arena[next].line[0];
			if((w % write_cycle) != (write_cycle - 1)) {
				sum += next;
			}
			else {
				((volatile cacheline_t*)arena)[which_line].line[0] = next;
			}
		}
	}
	return sum;
}

static cacheline_t* random_start(void)
{
	return arena + randrange(0, ((WSS * 1024)/sizeof(cacheline_t)));
}

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
		"              [-p PARTITION/CLUSTER [-z CLUSTER SIZE]] [-m CRITICALITY LEVEL]\n"
		"              [-k WSS] [-l LOOPS] [-b BUDGET]\n"
		"\n"
		"WCET and PERIOD are milliseconds, DURATION is seconds.\n");
	exit(EXIT_FAILURE);
}

static int loop_once(void)
{
	cacheline_t *mem;
	int temp;
	
	mem = random_start();
	temp = random_walk(mem, 1);
    
	//mem = sequential_start(wss);
	//temp = sequential_walk(mem, wss, 0);
	dont_optimize_me = temp;
	
	return dont_optimize_me;
}

static int job(double exec_time, double program_end)
{
	if (wctime() > program_end)
		return 0;
	else {
		double last_loop = 0, loop_start;
		double start = cputime();
		double now = cputime();
		while(now + last_loop < start + exec_time) {
			loop_start = now;
			loop_once();
			now = cputime();
			last_loop = now - loop_start;
		}
		sleep_next_period(); 
   		return 1;
	}
}

#define OPTSTR "p:wm:i:k:"
int main(int argc, char** argv)
{
	int ret, i;
	lt_t wcet, period, budget;
	double wcet_ms, period_ms;
	unsigned int priority = LITMUS_NO_PRIORITY;
	int migrate = 0;
	int cluster = 0;
	int opt;
	int wait = 0;
	double duration = 0, start = 0;
	struct rt_task param;
	struct mc2_task mc2_param;
	struct reservation_config config;
	int res_type = PERIODIC_POLLING;
	size_t arena_sz;
	
	/* default for reservation */
	config.id = 0;
	config.priority = LITMUS_NO_PRIORITY; /* use EDF by default */
	config.cpu = -1;
	mc2_param.crit = CRIT_LEVEL_C;
	
	while ((opt = getopt(argc, argv, OPTSTR)) != -1) {
		switch (opt) {
		case 'w':
			wait = 1;
			break;
		case 'p':
			cluster = atoi(optarg);
			migrate = 1;
			config.cpu = cluster;
			break;
		case 'm':
			mc2_param.crit = atoi(optarg);
			if ((mc2_param.crit >= CRIT_LEVEL_A) && (mc2_param.crit <= CRIT_LEVEL_C)) {
				res_type = PERIODIC_POLLING;
			}
			else
				usage("Invalid criticality level.");
			break;
		case 'i':
			config.priority = atoi(optarg);
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
	srand(getpid());
	/* We need three parameters */
	if (argc - optind < 3)
		usage("Arguments missing.");

	wcet_ms   = atof(argv[optind + 0]);
	period_ms = atof(argv[optind + 1]);

	wcet   = ms2ns(wcet_ms);
	period = ms2ns(period_ms);
	budget = ms2ns(period_ms);
	if (wcet <= 0)
		usage("The worst-case execution time must be a "
				"positive number.");
	if (period <= 0)
		usage("The period must be a positive number.");
	if (wcet > period) {
		usage("The worst-case execution time must not "
				"exceed the period.");
	}

	duration  = atof(argv[optind + 2]);
	
	if (migrate) {
		ret = be_migrate_to_domain(cluster);
		if (ret < 0)
			bail_out("could not migrate to target partition or cluster.");
	}

	/* reservation config */
	config.id = gettid();
	config.polling_params.budget = budget;
	config.polling_params.period = period;
	config.polling_params.offset = 0;
	config.polling_params.relative_deadline = 0;
	
	if (config.polling_params.budget > config.polling_params.period) {
		usage("The budget must not exceed the period.");
	}
	
	/* create a reservation */
	ret = reservation_create(res_type, &config);
	if (ret < 0) {
		bail_out("failed to create reservation.");
	}
	
	init_rt_task_param(&param);
	param.exec_cost = wcet;
	param.period = period;
	param.priority = priority;
	param.cls = RT_CLASS_HARD;
	param.release_policy = TASK_PERIODIC;
	param.budget_policy = NO_ENFORCEMENT;
	if (migrate) {
		param.cpu = gettid();
	}
	ret = set_rt_task_param(gettid(), &param);
	if (ret < 0)
		bail_out("could not setup rt task params");
	
	mc2_param.res_id = gettid();
	ret = set_mc2_task_param(gettid(), &mc2_param);
	if (ret < 0)
		bail_out("could not setup mc2 task params");
	
	arena_sz = WSS*1024;
	arena = alloc_arena(arena_sz, 0, 0);
	init_arena(arena, arena_sz);
	
	ret = init_litmus();
	if (ret != 0)
		bail_out("init_litmus() failed\n");

	start = wctime();
	ret = task_mode(LITMUS_RT_TASK);
	if (ret != 0)
		bail_out("could not become RT task");

	if (mc2_param.crit == CRIT_LEVEL_C)
		set_page_color(-1);
	else
		set_page_color(config.cpu);
	
	lock_memory();
	
	if (wait) {
		ret = wait_for_ts_release();
		if (ret != 0)
			bail_out("wait_for_ts_release()");
		start = wctime();
	}

	while (job(wcet_ms * 0.001, start + duration)) {};

	ret = task_mode(BACKGROUND_TASK);
	if (ret != 0)
		bail_out("could not become regular task (huh?)");

	reservation_destroy(gettid(), config.cpu);
	dealloc_arena(arena, arena_sz);
	return 0;
}
