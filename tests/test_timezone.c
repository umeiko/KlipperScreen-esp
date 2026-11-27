#include "app_settings.h"
#include "app_timezone.h"
#include "bsp_http_date.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static char config[4096];
static bool fail_write, fail_sync;
static time_t synced;

int bsp_conf_read(const char *name, char *buf, size_t len)
{
    if (strcmp(name, "klipperscreen.conf") != 0 || !config[0]) return -1;
    int n = snprintf(buf, len, "%s", config);
    return n < (int)len ? n : -1;
}

int bsp_conf_write(const char *name, const char *buf)
{
    assert(strcmp(name, "klipperscreen.conf") == 0);
    if (fail_write) return -1;
    assert(strlen(buf) < sizeof(config));
    strcpy(config, buf);
    return 0;
}

bool bsp_conf_default_printer(char *host, size_t hlen, char *name, size_t nlen)
{
    (void)host; (void)hlen; (void)name; (void)nlen;
    return false;
}

static int fake_settimeofday(const struct timeval *tv, void *zone)
{
    assert(zone == NULL);
    if (fail_sync) return -1;
    synced = tv->tv_sec;
    return 0;
}

/* Compile the actual ESP32 synchronization entry point with only the OS write mocked. */
#define settimeofday fake_settimeofday
#define ESP_LOGI(...) ((void)0)
#include "time_sync_under_test.inc"
#undef settimeofday

static void check_local(const char *date, int hour, int minute, int second, int dst)
{
    assert(bsp_time_sync_from_http_date(date));
    struct tm local;
    assert(localtime_r(&synced, &local));
    assert(local.tm_hour == hour && local.tm_min == minute && local.tm_sec == second);
    assert(local.tm_isdst == dst);
}

int main(void)
{
    const char *reported = "Fri, 27 Nov 2026 12:21:00 GMT";
    assert(app_timezone_current() == 0 && app_timezone_apply(0));
    check_local(reported, 20, 21, 0, 0); /* Reproduce the reported UTC+8 offset. */
    time_t epoch = synced;
    strcpy(config, "language=en\nbrightness=75\ntheme=dark\n");
    assert(app_timezone_select(1));
    assert(strstr(config, "timezone=Europe/Berlin\n"));
    assert(strstr(config, "brightness=75\n") && strstr(config, "language=en\n"));
    check_local(reported, 13, 21, 0, 0);
    assert(synced == epoch); /* Changing TZ changes presentation, never UTC. */
    char tz[64];
    snprintf(tz, sizeof(tz), "%s", getenv("TZ"));
    check_local("Wed, 01 Jul 2026 12:21:00 GMT", 14, 21, 0, 1);
    assert(strcmp(getenv("TZ"), tz) == 0); /* Reconnect/resync cannot reset to CST. */
    assert(setenv("TZ", "UTC0", 1) == 0); tzset();
    assert(app_timezone_apply(app_timezone_current())); /* Boot-time restore. */
    check_local(reported, 13, 21, 0, 0);
    puts("PASS: reported 7-hour offset, instant selection, persistence and repeated UTC synchronization");

    check_local("Sun, 29 Mar 2026 00:59:59 GMT", 1, 59, 59, 0);
    check_local("Sun, 29 Mar 2026 01:00:00 GMT", 3, 0, 0, 1);
    check_local("Sun, 25 Oct 2026 00:59:59 GMT", 2, 59, 59, 1);
    check_local("Sun, 25 Oct 2026 01:00:00 GMT", 2, 0, 0, 0);
    puts("PASS: Europe/Berlin spring-forward and fall-back boundaries");

    const int winter_minutes[] = {480, 60, 0, 0, -300, -360, -420, -480, 330, 540, 660};
    const int summer_minutes[] = {480, 120, 0, 60, -240, -300, -360, -420, 330, 540, 600};
    assert(app_timezone_count() == sizeof(winter_minutes) / sizeof(winter_minutes[0]));
    for (unsigned i = 0; i < app_timezone_count(); i++) {
        assert(app_timezone_apply(i));
        const char *dates[] = { reported, "Wed, 01 Jul 2026 12:21:00 GMT" };
        for (unsigned season = 0; season < 2; season++) {
            assert(bsp_time_sync_from_http_date(dates[season]));
            struct tm local;
            assert(localtime_r(&synced, &local));
            int offset = season ? summer_minutes[i] : winter_minutes[i];
            int expected = (12 * 60 + 21 + offset + 1440) % 1440;
            assert(local.tm_hour * 60 + local.tm_min == expected);
        }
    }
    assert(app_timezone_apply(app_timezone_current()));
    fail_write = true;
    assert(!app_timezone_select(2) && app_timezone_current() == 1);
    assert(strcmp(getenv("TZ"), tz) == 0);
    fail_write = false;
    assert(!app_timezone_select(app_timezone_count()));
    assert(!settings_save_timezone("Europe/Berlin\nbrightness=0"));
    assert(!settings_save_timezone(NULL) && !settings_save_timezone(""));
    strcpy(config, "timezone=Unknown/Zone\n");
    assert(app_timezone_current() == 0);
    puts("PASS: all preset winter/summer offsets, failed-save rollback and invalid preferences");

    const char *bad[] = { "", "garbage", "Bad, 27 Nov 2026 12:21:00 GMT",
        "Fri, 27 Xxx 2026 12:21:00 GMT", "Fri, 31 Nov 2026 12:21:00 GMT",
        "Sun, 29 Feb 2026 12:21:00 GMT", "Fri, 27 Nov 2026 24:21:00 GMT",
        "Fri, 27 Nov 2026 12:60:00 GMT", "Fri, 27 Nov 2026 12:21:00 CET",
        "Fri, 27 Nov 2026 12:21:00 GMT junk" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(!bsp_time_sync_from_http_date(bad[i]));
    assert(!bsp_time_sync_from_http_date(NULL));
    assert(bsp_http_date_parse("Thu, 01 Jan 1970 00:00:00 GMT", &epoch) && epoch == 0);
    assert(bsp_http_date_parse("Tue, 29 Feb 2028 00:00:00 GMT", &epoch));
    assert(!bsp_http_date_parse(reported, NULL));
    fail_sync = true;
    assert(!bsp_time_sync_from_http_date(reported));
    puts("PASS: UTC date validation, leap year, epoch and OS synchronization failure");
    return 0;
}
