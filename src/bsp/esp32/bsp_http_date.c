#include "bsp_http_date.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool bsp_http_date_parse(const char *date, time_t *epoch)
{
    if (!date || !epoch) return false;
    char weekday[4], month[4], zone[4];
    int day, year, hour, minute, second, end = 0;
    if (sscanf(date, "%3[A-Za-z], %2d %3[A-Za-z] %4d %2d:%2d:%2d %3[A-Za-z]%n",
               weekday, &day, month, &year, &hour, &minute, &second, zone, &end) != 8)
        return false;
    while (isspace((unsigned char)date[end])) end++;
    if (date[end] || strcmp(zone, "GMT") != 0 || year < 1970 || year > 9999 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
        return false;
    static const char *weekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    unsigned wd = 0;
    while (wd < 7 && strcmp(weekday, weekdays[wd]) != 0) wd++;
    if (wd == 7) return false;
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const int before_month[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    static const int month_days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int mon = 0;
    while (mon < 12 && strcmp(month, months[mon]) != 0) mon++;
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (mon == 12 || day < 1 || day > month_days[mon] + (mon == 1 && leap)) return false;

    /* Gregorian days since 1970-01-01, independent of libc's current timezone. */
    int y = year - 1;
    int64_t days = 365LL * (year - 1970) + (y / 4 - y / 100 + y / 400) -
                   (1969 / 4 - 1969 / 100 + 1969 / 400) + before_month[mon] +
                   (mon > 1 && leap) + day - 1;
    int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    time_t result = (time_t)seconds;
    if (result < 0 || (int64_t)result != seconds) return false;
    *epoch = result;
    return true;
}
