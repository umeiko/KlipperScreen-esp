/* Bounded config parser + real controller, fake RPC only. Never moves a printer.
 * gcc tests/test_bed_level.c src/core/bed_level_config.c -Isrc/core -Isrc/bsp
 *     -Isrc/ports/desktop -Ithird_party/lvgl -I<cjson headers> -lcjson -lm */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_settings.h"
#include "printer.h"
#include "bed_level_config.h"
#include "lvgl.h"
static uint32_t clock_ms = 2000;
uint32_t lv_tick_get(void) { return clock_ms; }
uint32_t lv_tick_elaps(uint32_t before) { return clock_ms-before; }
static printer_state_t state = PRINTER_STATE_STANDBY;
static machine_mode_t mode = MACHINE_MODE_KLIPPER;
printer_state_t printer_state(void) { return state; }
printer_capabilities_t printer_capabilities(void) { return PRINTER_CAP_KLIPPER_ALL; }
bool printer_zcal_active(void) { return false; }
machine_mode_t settings_load_machine_mode(void) { return mode; }
bool settings_load_moonraker(moonraker_conf_t *c)
{
    memset(c, 0, sizeof(*c)); strcpy(c->host, "offline"); c->port=7125; c->valid=true; return true;
}
static struct { char method[48], params[1024]; void (*cb)(char *, void *); void *ud; } rpc[256];
static unsigned requests;
static bool transport_ok = true;
static bool fake_rpc(const char *method, const char *params, void (*cb)(char *, void *), void *ud)
{
    if (!transport_ok) return false;
    assert(requests < 256);
    snprintf(rpc[requests].method, 48, "%s", method); snprintf(rpc[requests].params, 1024, "%s", params ? params : "");
    rpc[requests].cb=cb; rpc[requests++].ud=ud; return true;
}
#define moonraker_rpc fake_rpc
#include "../src/core/bed_level.c"
#undef moonraker_rpc
static const char *config = "{\"status\":{\"configfile\":{\"config\":{\"bed_screws\":{"
    "\"screw1\":\"20,20\",\"screw2\":\"200,20\",\"screw3\":\"200,200\",\"screw4\":\"20,200\"}}}}}";
static const char *position = "{\"status\":{\"toolhead\":{\"axis_minimum\":[0,0,-5],\"axis_maximum\":[220,220,250],"
    "\"position\":[50,60,10],\"homed_axes\":\"xyz\"},\"gcode_move\":{\"gcode_position\":[48,57,9.5]},"
    "\"idle_timeout\":{\"state\":\"Ready\"},\"bed_screws\":{\"is_active\":false}}}";
