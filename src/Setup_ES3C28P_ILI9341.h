// lcdwiki ES3C28P / ES3N28P — 2.8" ESP32-S3, USB-C, ILI9341V
// https://www.lcdwiki.com/2.8inch_ESP32-S3_Display

#define USER_SETUP_INFO "ES3C28P ILI9341"

#define ILI9341_DRIVER
#define TFT_WIDTH 240
#define TFT_HEIGHT 320

#define TFT_CS 10
#define TFT_DC 46
#define TFT_RST -1
#define TFT_MOSI 11
#define TFT_MISO 13
#define TFT_SCLK 12

#define TFT_BL 45
#define TFT_BACKLIGHT_ON HIGH

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4

#define SPI_FREQUENCY 40000000
#define SPI_READ_FREQUENCY 20000000
