#include "box2_audio.h"
#include "box2_config.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "box2_audio";
static i2s_chan_handle_t s_tx_channel;
static i2s_chan_handle_t s_rx_channel;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_output_dev;
static esp_codec_dev_handle_t s_input_dev;
static volatile int s_volume = 50;
static volatile int s_engine_percent;
static volatile int s_pending_effect = -1;
static volatile int s_custom_frequency;
static volatile int s_custom_duration_ms;

static int16_t triangle_sample(uint32_t phase, int amplitude)
{
    uint16_t p = (uint16_t)(phase >> 16);
    int32_t wave = p < 32768 ? (int32_t)p : (int32_t)(65535 - p);
    return (int16_t)(((wave - 16384) * amplitude) / 16384);
}

static void audio_task(void *arg)
{
    (void)arg;
    enum { BLOCK_SAMPLES = 240 };
    int16_t samples[BLOCK_SAMPLES];
    uint32_t engine_phase = 0;
    uint32_t harmonic_phase = 0;
    uint32_t effect_phase = 0;
    uint32_t noise = 0x4a3b2c1d;
    int active_effect = -1;
    int effect_block = 0;
    int effect_blocks = 0;
    int applied_volume = -1;
    while (true) {
        if (applied_volume != s_volume) {
            applied_volume = s_volume;
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                esp_codec_dev_set_out_vol(s_output_dev, applied_volume));
        }
        int requested = s_pending_effect;
        if (requested >= 0) {
            s_pending_effect = -1;
            active_effect = requested;
            effect_block = 0;
            switch (active_effect) {
                case BOX2_SFX_MENU: effect_blocks = 9; break;
                case BOX2_SFX_START: effect_blocks = 48; break;
                case BOX2_SFX_PASS: effect_blocks = 16; break;
                case BOX2_SFX_CRASH: effect_blocks = 38; break;
                case BOX2_SFX_GAME_OVER: effect_blocks = 85; break;
                default: effect_blocks = s_custom_duration_ms / 10; break;
            }
            effect_phase = 0;
        }
        int engine = s_engine_percent;
        if (engine < 0) engine = 0;
        if (engine > 100) engine = 100;
        uint32_t engine_step = ((uint64_t)(55 + engine * 2) << 32) /
                               BOX2_AUDIO_SAMPLE_RATE;
        uint32_t harmonic_step = engine_step * 2 + engine_step / 2;
        for (int i = 0; i < BLOCK_SAMPLES; ++i) {
            int sample = 0;
            if (engine > 0) {
                sample = triangle_sample(engine_phase, 900 + engine * 16);
                sample += triangle_sample(harmonic_phase, 260 + engine * 5);
                engine_phase += engine_step;
                harmonic_phase += harmonic_step;
            }
            if (active_effect >= 0) {
                int progress = effect_block * 100 / (effect_blocks ? effect_blocks : 1);
                int frequency = 900;
                int amplitude = 6000;
                if (active_effect == BOX2_SFX_START) {
                    frequency = 380 + progress * 9;
                } else if (active_effect == BOX2_SFX_PASS) {
                    frequency = 1100 + progress * 8;
                } else if (active_effect == BOX2_SFX_GAME_OVER) {
                    frequency = 700 - progress * 5;
                    amplitude = 5200 - progress * 28;
                } else if (active_effect == BOX2_SFX_CRASH) {
                    noise ^= noise << 13;
                    noise ^= noise >> 17;
                    noise ^= noise << 5;
                    amplitude = 7500 - progress * 55;
                    sample += ((int)(noise & 0xffff) - 32768) * amplitude / 32768;
                    frequency = 120 - progress / 2;
                } else if (active_effect > BOX2_SFX_GAME_OVER) {
                    frequency = s_custom_frequency;
                }
                if (amplitude < 500) amplitude = 500;
                uint32_t step = ((uint64_t)(uint32_t)frequency << 32) /
                                BOX2_AUDIO_SAMPLE_RATE;
                sample += triangle_sample(effect_phase, amplitude);
                effect_phase += step;
            }
            if (sample > INT16_MAX) sample = INT16_MAX;
            if (sample < INT16_MIN) sample = INT16_MIN;
            samples[i] = (int16_t)sample;
        }
        if (esp_codec_dev_write(s_output_dev, samples, sizeof(samples)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (active_effect >= 0 && ++effect_block >= effect_blocks) {
            active_effect = -1;
        }
    }
}
esp_err_t box2_audio_init(i2c_master_bus_handle_t i2c_bus)
{
    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_cfg.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, &s_tx_channel, &s_rx_channel),
                        TAG, "create duplex I2S");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOX2_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOX2_I2S_MCLK,
            .bclk = BOX2_I2S_BCLK,
            .ws = BOX2_I2S_WS,
            .dout = BOX2_I2S_DOUT,
            .din = BOX2_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_channel, &std_cfg),
                        TAG, "configure I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_channel, &std_cfg),
                        TAG, "configure I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_channel), TAG, "enable I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_channel), TAG, "enable I2S RX");
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx_channel,
        .tx_handle = s_tx_channel,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if, ESP_FAIL, TAG, "create codec I2S interface");
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = BOX2_ES8389_ADDR_8BIT,
        .bus_handle = i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(s_ctrl_if, ESP_FAIL, TAG, "create codec I2C interface");
    s_gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(s_gpio_if, ESP_FAIL, TAG, "create codec GPIO interface");
    es8389_codec_cfg_t codec_cfg = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
        .no_dac_ref = false,
        .mclk_div = 256,
    };
    s_codec_if = es8389_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(s_codec_if, ESP_FAIL, TAG, "create ES8389 codec");
    esp_codec_dev_cfg_t output_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    esp_codec_dev_cfg_t input_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_output_dev = esp_codec_dev_new(&output_cfg);
    s_input_dev = esp_codec_dev_new(&input_cfg);
    ESP_RETURN_ON_FALSE(s_output_dev && s_input_dev, ESP_FAIL, TAG, "create codec devices");
    ESP_RETURN_ON_ERROR(esp_codec_set_disable_when_closed(s_output_dev, false),
                        TAG, "keep output codec active");
    ESP_RETURN_ON_ERROR(esp_codec_set_disable_when_closed(s_input_dev, false),
                        TAG, "keep input codec active");
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = BOX2_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_output_dev, &sample_info), TAG, "open speaker");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_input_dev, &sample_info), TAG, "open microphone");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_output_dev, s_volume), TAG,
                        "set default speaker volume");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_input_dev, 40.0f), TAG, "set mic gain");
    int chip_id0 = 0;
    int chip_id1 = 0;
    int id0_err = esp_codec_dev_read_reg(s_output_dev, 0xFD, &chip_id0);
    int id1_err = esp_codec_dev_read_reg(s_output_dev, 0xFE, &chip_id1);
    ESP_LOGI(TAG, "ES8389 register ID: %s FD=0x%02X FE=0x%02X",
             (id0_err == 0 && id1_err == 0) ? "read OK" : "unavailable",
             chip_id0 & 0xff, chip_id1 & 0xff);
    ESP_RETURN_ON_FALSE(xTaskCreate(audio_task, "race_audio", 4096, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "create audio task");
    return ESP_OK;
}

