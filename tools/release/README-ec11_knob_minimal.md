# EC11 旋钮最小系统 / EC11 Knob Minimal System

硬件：ESP32-S3-DevKitC-1 N16R8、8 针 240×320 ST7789 SPI 屏、KY-040/EC11 旋钮模块。开发板使用 USB-C 供电，本机型没有触摸屏。

Hardware: ESP32-S3-DevKitC-1 N16R8, an 8-pin 240×320 ST7789 SPI display, and a KY-040/EC11 encoder module. Power the DevKit over USB-C. This target has no touch screen.

| Module / 模块 | ESP32-S3 |
|---|---|
| ST7789 GND / VCC | GND / 3V3 |
| ST7789 SCL(SCK) / SDA(MOSI) | GPIO21 / GPIO47 |
| ST7789 CS / DC / RST / BL | GPIO41 / GPIO40 / GPIO45 / GPIO42 |
| EC11 CLK(A) / DT(B) / SW | GPIO13 / GPIO14 / GPIO46 |
| EC11 GND / + (VCC) | GND / 3V3 |

屏幕上的 `SCL`、`SDA` 在这里是 SPI 时钟和数据，不是 I2C。接好线后，在 Windows 命令提示符中运行 `flash.bat COM6`，把 `COM6` 换成开发板的端口。

On the display module, `SCL` and `SDA` mean the SPI clock and data signals here, not I2C. After wiring, run `flash.bat COM6` in Windows Command Prompt and replace `COM6` with the DevKit's port.
