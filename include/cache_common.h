#ifndef __CACHE_COMMON_H__
#define __CACHE_COMMON_H__

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>

#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/io.h>
#include <sys/utsname.h>

#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "litmus.h"
#include "asm/cycles.h"

#if defined(__i386__) || defined(__x86_64__)
#include "asm/irq.h"
#endif


#define UNCACHE_DEV "/dev/litmus/uncache"

static void die(char *error)
{
    fprintf(stderr, "Error: %s (errno: %m)\n",
        error);
    exit(1);
}

static int migrate_to(int cpu)
{
    int ret;

    static __thread cpu_set_t* cpu_set = NULL;
    static __thread size_t cpu_set_sz;
    static __thread int num_cpus;
    if(!cpu_set)
    {
        num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        cpu_set = CPU_ALLOC(num_cpus);
        cpu_set_sz = CPU_ALLOC_SIZE(num_cpus);
    }

    CPU_ZERO_S(cpu_set_sz, cpu_set);
    CPU_SET_S(cpu, cpu_set_sz, cpu_set);
    ret = sched_setaffinity(0 /* self */, cpu_set_sz, cpu_set);
    return ret;
}

static int check_migrations(int num_cpus)
{
    int cpu, err;

    for (cpu = 0; cpu < num_cpus; cpu++) {
        err = migrate_to(cpu);
        if (err != 0) {
            fprintf(stderr, "Migration to CPU %d failed: %m.\n",
                cpu + 1);
            return 1;
        }
    }
    return 0;
}

static int become_posix_realtime_task(int prio)
{
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = prio;
    return sched_setscheduler(0 /* self */, SCHED_FIFO, &param);
}

static int renice(int nice_val)
{
        return setpriority(PRIO_PROCESS, 0 /* self */, nice_val);
}

static int lock_memory(void)
{
    return mlockall(MCL_CURRENT | MCL_FUTURE);
}

/* define CACHELINE_SIZE if not provided by compiler args */
#ifndef CACHELINE_SIZE
#if defined(__i386__) || defined(__x86_64__)
/* recent intel cpus */
#define CACHELINE_SIZE 64
#elif defined(__arm__)
/* at least with Cortex-A9 cpus ("8 words") */
#define CACHELINE_SIZE 32
#else
#error "Could not determine cacheline size!"
#endif
#endif

#define INTS_IN_CACHELINE (CACHELINE_SIZE/sizeof(int))
typedef struct cacheline
{
        int line[INTS_IN_CACHELINE];
} __attribute__((aligned(CACHELINE_SIZE))) cacheline_t;

static cacheline_t* alloc_arena(size_t size, int use_huge_pages, int use_uncache_pages)
{
    int flags = MAP_PRIVATE | MAP_POPULATE;
    cacheline_t* arena = NULL;
    int fd;

    if(use_huge_pages)
        flags |= MAP_HUGETLB;

	if(use_uncache_pages) {
			fd = open(UNCACHE_DEV, O_RDWR);
			if (fd == -1)
					die("Failed to open uncache device. Are you running the LITMUS^RT kernel?");
	}
	else {
			fd = -1;
			flags |= MAP_ANONYMOUS;
	}

    arena = mmap(0, size, PROT_READ | PROT_WRITE, flags, fd, 0);
	
    if(use_uncache_pages)
		close(fd);

    assert(arena);

        return arena;
}

static void dealloc_arena(cacheline_t* arena, size_t size)
{
		int ret = munmap((void*)arena, size);
        if(ret != 0)
                die("munmap() error");
}

static int randrange(int min, int max)
{
        /* generate a random number on the range [min, max) w/o skew */
        int limit = max - min;
        int devisor = RAND_MAX/limit;
        int retval;

        do {
                retval = rand() / devisor;
        } while(retval == limit);
        retval += min;

        return retval;
}

static void init_arena(cacheline_t* arena, size_t size)
{
    int i;
        size_t num_arena_elem = size / sizeof(cacheline_t);

        /* Generate a cycle among the cache lines using Sattolo's algorithm.
           Every int in the cache line points to the same cache line.
           Note: Sequential walk doesn't care about these values. */
        for (i = 0; i < num_arena_elem; i++) {
                int j;
                for(j = 0; j < INTS_IN_CACHELINE; ++j)
                        arena[i].line[j] = i;
        }
        while(1 < i--) {
                int j = randrange(0, i);
                cacheline_t temp = arena[j];
                arena[j] = arena[i];
                arena[i] = temp;
        }
}

static void sleep_us(int microseconds)
{
    struct timespec delay;

    delay.tv_sec = 0;
    delay.tv_nsec = microseconds * 1000;
    if (nanosleep(&delay, NULL) != 0)
        die("sleep failed");
}

static int completed(int nSamples, int* history, int nCategories)
{
        int i;
        for(i = 0; i < nCategories; ++i)
                if(history[i] < nSamples)
                        return 0;
        return 1;
}

inline unsigned long get_cyclecount (void)
{
	unsigned long value;
	// Read CCNT Register
	asm volatile ("MRC p15, 0, %0, c9, c13, 0\t\n": "=r"(value));
	return value;
}


#endif