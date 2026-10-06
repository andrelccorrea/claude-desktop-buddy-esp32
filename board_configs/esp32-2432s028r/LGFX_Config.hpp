// LovyanGFX panel configuration for the 2.8" 320x240 ILI9341 SPI panel with
// XPT2046 resistive touch (Sunton ESP32-2432S028R -- this specific board is
// the widely known "Cheap Yellow Display"/CYD in the hobbyist community).
// Unlike every other board in this project, this one is a classic ESP32
// (ESP32-D0WD-V3, confirmed via esptool on real hardware), NOT ESP32-S3 --
// no native USB, no RGB-parallel LCD peripheral, no PSRAM. Display is
// driven over SPI (SPI2_HOST) and touch over a SEPARATE SPI bus (SPI3_HOST),
// not I2C like every other board's GT911 touch.
//
// Pin values below come from the community board definition at
// https://github.com/rzeldent/platformio-espressif32-sunton/blob/main/esp32-2432S028R.json
// -- same source repo the other Sunton-family boards in this project were
// pulled from, and this being the well-known "CYD" board makes this pinout
// widely independently verified elsewhere too (unlike the RGB-panel boards,
// where this repo's data turned out wrong for the actual Elecrow-made
// units). Still, VERIFY against your actual board if the display doesn't
// come up cleanly.
#pragma once

#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
    lgfx::Bus_SPI _bus_instance;
    lgfx::Panel_ILI9341 _panel_instance;
    lgfx::Light_PWM _light_instance;
    lgfx::Touch_XPT2046 _touch_instance;

public:
    LGFX(void) {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 24000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 14;
            cfg.pin_mosi = 13;
            cfg.pin_miso = 12;
            cfg.pin_dc = 2;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs = 15;
            cfg.pin_rst = -1;
            cfg.pin_busy = -1;
            // This panel batch (TPM408-2.8 glass) is wired as a native
            // 320x240 landscape panel, mirrored, and takes RGB. Declared as
            // 320x240 with offset 7 (MADCTL MV|MX|MY), rotation 0 is upright
            // portrait 240x320; 240x320 without MV showed the image
            // transposed with 80 unwritten rows.
            cfg.memory_width = 320;
            cfg.memory_height = 240;
            cfg.panel_width = 320;
            cfg.panel_height = 240;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 7;
            cfg.rgb_order = true;
            cfg.invert = false;
            _panel_instance.config(cfg);
        }

        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = 21;
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        {
            // XPT2046 resistive touch, its own dedicated SPI bus (SPI3_HOST)
            // -- separate from the display's SPI2_HOST, confirmed from the
            // Sunton source (ILI9341 on SPI2_HOST, XPT2046 on SPI3_HOST).
            auto cfg = _touch_instance.config();
            cfg.spi_host = SPI3_HOST;
            cfg.freq = 2000000;
            cfg.pin_sclk = 25;
            cfg.pin_mosi = 32;
            cfg.pin_miso = 39;
            cfg.pin_cs = 33;
            cfg.pin_int = 36;
            cfg.bus_shared = false;
            cfg.x_min = 0;
            cfg.x_max = 239;
            cfg.y_min = 0;
            cfg.y_max = 319;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }

        setPanel(&_panel_instance);
    }
};
