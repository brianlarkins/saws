#ifndef __TC_H__
#define __TC_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef int gtc_t;

#include <tc-task.h>
// #include <tc-group.h>

#define AUTO_BODY_SIZE -1

enum victim_select_e { VICTIM_RANDOM, VICTIM_ROUND_ROBIN };
enum steal_method_e  { STEAL_HALF, STEAL_ALL, STEAL_CHUNK };

#endif /*__TC_H__*/
