#include <krnl/libraries/std/time.h>
#include <krnl/arch/x86/hpet.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>
#include <krnl/boot/bootloaders/bootloader.h>
#include <krnl/libraries/std/stdio.h>

char tzname[2][3] = {"CET", "CET"};
int daylight = 0;
long timezone = 0;
uint64_t boot_time = 0;

void tzset(void) {
    //Set timezone to Spanish time
    memcpy(tzname[0], "CET", 3);
    memcpy(tzname[1], "CET", 3);
}

time_t get_epoch() {
    if (boot_time == 0) {
        boot_time = (time_t)get_boot_time();
    }
    //use hpet_get_current_time() which returns microseconds since boot
    return (time_t)boot_time + (hpet_get_current_time() / 1000000);
}

time_t time(time_t *tloc) {
    if (tloc) {
        *tloc = (time_t)get_epoch();
    }
    return (time_t)get_epoch();
}

struct timeval * timeval_now(struct timeval *tv) {
    if (tv) {
        time_t epoch = get_epoch();
        uint64_t microseconds_since_boot = hpet_get_current_time();
        tv->tv_sec = epoch + (microseconds_since_boot / 1000000);
        tv->tv_usec = microseconds_since_boot % 1000000;
    }
    return tv;
}

struct timespec * timespec_now(struct timespec *ts) {
    if (ts) {
        struct timeval tv;
        timeval_now(&tv);
        TIMEVAL_TO_TIMESPEC(&tv, ts);
    }
    return ts;
}

struct timespec * clock_res(struct timespec *ts) {
    uint64_t microseconds_resolution = hpet_get_resolution();
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = microseconds_resolution * 1000;
    }
    return ts;
}

struct tm *localtime(const time_t *timep) {
    static struct tm tm;
    tm.tm_sec = *timep % 60;
    tm.tm_min = (*timep / 60) % 60;
    tm.tm_hour = (*timep / 3600) % 24;
    tm.tm_mday = (*timep / 86400) % 30;
    tm.tm_mon = (*timep / 2592000) % 12;
    tm.tm_year = (*timep / 31104000) + 70;
    tm.tm_wday = (*timep / 86400) % 7;
    tm.tm_yday = (*timep / 86400) % 365;
    tm.tm_isdst = 0;
    return &tm;
}

const char days[7][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char months[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

uint64_t strftime(char *s, uint64_t max, const char *format, const struct tm *tm) {
    uint64_t i = 0;
    while (i < max && *format) {
        if (*format == '%') {
            format++;
            switch (*format) {
                case 'a':
                    memcpy(s+i, days[tm->tm_wday], 3);
                    i += 3;
                    break;
                case 'b':
                    memcpy(s+i, months[tm->tm_mon], 3);
                    i += 3;
                    break;
                case 'd':
                    snprintf(s+i, 3, "%02d", tm->tm_mday);
                    i += 2;
                    break;
                case 'H':
                    snprintf(s+i, 3, "%02d", tm->tm_hour);
                    i += 2;
                    break;
                case 'M':
                    snprintf(s+i, 3, "%02d", tm->tm_min);
                    i += 2;
                    break;
                case 'S':
                    snprintf(s+i, 3, "%02d", tm->tm_sec);
                    i += 2;
                    break;
                case 'Y':
                    snprintf(s+i, 5, "%04d", tm->tm_year + 1900);
                    i += 4;
                    break;
                case 'Z':
                    memcpy(s+i, tzname[0], 3);
                    i += 3;
                    break;
                case '%':
                    s[i] = '%';
                    i++;
                    break;
                default:
                    s[i] = *format;
                    i++;
                    break;
            }
        } else {
            s[i] = *format;
            i++;
        }
        format++;
    }
    s[i] = 0;
    return i;
}

struct timespec epoch_to_timespec(uint32_t epoch) {
    struct timespec ts;
    ts.tv_sec = epoch;
    ts.tv_nsec = 0; // No nanoseconds in epoch
    return ts;
}