//The purpose of this is to fill an array with the host names of each rank, with rank 0 corresponding to arr slot 0, rank 1 to index 1, and so on

#include <stdio.h>
#include <unistd.h>

#include <tc.h>

#define BUFFER_SIZE 1024
#define MAXHOSTS    2112
#define MAXHOSTNAME 64

char host_arr[MAXHOSTS][MAXHOSTNAME];
int owner_arr[MAXHOSTS];

int main (int argc, char *argv[]) {
  char hostbuffer[BUFFER_SIZE];

  gtc_init();

  

  gethostname(hostbuffer, sizeof(hostbuffer));
  shmem_putmem(host_arr[_c->rank],hostbuffer,MAXHOSTNAME, 0);
  
/*  for (int i=0; i <=_c->size; i+=48){
    if (_c->rank >= i && _c->rank < i+48) {
      //shmem_putmem(owner_arr[_c->rank], i, sizeof(i), 0)
      owner_arr[_c->rank] = i;
      break;
    }
  }
*/


  shmem_barrier_all();


  /*for (int k=0; k<_c->size; k++) {
    if (host_arr[k] == host_arr[_c->rank]) {
      owner_arr[_c->rank] = k;
      break;
    }
  }*/

  if (_c->rank == 0) {
    for (int i=0; i<_c->size; i++) {
      printf("hostname for rank %d is %s\n", i, host_arr[i]);
    }

   /* for (int j=0; j<_c->size;j++) {
      printf("owner process for rank %d is %d\n", j, owner_arr[j]);
    }*/
  }

  gtc_fini();


//shmem_finalize(); //do I need this?
return 0;
}
