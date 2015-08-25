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

#define PAGE_SIZE 4096
#define NUM_ITEMS 8192

int *pages;
unsigned long *access_order;

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

#define NUMS 4096
static int num[NUMS];
static char* progname;

static int randrange(const int max)
{
	return (rand() / (RAND_MAX / max + 1));
}

static void sattolo(unsigned long *items, const unsigned long len)
{
	unsigned long i;
	/* first set up 0, 1, ..., n - 1 */
	for (i = 0; i < len; i++)
		items[i] = i;
	/* note: i is now n */
	while (1 < i--) {
		/* 0 <= j < i */
		int t, j = randrange(i);
		t = items[i];
		items[i] = items[j];
		items[j] = t;
	}
}

static int loop_once(void)
{
	int i, j = 0;
	for (i = 0; i < NUMS; i++)
		j += num[i]++;
	return j;
/*
	int i, tmp;
	for (i = 0; i < NUM_ITEMS; i++) {
		tmp = pages[access_order[i]];
		if (access_order[i] % 3 == 0)
			pages[access_order[i]] = i+tmp;
	}
	return 1;
*/
}

static int loop_for(double exec_time, double emergency_exit)
{
	double last_loop = 0, loop_start;
	int tmp = 0;

	double start = cputime();
	double now = cputime();

	while (now + last_loop < start + exec_time) {
		loop_start = now;
		tmp += loop_once();
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


static void debug_delay_loop(int count)
{
	double start, end, delay;

	while (count--) {
		for (delay = 0.5; delay > 0.01; delay -= 0.01) {
			start = wctime();
			loop_for(delay, 0);
			end = wctime();
			printf("%6.4fs: looped for %10.8fs, delta=%11.8fs, error=%7.4f%%\n",
			       delay,
			       end - start,
			       end - start - delay,
			       100 * (end - start - delay) / delay);
		}
	}
}

static int job(double exec_time, double program_end, int lock_od, double cs_length)
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
			loop_for(chunk1, program_end + 1);

			/* critical section */
			litmus_lock(lock_od);
			loop_for(cs_length, program_end + 1);
			litmus_unlock(lock_od);

			/* non-critical section */
			loop_for(chunk2, program_end + 2);
		} else {
			loop_for(exec_time, program_end + 1);
		}
		sleep_next_period();
		return 1;
	}
}

struct lt_interval* parse_td_intervals(int num, char* optarg, unsigned int *num_intervals)
{
	int i, matched;
	struct lt_interval *slots = malloc(sizeof(slots[0]) * num);
	char** arg = (char**)malloc(sizeof(char*) * num);
	char *token, *saveptr;
	double start, end;
	
	for (i = 0; i < num; i++) {
		arg[i] = (char*)malloc(sizeof(char)*100);
	}
	
	i = 0;
	token = strtok_r(optarg, ":", &saveptr);
	while(token != NULL) {
		sprintf(arg[i++], "%s", token);
		token = strtok_r(NULL, ":", &saveptr);
	}
	
	*num_intervals = 0;
	
	for (i=0; i<num; i++) {
		matched = sscanf(arg[i], "[%lf,%lf]", &start, &end);
		if (matched != 2) {
			fprintf(stderr, "could not parse '%s' as interval\n", arg[i]);
			exit(5);
		}
		if (start < 0) {
			fprintf(stderr, "interval %s: must not start before zero\n", arg[i]);
			exit(5);
		}
		if (end <= start) {
			fprintf(stderr, "interval %s: end before start\n", arg[i]);
			exit(5);
		}

		slots[i].start = ms2ns(start);
		slots[i].end   = ms2ns(end);

		if (i > 0 && slots[i - 1].end >= slots[i].start) {
			fprintf(stderr, "interval %s: overlaps with previous interval\n", arg[i]);
			exit(5);
		}

		(*num_intervals)++;
	}
	
	for (i=0; i<num; i++) {
		free(arg[i]);
	}
	free(arg);
	return slots;
}

