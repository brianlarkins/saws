/*
 * shmem_latency.c
 * compile: oshcc -o shmem_latency shmem_latency.c -lm
 * run:     srun -n 2 -N 1 ./shmem_latency [# iterations] [task size in bytes] [# tasks] [stress test?] [file path]
 *	    srun -n 2 -N 2 ./shmem_latency [# iterations] [task size in bytes] [# tasks] [stress test?] [file path]
 * options are optional 
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <shmem.h>
#include <shmemx.h>
#include <math.h>

#define MAXPROC 67108864	

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))

long long tsk_q[MAXPROC];			//512 MB array
int rank;					//global process id
FILE *fptr;					//file to write data to	

void steal(int from, long long start, long long size, double *min, double *max, double *data)
{
	clock_t t = clock();
	
	/* steal size items starting at index start */
	shmem_get(&tsk_q[start], &tsk_q[start], size, from);
	
	t = clock() - t;
	
	double time_taken = ((double)t/CLOCKS_PER_SEC)*1000000;
	
	/* store time data */
	data[start] = time_taken;

	*max = MAX(*max, time_taken);
	*min = MIN(*min, time_taken);
}

void communicate(void (*steal)(), void (*table)(), long long tsk_size, long long msg_iter, long long max_tsk, long long stress)
{
	if (rank == 0)
	{
		/* test the number of tasks per steal from 1 to max_tsk if stress else max_tsk */
		int num_tsk;
		
		if (stress)
			num_tsk = 1;
		else
			num_tsk = max_tsk;

		for (; num_tsk <= max_tsk; num_tsk ++)
		{
			double min;
			double max;
			double data[msg_iter];	

			/* the length of the steal is num_tsk * tsk_size / sizeof(long long) */
			for (long long i = 0; i < msg_iter; i ++)
				(*steal)(1, 
					 i, 
					 num_tsk * tsk_size / sizeof(long long),
					 &min,
					 &max,
					 data
					);

			/* print data analysis */			
			(*table)(tsk_size, num_tsk, msg_iter, min, max, data);
		}
	}
}

void printtable(long long tsk_size, long long num_tsk, long long msg_iter, double min, double max, double *data)
{	
	double sum = 0;
	
	for (int i = 0; i < msg_iter; i ++)
		sum += data[i];

	printf("============ RESULTS ================\n");
	printf("Message count:	    %lld		\n", num_tsk);
	printf("Message size:	    %lld	     \n", tsk_size);
	printf("Message repetition: %lld 	     \n", msg_iter);
	printf("Total duration:	    %.3f        %s\n", sum / 1000, "ms"); 
	
	double average = sum / msg_iter;

	printf("Average duration:   %.3f         %s\n", average, "us");
	
	printf("Minimum duration:   %.3f	  %s\n", min, "us");
	printf("Maximum duration:   %.3f	  %s\n", max, "us");
	
	double variance = 0;

	for (int i = 0; i < msg_iter; i ++)
		 variance += ((data[i] - average) * (data[i] - average));
	
	variance /= (msg_iter - 1);

	printf("Standard deviation: %.3f	  %s\n", sqrt(variance), "us");
	printf("Message rate:	    %.3f	  %s\n", msg_iter/(sum/1000000), "msg/s");
	printf("=====================================\n");

	fprintf(fptr, "%lld\t%.3f\t%.3f\t\t\n", num_tsk, sum / 1000, average); 	
}

void createqueue()
{
	for (long long i = 0; i < 67108864; i ++)
		tsk_q[i] = i;
}

int main(int argc, char *argv[])
{
	shmem_init();
	
	char *file_path = "data.txt";

	long long msg_iter = 1000000;
	long long tsk_size = 8;
	long long max_tsk = 3000;
	long long stress = 0;

	rank = shmem_my_pe();

	if (argc > 1)
	{
		msg_iter = atoi(argv[1]);
	
		if (argc > 2)
		{
			tsk_size = atoi(argv[2]);
			
			if (argc > 3)
			{
				max_tsk = atoi(argv[3]);
				
				if (argc > 4)
				{	
					stress = atoi(argv[4]);
							
					if (argc > 5)
						file_path = argv[5];
				}
			}	
		}
	}
	
	fptr = fopen(file_path, "w");

	if (fptr == NULL)
	{
		perror("File error: ");
		exit(1);
	}

	fprintf(fptr, "tsk_size: %lld\nrep: %lld\n", tsk_size, msg_iter);
	fprintf(fptr, "message count -> total duration -> average duration\n");

	createqueue();
	
	shmem_barrier_all();	

	communicate(&steal, &printtable, tsk_size, msg_iter, max_tsk, stress);

	shmem_barrier_all();	

	fclose(fptr);

	shmem_finalize();	
}
