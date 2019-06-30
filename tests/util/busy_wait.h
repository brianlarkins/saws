#ifndef __BUSY_WAIT_H__
#define __BUSY_WAIT_H__

/** Figure out how many iterations busy_wait needs to do to wait for
 *  the desired time.
 *
 *  @PARAM desired_time How long to wait in seconds.
 *  @RETURNS            Timeout value you need to pass to busy_wait to
 *                      have it wait for desired_time seconds.
 **/
int  tune_busy_wait(double desired_time);

void busy_wait(int busy_iter);

#endif /* __BUSY_WAIT_H__ */
