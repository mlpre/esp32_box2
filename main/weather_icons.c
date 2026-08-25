#include "weather_icons.h"

#include <stddef.h>
#include <stdint.h>

#define WEATHER_ICON_WIDTH 96U
#define WEATHER_ICON_HEIGHT 72U
#define WEATHER_ICON_BYTES (WEATHER_ICON_WIDTH * WEATHER_ICON_HEIGHT * 2U)
#define WEATHER_ICON_COUNT 9U

typedef enum
{
    ASSET_CLEAR_DAY = 0,
    ASSET_CLEAR_NIGHT,
    ASSET_PARTLY_CLOUDY_DAY,
    ASSET_PARTLY_CLOUDY_NIGHT,
    ASSET_CLOUDY,
    ASSET_FOG,
    ASSET_RAIN,
    ASSET_SNOW,
    ASSET_THUNDER,
} weather_asset_index_t;

extern const uint8_t weather_icons_bin_start[]
    asm("_binary_weather_icons_bin_start");
extern const uint8_t weather_icons_bin_end[]
    asm("_binary_weather_icons_bin_end");

static lv_image_dsc_t s_images[WEATHER_ICON_COUNT];
static bool s_initialized;

static void initialize_images(void)
{
    if (s_initialized)
    {
        return;
    }
    size_t available = (size_t)(weather_icons_bin_end - weather_icons_bin_start);
    if (available != WEATHER_ICON_COUNT * WEATHER_ICON_BYTES)
    {
        return;
    }
    for (size_t i = 0; i < WEATHER_ICON_COUNT; ++i)
    {
        s_images[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_images[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_images[i].header.flags = 0;
        s_images[i].header.w = WEATHER_ICON_WIDTH;
        s_images[i].header.h = WEATHER_ICON_HEIGHT;
        s_images[i].header.stride = WEATHER_ICON_WIDTH * 2U;
        s_images[i].data_size = WEATHER_ICON_BYTES;
        s_images[i].data = weather_icons_bin_start + i * WEATHER_ICON_BYTES;
    }
    s_initialized = true;
}

const lv_image_dsc_t *weather_icon_asset(weather_icon_t icon, bool is_day)
{
    initialize_images();
    if (!s_initialized)
    {
        return NULL;
    }

    weather_asset_index_t index;
    switch (icon)
    {
    case WEATHER_ICON_CLEAR:
        index = is_day ? ASSET_CLEAR_DAY : ASSET_CLEAR_NIGHT;
        break;
    case WEATHER_ICON_PARTLY_CLOUDY:
        index = is_day ? ASSET_PARTLY_CLOUDY_DAY : ASSET_PARTLY_CLOUDY_NIGHT;
        break;
    case WEATHER_ICON_CLOUDY:
        index = ASSET_CLOUDY;
        break;
    case WEATHER_ICON_FOG:
        index = ASSET_FOG;
        break;
    case WEATHER_ICON_RAIN:
        index = ASSET_RAIN;
        break;
    case WEATHER_ICON_SNOW:
        index = ASSET_SNOW;
        break;
    case WEATHER_ICON_THUNDER:
        index = ASSET_THUNDER;
        break;
    default:
        return NULL;
    }
    return &s_images[index];
}