#define OPTSTR "p:c:wlveo:f:s:q:X:L:Q:vh:m:i:b:"
int main(int argc, char** argv)
{
	int ret;
	lt_t wcet;
	lt_t period;
	lt_t hyperperiod;
	lt_t budget;
	double wcet_ms, period_ms, hyperperiod_ms, budget_ms;
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
	struct mc2_task mc2_param;
	struct reservation_config config;
	int res_type = PERIODIC_POLLING;
	int n_str, num_int = 0;

	int verbose = 0;
	unsigned int job_no;

	/* locking */
	int lock_od = -1;
	int resource_id = 0;
	const char *lock_namespace = "./rtspin-locks";
	int protocol = -1;
	double cs_length = 1; /* millisecond */

	progname = argv[0];

	/* default for reservation */
	config.id = 0;
	config.priority = LITMUS_NO_PRIORITY; /* use EDF by default */
	config.cpu = -1;
	
	mc2_param.crit = CRIT_LEVEL_C;
	
	hyperperiod_ms = 1000;
	budget_ms = 10;
	
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
			test_loop = 1;
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
		case 'm':
			mc2_param.crit = atoi(optarg);
			if (mc2_param.crit < CRIT_LEVEL_A || mc2_param.crit == NUM_CRIT_LEVELS) {
				usage("Invalid criticality level.");
			}
			res_type = PERIODIC_POLLING;
			break;
		case 'h':
			hyperperiod_ms = atof(optarg);
			break;
		case 'b':
			budget_ms = atof(optarg);
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

	if (test_loop) {
		debug_delay_loop(1);
		return 0;
	}

	if (mc2_param.crit > CRIT_LEVEL_A && config.priority != LITMUS_NO_PRIORITY)
		usage("Bad criticailty level or priority");

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
	budget = ms2ns(budget_ms);
	hyperperiod = ms2ns(hyperperiod_ms);
	
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

	/* reservation config */
	config.id = gettid();
	
	if (hyperperiod%period != 0 ) {
		;//bail_out("hyperperiod must be multiple of period");
	}
	
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
	param.cls = class;
	param.release_policy = TASK_PERIODIC;
	param.budget_policy = (want_enforcement) ?
			PRECISE_ENFORCEMENT : NO_ENFORCEMENT;
	if (migrate) {
		param.cpu = gettid();
	}
	ret = set_rt_task_param(gettid(), &param);
//printf("SET_RT_TASK\n");
	if (ret < 0)
		bail_out("could not setup rt task params");
	
	mc2_param.res_id = gettid();
	ret = set_mc2_task_param(gettid(), &mc2_param);
//printf("SET_MC2_TASK\n");
	if (ret < 0)
		bail_out("could not setup mc2 task params");

	pages = (int*)malloc(sizeof(int)*NUM_ITEMS);
	access_order = (unsigned long*)malloc(sizeof(unsigned long)*NUM_ITEMS);
	sattolo(access_order, NUM_ITEMS);

	init_litmus();
printf("CALL\n");
	set_page_color(config.cpu);
printf("CALL\n");

//printf("INIT_LITMUS\n");
	start = wctime();
	ret = task_mode(LITMUS_RT_TASK);
//printf("TASK_MODE\n");
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
//printf("BEFORE WAIT\n");
		ret = wait_for_ts_release();
		if (ret != 0)
			bail_out("wait_for_ts_release()");
		start = wctime();
	}

	if (file) {
		for (cur_job = 0; cur_job < num_jobs; ++cur_job) {
			job(exec_times[cur_job] * 0.001 * scale,
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
		} while (job(wcet_ms * 0.001 * scale, start + duration,
			   lock_od, cs_length * 0.001));
	}
printf("BEFORE BACK_TASK\n");
	ret = task_mode(BACKGROUND_TASK);
	if (ret != 0)
		bail_out("could not become regular task (huh?)");

	if (file)
		free(exec_times);

	reservation_destroy(gettid(), config.cpu);
	
printf("CALL\n");
	set_page_color(config.cpu);
printf("CALL\n");
	free(pages);
	free(access_order);
	return 0;
}