esp_err_t box2_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    return s_output_dev ? ESP_OK : ESP_ERR_INVALID_STATE;
}

int box2_audio_get_volume(void)
{
    return s_volume;
}

void box2_audio_set_engine(int speed_percent)
{
    if (speed_percent < 0) speed_percent = 0;
    if (speed_percent > 100) speed_percent = 100;
    s_engine_percent = speed_percent;
}

esp_err_t box2_audio_play_effect(box2_audio_effect_t effect)
{
    if (!s_output_dev || effect < BOX2_SFX_MENU || effect > BOX2_SFX_GAME_OVER) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pending_effect = effect;
    return ESP_OK;
}

esp_err_t box2_audio_play_tone(int frequency_hz, int duration_ms)
{
    if (!s_output_dev || frequency_hz <= 0 || duration_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_custom_frequency = frequency_hz;
    s_custom_duration_ms = duration_ms;
    s_pending_effect = BOX2_SFX_GAME_OVER + 1;
    return ESP_OK;
}
esp_err_t box2_audio_read_peak(int *peak)
{
    if (!s_input_dev || !peak) {
        return ESP_ERR_INVALID_ARG;
    }
    int16_t samples[240];
    int err = esp_codec_dev_read(s_input_dev, samples, sizeof(samples));
    if (err != ESP_OK) {
        return err;
    }
    int minimum = INT16_MAX;
    int maximum = INT16_MIN;
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        int value = samples[i];
        if (value < minimum) minimum = value;
        if (value > maximum) {
            maximum = value;
        }
    }
    *peak = (maximum - minimum) / 2;
    return ESP_OK;
}
