#include "radio_font.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Generated from GNU Unifont 17.0.04 by tools/generate_radio_font.py and
 * linked into the application. The font subset is licensed under SIL OFL 1.1;
 * see LICENSES/UNIFONT-OFL-1.1.txt.
 */
extern const uint8_t radio_font_bin_start[] asm("_binary_radio_font_bin_start");
extern const uint8_t radio_font_bin_end[] asm("_binary_radio_font_bin_end");

typedef struct
{
    uint32_t first;
    uint32_t last;
    uint32_t first_index;
    uint8_t advance;
} radio_font_range_t;

/* Keep this table in the same order as RANGES in generate_radio_font.py. */
static const radio_font_range_t s_ranges[] = {
    {0x0020, 0x007e, 0, 8},
    {0x00a0, 0x00ff, 95, 8},
    {0x2000, 0x206f, 191, 16},
    {0x3000, 0x303f, 303, 16},
    {0x3400, 0x4dbf, 367, 16},
    {0x4e00, 0x9fff, 6959, 16},
    {0xff00, 0xffef, 27951, 16},
};

#define RADIO_FONT_GLYPH_BYTES 32U
#define RADIO_FONT_EXPECTED_BYTES (28191U * RADIO_FONT_GLYPH_BYTES)

static bool find_glyph(uint32_t letter, uint32_t *index, uint8_t *advance)
{
    for (size_t i = 0; i < sizeof(s_ranges) / sizeof(s_ranges[0]); ++i)
    {
        const radio_font_range_t *range = &s_ranges[i];
        if (letter >= range->first && letter <= range->last)
        {
            *index = range->first_index + letter - range->first;
            *advance = range->advance;
            return true;
        }
    }

    /* Unsupported supplementary-plane characters are shown as '?'. */
    *index = (uint32_t)('?' - 0x20);
    *advance = 8;
    return true;
}

static bool get_glyph_dsc_16(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                             uint32_t letter, uint32_t letter_next)
{
    (void)font;
    (void)letter_next;

    uint32_t index;
    uint8_t advance;
    if (!find_glyph(letter, &index, &advance))
    {
        return false;
    }

    dsc->adv_w = advance;
    dsc->box_w = 16;
    dsc->box_h = 16;
    dsc->ofs_x = 0;
    dsc->ofs_y = 0;
    dsc->format = LV_FONT_GLYPH_FORMAT_A1;
    dsc->is_placeholder = false;
    dsc->gid.index = index + 1;
    return true;
}

static const void *get_glyph_bitmap_16(lv_font_glyph_dsc_t *dsc,
                                       lv_draw_buf_t *draw_buf)
{
    if (!draw_buf || !draw_buf->data || dsc->gid.index == 0)
    {
        return NULL;
    }

    const size_t font_size = (size_t)(radio_font_bin_end - radio_font_bin_start);
    if (font_size != RADIO_FONT_EXPECTED_BYTES)
    {
        return NULL;
    }

    const uint32_t index = dsc->gid.index - 1;
    const uint8_t *packed = radio_font_bin_start + index * RADIO_FONT_GLYPH_BYTES;
    uint8_t *output = draw_buf->data;
    const uint32_t stride = draw_buf->header.stride;

    for (uint32_t y = 0; y < 16; ++y)
    {
        const uint16_t row = ((uint16_t)packed[y * 2] << 8) | packed[y * 2 + 1];
        for (uint32_t x = 0; x < 16; ++x)
        {
            output[y * stride + x] = (row & (0x8000U >> x)) ? 0xff : 0x00;
        }
    }
    return draw_buf;
}

const lv_font_t radio_font_16 = {
    .get_glyph_dsc = get_glyph_dsc_16,
    .get_glyph_bitmap = get_glyph_bitmap_16,
    .release_glyph = NULL,
    .line_height = 18,
    .base_line = 2,
    .subpx = LV_FONT_SUBPX_NONE,
    .kerning = LV_FONT_KERNING_NONE,
    .underline_position = -2,
    .underline_thickness = 1,
    .dsc = NULL,
    .fallback = NULL,
    .user_data = NULL,
};

static bool get_glyph_dsc_24(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                             uint32_t letter, uint32_t letter_next)
{
    (void)font;
    (void)letter_next;

    uint32_t index;
    uint8_t advance;
    if (!find_glyph(letter, &index, &advance))
    {
        return false;
    }

    dsc->adv_w = advance * 3 / 2;
    dsc->box_w = 24;
    dsc->box_h = 24;
    dsc->ofs_x = 0;
    dsc->ofs_y = 0;
    dsc->format = LV_FONT_GLYPH_FORMAT_A1;
    dsc->is_placeholder = false;
    dsc->gid.index = index + 1;
    return true;
}

static const void *get_glyph_bitmap_24(lv_font_glyph_dsc_t *dsc,
                                       lv_draw_buf_t *draw_buf)
{
    if (!draw_buf || !draw_buf->data || dsc->gid.index == 0)
    {
        return NULL;
    }

    const size_t font_size = (size_t)(radio_font_bin_end - radio_font_bin_start);
    if (font_size != RADIO_FONT_EXPECTED_BYTES)
    {
        return NULL;
    }

    const uint32_t index = dsc->gid.index - 1;
    const uint8_t *packed = radio_font_bin_start + index * RADIO_FONT_GLYPH_BYTES;
    uint8_t *output = draw_buf->data;
    const uint32_t stride = draw_buf->header.stride;

    /* Nearest-neighbour scaling keeps the original bitmap crisp on the LCD. */
    for (uint32_t y = 0; y < 24; ++y)
    {
        const uint32_t source_y = y * 2 / 3;
        const uint16_t row = ((uint16_t)packed[source_y * 2] << 8) |
                             packed[source_y * 2 + 1];
        for (uint32_t x = 0; x < 24; ++x)
        {
            const uint32_t source_x = x * 2 / 3;
            output[y * stride + x] =
                (row & (0x8000U >> source_x)) ? 0xff : 0x00;
        }
    }
    return draw_buf;
}

const lv_font_t radio_font_24 = {
    .get_glyph_dsc = get_glyph_dsc_24,
    .get_glyph_bitmap = get_glyph_bitmap_24,
    .release_glyph = NULL,
    .line_height = 27,
    .base_line = 3,
    .subpx = LV_FONT_SUBPX_NONE,
    .kerning = LV_FONT_KERNING_NONE,
    .underline_position = -3,
    .underline_thickness = 1,
    .dsc = NULL,
    .fallback = NULL,
    .user_data = NULL,
};
