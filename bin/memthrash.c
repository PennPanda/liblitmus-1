#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <sched.h>

#include <time.h>

#include "cache_common.h"

#define NUM_VARS   (8388608*2)

int64_t data[NUM_VARS];

#define OPTSTR "m:"
int main(int argc, char** argv)
{
        int i;
        int64_t sum;

        int cpu = -1;
        int opt;

        while ((opt = getopt(argc, argv, OPTSTR)) != -1)
        {
                switch(opt)
                {
                case 'm':
                        cpu = atoi(optarg);
                        break;
                case ':':
                case '?':
                default:
                        printf("Bad or missing argument.\n");
                        exit(-1);
                }
        }

        srand(time(NULL));

        if(cpu != -1)
                migrate_to(cpu);

        lock_memory();
        renice(-20); /* meanest task around */

        while (1) {
                for (i = 0; i < NUM_VARS; i++)
                        data[i] = rand();
                sum = 0;
                for (i = 0; i < NUM_VARS; i++)
                        sum += (i % 2 ? 1 : -1) * data[i];
                for (i = NUM_VARS - 1; i >= 0; i--)
                        sum += (i % 2 ? -1 : 1) * 100  /  (data[i] ? data[i] : 1);
        }

        return 0;
}