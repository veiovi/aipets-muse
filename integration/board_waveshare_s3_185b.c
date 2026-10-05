/* SPDX-License-Identifier: Apache-2.0
 * Waveshare ESP32-S3-Touch-LCD-1.85B, 360px ST77916/CST816S, 16MB flash/8MB PSRAM.
 * Pins/codecs/panel sequences: waveshareteam/ESP32-S3-Touch-LCD-1.85B
 * commit 139e6db584f3737fcfc6a958ee83b79fb69d317c, 01_comprehensive_example BSP.
 * PWR is a hardware latch, not a GPIO. BOOT is the sole software button.
 * Uses Muse's LVGL adapter and bounded DMA bands; no second BSP/UI stack.
 */
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"
#include "waveshare_185b_panel_init.h"

#define LCD_SIZE 360
#define LCD_CHUNK_BYTES (LCD_SIZE * 8 * 2)
static const char *TAG = "board_185b";
static i2c_master_bus_handle_t s_bus;
static muse_gpio_button_t s_boot;

static esp_err_t init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_11,
        .scl_io_num = GPIO_NUM_10,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_bus), TAG, "shared I2C");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, GPIO_NUM_0), TAG, "BOOT");
    s_boot.pressed = gpio_get_level(GPIO_NUM_0) == 0;
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t backlight = {
        .gpio_num = GPIO_NUM_5,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    return ledc_channel_config(&backlight);
}

static esp_err_t panel_new(esp_lcd_panel_handle_t *panel, esp_lcd_panel_io_handle_t *io)
{
    const gpio_config_t reset = {
        .pin_bit_mask = 1ULL << GPIO_NUM_3,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&reset), TAG, "LCD reset GPIO");
    gpio_set_level(GPIO_NUM_3, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(GPIO_NUM_3, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    const spi_bus_config_t bus = {
        .sclk_io_num = GPIO_NUM_40,
        .data0_io_num = GPIO_NUM_46,
        .data1_io_num = GPIO_NUM_45,
        .data2_io_num = GPIO_NUM_42,
        .data3_io_num = GPIO_NUM_41,
        .data4_io_num = -1, .data5_io_num = -1,
        .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = LCD_CHUNK_BYTES,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "LCD SPI");
    esp_lcd_panel_io_spi_config_t cfg = {
        .cs_gpio_num = GPIO_NUM_21,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 3000000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &cfg, io), TAG, "LCD IO");
    uint8_t id[4] = {0};
    esp_err_t result = esp_lcd_panel_io_rx_param(*io, (0x0B << 24) | (0x04 << 8), id, sizeof(id));
    /* Remove the low-speed probe before creating the drawing device. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(*io), TAG, "LCD probe cleanup");
    *io = NULL;
    ESP_RETURN_ON_ERROR(result, TAG, "LCD ID read");
    st77916_vendor_config_t vendor = { .flags.use_qspi_interface = true };
    if (id[0] == 0 && id[1] == 0x7F && id[2] == 0x7F && id[3] == 0x7F)
    {
        vendor.init_cmds = vendor_specific_init_version_1;
        vendor.init_cmds_size = sizeof(vendor_specific_init_version_1) / sizeof(st77916_lcd_init_cmd_t);
    }
    else if (id[0] == 0 && id[1] == 2 && id[2] == 0x7F && id[3] == 0x7F)
    {
        vendor.init_cmds = vendor_specific_init_version_2;
        vendor.init_cmds_size = sizeof(vendor_specific_init_version_2) / sizeof(st77916_lcd_init_cmd_t);
    }
    else
    {
        ESP_LOGE(TAG, "Unsupported LCD ID %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "ST77916 panel revision %d", id[1] == 2 ? 2 : 1);
    cfg.pclk_hz = 80000000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &cfg, io), TAG, "LCD IO");
    const esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = GPIO_NUM_3,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(*io, &dev, panel), TAG, "ST77916");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "LCD init");
    return esp_lcd_panel_disp_on_off(*panel, true);
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter.task_core_id = MUSE_UI_CORE;
    adapter.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter) != ESP_OK)
    {
        return NULL;
    }
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    if (panel_new(&panel, &io) != ESP_OK)
    {
        return NULL;
    }
    const esp_lv_adapter_display_config_t cfg = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_SIZE,
            .ver_res = LCD_SIZE,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(cfg, 90, LCD_CHUNK_BYTES);
    if (!disp)
    {
        return NULL;
    }
    esp_lcd_panel_io_i2c_config_t touch_io = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    touch_io.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t tp_io = NULL;
    if (esp_lcd_new_panel_io_i2c(s_bus, &touch_io, &tp_io) != ESP_OK)
    {
        return NULL;
    }
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_SIZE, .y_max = LCD_SIZE,
        .rst_gpio_num = GPIO_NUM_1,
        .int_gpio_num = GPIO_NUM_4,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    esp_lcd_touch_handle_t tp = NULL;
    if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp) != ESP_OK)
    {
        return NULL;
    }
    const esp_lv_adapter_touch_config_t input = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);
    *touch = esp_lv_adapter_register_touch(&input);
    return *touch && esp_lv_adapter_start() == ESP_OK ? disp : NULL;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * pct / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &tx, &rx), TAG, "I2S duplex");
    const i2s_std_config_t port = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = 2, .bclk = 48, .ws = 38, .dout = 47, .din = 39 },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &port), TAG, "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &port), TAG, "I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "enable TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "enable RX");
    audio_codec_i2s_cfg_t data_cfg = { .port = I2S_NUM_0, .tx_handle = tx, .rx_handle = rx };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t adc_i2c = {
        .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_bus,
    };
    es7210_codec_cfg_t adc_cfg = { .ctrl_if = audio_codec_new_i2c_ctrl(&adc_i2c) };
    if (!data || !adc_cfg.ctrl_if)
    {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_cfg_t adc = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210_codec_new(&adc_cfg), .data_if = data,
    };
    *mic = adc.codec_if ? esp_codec_dev_new(&adc) : NULL;
    audio_codec_i2c_cfg_t dac_i2c = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_bus,
    };
    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = audio_codec_new_i2c_ctrl(&dac_i2c),
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_9,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    if (!dac_cfg.ctrl_if || !dac_cfg.gpio_if)
    {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_cfg_t dac = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_codec_new(&dac_cfg), .data_if = data,
    };
    *spk = dac.codec_if ? esp_codec_dev_new(&dac) : NULL;
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_boot);
}

static esp_err_t power_off(void)
{
    /* The hardware PWR latch has no software control; never drive LCD reset as PWR. */
    return ESP_ERR_NOT_SUPPORTED;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.85B",
    .width = LCD_SIZE, .height = LCD_SIZE,
    .round = true, .touch = true, .diagonal_in = 1.85f,
    .talk_button = "boot",
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -12 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .audio_init = audio_init,
    .mic_slot = -1,
    .poll_buttons = poll_buttons,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
