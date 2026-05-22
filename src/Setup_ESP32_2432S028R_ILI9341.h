// TFT_eSPI — CYD ILI9341 (ESP32-2432S028R v1/v2, and some ESP32-S3 boards)

#define USER_SETUP_INFO "CYD ILI9341"

#define ILI9341_2_DRIVER
#define TFT_WIDTH 240
#define TFT_HEIGHT 320

#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST -1

#define TOUCH_CS 33

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4

#define SPI_READ_FREQUENCY 20000000
#define SPI_TOUCH_FREQUENCY 2500000

#if defined(CONFIG_IDF_TARGET_ESP32S3)
// ESP32-S3 CYD: HSPI port mapping often breaks the panel — use default SPI host
#define SPI_FREQUENCY 40000000
#else
#define USE_HSPI_PORT
#define SPI_FREQUENCY 55000000
#endif
