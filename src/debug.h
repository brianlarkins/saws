#ifndef __DEBUG_H__
#define __DEBUG_H__

#ifdef __INTEL_COMPILER
#pragma warning (disable:869)           /* parameter "param" was never referenced */
#pragma warning (disable:981)           /* operands are evaluated in unspecified order */
#pragma warning (disable:1418)          /* external function definition with no prior declaration */
#endif
 
#ifndef NO_SEATBELTS
#include <assert.h>
#else
#warning "NO_SEATBELTS Defined -- Building without safety check and performance analysis"
#define assert(X)
#endif

// Enable/Disable different classes of debugging statements by OR-ing these flags
// together to form DEBUGLEVEL
#define DBGINIT    1
#define DBGPROCESS 2
#define DBGGET     4
#define DBGTD      8
#define DBGTASK    16
#define DBGSHRB    32
#define DBGINBOX   64
#define DBGGROUP   128
#define DBGSYNCH   256

//#define DEBUGLEVEL DBGSHRB
//#define DEBUGLEVEL DBGTD
//#define DEBUGLEVEL DBGGET
//#define DEBUGLEVEL DBGPROCESS
//#define DEBUGLEVEL DBGGROUP
//#define DEBUGLEVEL DBGINIT|DBGTD
//#define DEBUGLEVEL DBGINIT|DBGGET|DBGTD
//#define DEBUGLEVEL DBGINIT|DBGPROCESS|DBGGET|DBGTD
//#define DEBUGLEVEL DBGINIT|DBGPROCESS|DBGGET|DBGTD|DBGGROUP|DBGSHRB

#ifndef DEBUGLEVEL
#define DEBUGLEVEL  0
#endif

// Execute CMD if the given debug FLAG is set
#define DEBUG(FLAG, CMD) if((FLAG) & (DEBUGLEVEL)) { CMD; }

#endif /* __DEBUG_H__ */
