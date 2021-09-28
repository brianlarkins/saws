/* testing pthreads time with task sharing */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>

#include "tc.h"


#define t_size 24
#define reps 10000
#define TASKBUF 6000000

struct mytask_s {
  char data[t_size]; 	//sizeof(mytask_t): 24 or 192
};
typedef struct mytask_s *mytask_t;
mytask_t *thetasks;	//array in mem representing tasks to steal
double gtime = 0.0;

void* steal() {
  int foo[9] = {1,10,50,100,250,500,1000,2000,3000};
  int i = 0;
  int nt;
  volatile tc_timer_t time;
  mytask_t buf[4096];	//create a buffer to copy into

  mytask_t bullshit[4096];
  TC_INIT_ATIMER(time);

  while (i<9) { // for # tasks (nt)
    nt = foo[i];
    int cur = 0;

    //start Atimer
    TC_INIT_ATIMER(time);
    TC_START_ATIMER(time);

    for (int r=0; r< reps; r++) {// for # repetitions
      // memcpy - cur chunk mem into buf, chunk = nt*sizeof(mytask_t)
      memcpy(&buf, &thetasks[cur], nt*sizeof(mytask_t));

      if (cur + nt > TASKBUF) {
        cur = 0;  //too big
      }
      else {
        cur = cur + nt;
      }
    }
    //stop timer
    TC_STOP_ATIMER(time);
    memcpy(&bullshit, &buf, sizeof(buf));
    gtime = TC_READ_ATIMER(time);
    printf("copied %d tasks %7.4f usec\n", nt, (gtime/(1000.0*reps)));
    time.total = 0;
    i++;
  } //end # tasks loop
  gtime = time.total + time.temp;
  return NULL;
}


int main(int argc, char* argv[]) {
  int rv;  //return value of pthread_create
  pthread_t thread_id;  //inside loop ??

  //malloc array in mem for 6M tasks - too big for L3 cache when tsize=24
  thetasks = malloc(TASKBUF*sizeof(mytask_t));

  // initialize array of tasks
  memset(thetasks, 2, TASKBUF*sizeof(mytask_t)); //initialize each data chunk in thetasks

  // create thread 
  rv = pthread_create(&thread_id, NULL, &steal, NULL);
  if (rv) {
    printf("\n Error: return code from pthread_create is %d \n", rv);
    exit(1); 
  }
  pthread_join(thread_id, NULL);

  //for (int i=0; i<TASKBUF; i++) {

  free(thetasks);
  return 0;
}
