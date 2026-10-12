#pragma once
/*
 * 天气时钟数据源：Open-Meteo（免费，无需 Key）。
 * 城市用经纬度（后台配置，默认渭南 34.50,109.50）。
 * 后台任务每 30 分钟拉取一次（实时温度 + WMO 天气码 + 今日高低温），
 * 缓存供屏保读取；断网/失败时保持 invalid，屏保回退纯时钟。
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单日预报（三天预报行） */
typedef struct {
    int   icon;      /* weather_icon_t 枚举 */
    char  text[12];  /* 天气文字（如"多云"） */
    int   tmax;      /* 白天最高 ℃ */
    int   tmin;      /* 夜间最低 ℃ */
} weather_day_t;

typedef struct {
    bool      valid;        /* 最近一次拉取成功 */
    int       temp_c;       /* 温度 ℃ */
    int       temp_min_c;   /* 今日最低 ℃（-100=未知） */
    int       temp_max_c;   /* 今日最高 ℃（-100=未知） */
    int       wind_level;   /* 风力等级（1-12），-1=未知 */
    int       icon;         /* weather_icon_t 枚举 */
    int       precip;       /* 降水概率 %，-1=未知（当前源无） */
    int       aqi;          /* AQI 数值，-1=未知（当前源无） */
    char      category[12]; /* 天气文字（实时） */
    char      city[32];     /* 城市显示名（后台配置） */
    uint64_t  updated_ms;   /* 数据时间戳（ms） */
    weather_day_t forecast[3]; /* 今天/明天/后天 */
    int       forecast_count;  /* 0=无 */
} weather_data_t;

/* 屏保用简化图标分类（WMO weather code 归并） */
typedef enum {
    WEATHER_SUN = 0,      /* 晴 */
    WEATHER_PARTLY,       /* 多云 */
    WEATHER_OVERCAST,     /* 阴 */
    WEATHER_RAIN,         /* 雨/雷雨 */
    WEATHER_SNOW,         /* 雪 */
    WEATHER_FOG,          /* 雾/霾 */
    WEATHER_UNKNOWN,
} weather_icon_t;

void weather_start(void);               /* 创建后台刷新任务（幂等） */
bool weather_get(weather_data_t *out);  /* 拷贝缓存，返回是否有效 */
weather_icon_t weather_icon_from_code(int code);

#ifdef __cplusplus
}
#endif
