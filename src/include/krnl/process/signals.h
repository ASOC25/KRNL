#ifndef _SIGNALS_H
#define _SIGNALS_H
void sleep(thread_t * process, int condition);
void wakeup(int condition);
int nanosleep(thread_t * thread, struct timespec *duration, struct timespec *rem);

#endif