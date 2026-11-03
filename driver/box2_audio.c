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
static uint32_t s_sample_rate;

static esp_err_t open_device_checked(esp_codec_dev_handle_t device,
                                      esp_codec_dev_type_t type,
                                      esp_codec_dev_sample_info_t *sample_info)
{
    esp_err_t err = esp_codec_dev_open(device, sample_info);
    if (err != ESP_OK)
    {
        return err;
    }
    // esp_codec_dev 1.6.2 discards the data interface errors inside open().
    // Reapply the final format and enable it with checked return values.
    err = s_data_if->set_fmt(s_data_if, type, sample_info);
    if (err != ESP_OK)
    {
        return err;
    }
    return s_data_if->enable(s_data_if, type, true);
}

static void audio_init_cleanup(void)
{
    if (s_input_dev)
    {
        esp_codec_set_disable_when_closed(s_input_dev, true);
    }
    if (s_output_dev)
    {
        esp_codec_set_disable_when_closed(s_output_dev, true);
    }
    esp_codec_dev_delete(s_input_dev);
    s_input_dev = NULL;
    esp_codec_dev_delete(s_output_dev);
    s_output_dev = NULL;
    if (s_codec_if)
    {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_data_if)
    {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_ctrl_if)
    {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_gpio_if)
    {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_rx_channel)
    {
        // A failure can leave either READY or RUNNING channels.
        i2s_channel_disable(s_rx_channel);
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }
    if (s_tx_channel)
    {
        i2s_channel_disable(s_tx_channel);
        i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
    }
    s_sample_rate = 0;
}

esp_err_t box2_audio_init(i2c_master_bus_handle_t i2c_bus)
{
    return box2_audio_init_with_sample_rate(i2c_bus, BOX2_AUDIO_SAMPLE_RATE);
}

esp_err_t box2_audio_init_with_sample_rate(i2c_master_bus_handle_t i2c_bus,
                                           uint32_t sample_rate)
{
    ESP_RETURN_ON_FALSE(i2c_bus, ESP_ERR_INVALID_ARG, TAG, "I2C bus is null");
    ESP_RETURN_ON_FALSE(sample_rate == 24000 || sample_rate == 48000,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported audio sample rate");
    if (s_sample_rate != 0)
    {
        return s_sample_rate == sample_rate ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_cfg.auto_clear_after_cb = true;
    ESP_GOTO_ON_ERROR(i2s_new_channel(&channel_cfg, &s_tx_channel, &s_rx_channel),
                      fail, TAG, "create duplex I2S");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
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
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_tx_channel, &std_cfg),
                      fail, TAG, "configure I2S TX");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_rx_channel, &std_cfg),
                      fail, TAG, "configure I2S RX");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(s_tx_channel), fail, TAG, "enable I2S TX");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(s_rx_channel), fail, TAG, "enable I2S RX");
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx_channel,
        .tx_handle = s_tx_channel,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_GOTO_ON_FALSE(s_data_if, ESP_FAIL, fail, TAG, "create codec I2S interface");
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = BOX2_ES8389_ADDR_8BIT,
        .bus_handle = i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_GOTO_ON_FALSE(s_ctrl_if, ESP_FAIL, fail, TAG, "create codec I2C interface");
    s_gpio_if = audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(s_gpio_if, ESP_FAIL, fail, TAG, "create codec GPIO interface");
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
    ESP_GOTO_ON_FALSE(s_codec_if, ESP_FAIL, fail, TAG, "create ES8389 codec");
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
    ESP_GOTO_ON_FALSE(s_output_dev && s_input_dev, ESP_FAIL, fail, TAG, "create codec devices");
    ESP_GOTO_ON_ERROR(esp_codec_set_disable_when_closed(s_output_dev, false),
                      fail, TAG, "keep output codec active");
    ESP_GOTO_ON_ERROR(esp_codec_set_disable_when_closed(s_input_dev, false),
                      fail, TAG, "keep input codec active");
    esp_codec_dev_sample_info_t input_info = {
        .bits_per_sample = 16,
        .channel = 1,
        // esp_codec_dev 1.6.2 maps mono to ch0 / I2S LEFT: ES8389 ADCL.
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };
    /*
     * The speaker has to be driven as stereo. With no_dac_ref = false the
     * ES8389 derives its DAC reference from the second slot; a mono-left TX
     * stream leaves that slot at zero, which shifts the differential output to
     * one rail. The amplifier then turns that DC step into a loud thump every
     * time SPK_EN toggles. Both slots get the same signal below.
     */
    esp_codec_dev_sample_info_t output_info = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0, // Let the codec layer enable both slots.
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };
    ESP_GOTO_ON_ERROR(open_device_checked(s_output_dev, ESP_CODEC_DEV_TYPE_OUT, &output_info),
                      fail, TAG, "open speaker and configure I2S TX");
    ESP_GOTO_ON_ERROR(open_device_checked(s_input_dev, ESP_CODEC_DEV_TYPE_IN, &input_info),
                      fail, TAG, "open microphone and configure I2S RX");
    ESP_GOTO_ON_ERROR(esp_codec_dev_set_out_vol(s_output_dev, 0), fail, TAG, "silence speaker");
    ESP_GOTO_ON_ERROR(esp_codec_dev_set_in_gain(s_input_dev, 40.0f), fail, TAG, "set mic gain");
    s_sample_rate = sample_rate;
    return ESP_OK;
fail:
    audio_init_cleanup();
    return ret;
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

esp_err_t box2_audio_write(const int16_t *stereo_frames, size_t frame_count)
{
    if (!s_output_dev || !stereo_frames || frame_count == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    // The TX slot is stereo, so every frame is left + right interleaved.
    return esp_codec_dev_write(s_output_dev, (void *)stereo_frames,
                               frame_count * 2 * sizeof(*stereo_frames));
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
    return s_sample_rate;
}
