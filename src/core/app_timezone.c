#include "app_timezone.h"
#include "app_settings.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const struct { const char *id, *label, *rule; } zones[] = {
    { "Asia/Shanghai", "UTC+08:00", "CST-8" }, /* preserve existing devices' default */
    { "Europe/Berlin", "CET/CEST", "CET-1CEST,M3.5.0/2,M10.5.0/3" },
    { "UTC", "UTC", "UTC0" },
    { "Europe/London", "GMT/BST", "GMT0BST,M3.5.0/1,M10.5.0/2" },
    { "America/New_York", "EST/EDT", "EST5EDT,M3.2.0/2,M11.1.0/2" },
    { "America/Chicago", "CST/CDT", "CST6CDT,M3.2.0/2,M11.1.0/2" },
    { "America/Denver", "MST/MDT", "MST7MDT,M3.2.0/2,M11.1.0/2" },
    { "America/Los_Angeles", "PST/PDT", "PST8PDT,M3.2.0/2,M11.1.0/2" },
    { "Asia/Kolkata", "UTC+05:30", "IST-5:30" },
    { "Asia/Tokyo", "UTC+09:00", "JST-9" },
    { "Australia/Sydney", "AEST/AEDT", "AEST-10AEDT,M10.1.0/2,M4.1.0/3" },
};

unsigned app_timezone_count(void) { return sizeof(zones) / sizeof(zones[0]); }

const char *app_timezone_label(unsigned index)
{
    return index < app_timezone_count() ? zones[index].label : "";
}

unsigned app_timezone_current(void)
{
    char id[48];
    settings_load_timezone(id, sizeof(id));
    for (unsigned i = 0; i < app_timezone_count(); i++)
        if (strcmp(id, zones[i].id) == 0) return i;
    return 0;
}

bool app_timezone_apply(unsigned index)
{
    if (index >= app_timezone_count() || setenv("TZ", zones[index].rule, 1) != 0) return false;
    tzset();
    return true;
}

bool app_timezone_select(unsigned index)
{
    unsigned before = app_timezone_current();
    if (!app_timezone_apply(index)) return false;
    if (settings_save_timezone(zones[index].id)) return true;
    /* Failed flash writes must not leave the UI/clock on an unsaved selection. */
    app_timezone_apply(before);
    return false;
}