static void reply(unsigned i, const char *text) { rpc[i].cb(text ? strdup(text) : NULL, rpc[i].ud); }
static void parser_tests(void)
{
    bed_level_config_stream_t s; bed_level_data_t d;
    bed_level_config_begin(&s);
    for (const char *p=config; *p; p++) assert(bed_level_config_feed(&s, p, 1));
    assert(bed_level_config_finish(&s, &d) && d.state==BED_LEVEL_READY && d.count==4 && !d.probe);
    const char *three = "{\"result\":{\"status\":{\"configfile\":{\"config\":{\"screws_tilt_adjust\":{"
        "\"screw1\":\"20,20\",\"screw2\":\"200,20\",\"screw3\":\"110,200\"},"
        "\"bltouch\":{\"x_offset\":\"-30\",\"y_offset\":\"10\"}}}}}}";
    bed_level_config_begin(&s); assert(bed_level_config_feed(&s, three, strlen(three)));
    assert(bed_level_config_finish(&s, &d) && d.probe && d.count==3 && d.points[0].x==-10 && d.points[0].y==30);
    const char *missing = "{\"status\":{\"configfile\":{\"config\":{\"gcode_macro FAKE\":{\"bed_screws\":{}}}}}}";
    bed_level_config_begin(&s); assert(bed_level_config_feed(&s, missing, strlen(missing)));
    assert(bed_level_config_finish(&s,&d) && d.state==BED_LEVEL_MISSING);
    bed_level_config_begin(&s); assert(bed_level_config_feed(&s, config, strlen(config)-1));
    assert(!bed_level_config_finish(&s,&d));
    /* Huge unrelated macro string stays out of the fixed 6KiB retained buffer. */
    bed_level_config_begin(&s);
    const char *prefix="{\"status\":{\"configfile\":{\"config\":{\"gcode_macro BIG\":{\"gcode\":\"";
    assert(bed_level_config_feed(&s,prefix,strlen(prefix)));
    char chunk[512]; memset(chunk,'a',sizeof(chunk));
    for (int i=0;i<512;i++) assert(bed_level_config_feed(&s,chunk,sizeof(chunk)));
    assert(s.used==1);
    const char *tail="\"},\"bed_screws\":{\"screw1\":\"1,1\",\"screw2\":\"2,2\",\"screw3\":\"3,3\"}}}}}";
    assert(bed_level_config_feed(&s,tail,strlen(tail)) && bed_level_config_finish(&s,&d) && d.state==BED_LEVEL_INVALID);
}
static void load(void)
{
    unsigned n=requests; bed_level_reset(); bed_level_poll(); reply(n,config);
    clock_ms+=1001; bed_level_poll(); reply(requests-1,position);
    assert(bed_level_poll()->can_move);
}
int main(void)
{
    parser_tests(); load();
    assert(bed_level_move(0)); unsigned status_request=requests-1; reply(status_request,position);
    assert(!strcmp(rpc[requests-1].method,"printer.gcode.script"));
    assert(strstr(rpc[requests-1].params,"G1 Z9.500 F300") && strstr(rpc[requests-1].params,"G1 X18.000 Y17.000 F3000"));
    assert(strstr(rpc[requests-1].params,"RESTORE_GCODE_STATE") && !bed_level_move(1));
    reply(requests-1,"\"ok\""); reply(requests-1,position); assert(!bed_level_poll()->busy);
    /* Printing and paused states must never emit motion or motor-off commands. */
    state=PRINTER_STATE_PRINTING; assert(!bed_level_move(0) && !bed_level_motors_off());
    state=PRINTER_STATE_PAUSED; assert(!bed_level_move(0)); state=PRINTER_STATE_STANDBY;
    assert(bed_level_move(1)); unsigned before=requests; state=PRINTER_STATE_PRINTING;
    reply(requests-1,position); assert(requests==before); state=PRINTER_STATE_STANDBY;
    load(); data.points[0].x=-1; assert(bed_level_move(0)); before=requests; reply(requests-1,position);
    assert(requests==before && data.operation_failed); load();
    /* Homing completes before coordinates/offsets are queried again and moved. */
    const char *unhomed="{\"status\":{\"toolhead\":{\"axis_minimum\":[0,0,-5],\"axis_maximum\":[220,220,250],"
        "\"position\":[0,0,0],\"homed_axes\":\"\"},\"gcode_move\":{\"gcode_position\":[0,0,0]}}}";
    assert(bed_level_move(0)); reply(requests-1,unhomed);
    assert(strstr(rpc[requests-1].params,"G28\\nM400"));
    reply(requests-1,"\"ok\""); assert(!strcmp(rpc[requests-1].method,"printer.objects.query"));
    reply(requests-1,position); assert(strstr(rpc[requests-1].params,"G1 X18.000"));
    unsigned stale=requests-1; bed_level_reset(); reply(stale,"\"ok\""); assert(!data.busy);
    load(); data.probe=true;
    assert(bed_level_calculate()); reply(requests-1,position);
    assert(strstr(rpc[requests-1].params,"SCREWS_TILT_CALCULATE\\nM400"));
    reply(requests-1,"\"ok\"");
    cJSON *r=cJSON_Parse(position), *st=cJSON_GetObjectItem(r,"status");
    cJSON *tilt=cJSON_AddObjectToObject(st,"screws_tilt_adjust");
    cJSON *res=cJSON_AddObjectToObject(tilt,"results");
    cJSON_AddBoolToObject(cJSON_AddObjectToObject(res,"screw1"),"is_base",true);
    cJSON *second=cJSON_AddObjectToObject(res,"screw2");
    cJSON_AddStringToObject(second,"sign","CW"); cJSON_AddStringToObject(second,"adjust","00:12");
    char *result=cJSON_PrintUnformatted(r); reply(requests-1,result); cJSON_free(result); cJSON_Delete(r);
    assert(!strcmp(data.points[0].result,"base") && !strcmp(data.points[1].result,"CW 00:12"));
    load(); assert(bed_level_motors_off()); reply(requests-1,position);
    assert(strstr(rpc[requests-1].params,"M84")); reply(requests-1,NULL); reply(requests-1,position);
    assert(data.operation_failed);
    assert(bed_level_move(0)); reply(requests-1,position); reply(requests-1,NULL);
    assert(strstr(rpc[requests-1].params,"RESTORE_GCODE_STATE NAME=KS_BED_LEVEL MOVE=0"));
    reply(requests-1,"\"ok\""); reply(requests-1,position);
    mode=MACHINE_MODE_BAMBU; assert(!bed_level_move(0)); mode=MACHINE_MODE_KLIPPER;
    transport_ok=false; assert(!bed_level_move(0)); transport_ok=true;
    /* Passive polling failure is not a failed user operation and retries with backoff. */
    load(); clock_ms += STATUS_POLL_MS+1; transport_ok=false;
    bed_level_poll(); assert(data.status_failed && !data.operation_failed && !data.can_move && !data.busy);
    unsigned unchanged=requests; clock_ms+=STATUS_RETRY_MS-1; bed_level_poll(); assert(requests==unchanged);
    transport_ok=true; clock_ms+=2; bed_level_poll(); reply(requests-1,position);
    assert(!data.status_failed && !data.operation_failed && bed_level_poll()->can_move);
    /* An RPC error / malformed background response has the same non-action semantics. */
    clock_ms+=STATUS_POLL_MS+1; bed_level_poll(); reply(requests-1,NULL);
    assert(data.status_failed && !data.operation_failed);
    clock_ms+=STATUS_RETRY_MS+1; bed_level_poll(); reply(requests-1,"{}");
    assert(data.status_failed && !data.operation_failed);
    /* Status timeout preserves config; late replies cannot overwrite a newer request. */
    clock_ms+=STATUS_RETRY_MS+1; bed_level_poll(); unsigned timed_out=requests-1;
    clock_ms+=10001; bed_level_poll(); assert(data.state==BED_LEVEL_READY && data.status_failed && !data.operation_failed);
    reply(timed_out,position); assert(data.status_failed);
    clock_ms+=STATUS_RETRY_MS+1; bed_level_poll(); reply(requests-1,position);
    assert(bed_level_poll()->can_move && !data.operation_failed);
    puts("PASS: bounded streaming, 3/4 points, probe offsets, missing/invalid/truncated config,");
    puts("safe movement order, homing, task/range/stale-reply guards, tilt results, motors and transport failures");
    return 0;
}
