//The purpose of this is to fill an array with the host names of each rank, with rank 0 corresponding to arr slot 0, rank 1 to index 1, and so on

#include <stdio.h>
#include <unistd.h>

#include <tc.h>

#define BUFFER_SIZE 1024
#define MAXHOSTS    2112
#define MAXHOSTNAME 64

char host_arr[MAXHOSTS][MAXHOSTNAME];

int main (int argc, char *argv[]) {
  char hostbuffer[BUFFER_SIZE];

  gtc_init();

  //char dest[nprocs];

  gethostname(hostbuffer, sizeof(hostbuffer));
  shmem_putmem(host_arr[_c->rank],hostbuffer,MAXHOSTNAME, 0); 
  shmem_barrier_all();

  if (_c->rank == 0) {
    for (int i=0; i<_c->size; i++) {
      printf("hostname for rank %d is %s\n", i, host_arr[i]);
    }
  }

  gtc_fini();


//shmem_finalize(); //do I need this?
return 0;
}
