#include "box2_audio.h"
#include "box2_config.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
static const char *TAG = "box2_audio";
static i2s_chan_handle_t s_tx_channel;
static i2s_chan_handle_t s_rx_channel;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_output_dev;
static esp_codec_dev_handle_t s_input_dev;
esp_err_t box2_audio_init(i2c_master_bus_handle_t i2c_bus)
{
    if (s_output_dev && s_input_dev)
    {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(i2c_bus, ESP_ERR_INVALID_ARG, TAG, "I2C bus is null");
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
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_output_dev, 0), TAG, "silence speaker");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_input_dev, 40.0f), TAG, "set mic gain");
    int chip_id0 = 0;
    int chip_id1 = 0;
    int id0_err = esp_codec_dev_read_reg(s_output_dev, 0xFD, &chip_id0);
    int id1_err = esp_codec_dev_read_reg(s_output_dev, 0xFE, &chip_id1);
    ESP_LOGI(TAG, "ES8389 register ID: %s FD=0x%02X FE=0x%02X",
             (id0_err == 0 && id1_err == 0) ? "read OK" : "unavailable",
             chip_id0 & 0xff, chip_id1 & 0xff);
    return ESP_OK;
}
esp_err_t box2_audio_set_output_volume(int volume_percent)
{
    if (!s_output_dev || volume_percent < 0 || volume_percent > 100)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_codec_dev_set_out_vol(s_output_dev, volume_percent);
}

esp_err_t box2_audio_set_input_gain(float gain_db)
{
    if (!s_input_dev)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_set_in_gain(s_input_dev, gain_db);
}

esp_err_t box2_audio_write(const int16_t *samples, size_t sample_count)
{
    if (!s_output_dev || !samples || sample_count == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_codec_dev_write(s_output_dev, (void *)samples,
                               sample_count * sizeof(*samples));
}

esp_err_t box2_audio_read(int16_t *samples, size_t sample_count)
{
    if (!s_input_dev || !samples || sample_count == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_codec_dev_read(s_input_dev, samples, sample_count * sizeof(*samples));
}

uint32_t box2_audio_sample_rate(void)
{
    return BOX2_AUDIO_SAMPLE_RATE;
}
