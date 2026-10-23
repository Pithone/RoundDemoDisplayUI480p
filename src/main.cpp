#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_timer.h>
#include <driver/spi_master.h>
#include <lvgl.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <cmath>
#include <dirent.h>
#include <string>
#include <vector>

#include "board_config.h"
#include "board_io.h"
#include "sd_card.h"

extern "C" void* lvglMemoryPoolAlloc(size_t size) {
    void* pool = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pool == nullptr) {
        Serial.printf("[ERROR] Could not allocate %u-byte LVGL pool in PSRAM\n",
                      static_cast<unsigned>(size));
    }
    return pool;
}

namespace {

constexpr uint32_t kTouchPollMs = 20;
constexpr uint32_t kLongHoldWarningMs = 4000;
constexpr uint32_t kLongHoldSleepMs = 5000;
constexpr uint32_t kIgnoredTouchReleaseMs = 1500;
constexpr uint32_t kPanelSleepTransitionDelayMs = 120;
constexpr uint32_t kPanelCommandDelayMs = 20;
constexpr uint32_t kDimBeforeSleepMs = 5U * 60U * 1000U;
constexpr uint32_t kBeepMs = 90;
constexpr int16_t kSwipeDistance = 65;
constexpr int16_t kHoldMovementLimit = 20;
constexpr int16_t kSettingsTapMaxMovement = 35;
constexpr uint32_t kSettingsTapMaxDurationMs = 800;
constexpr int16_t kSettingsTapAreaHeight = board::kHeight / 6;
constexpr size_t kMaximumImageBytes = 480U * 480U * 4U;
constexpr char kSdDirectory[] = "/sdcard";

struct UiImage {
    std::string filename;
    uint16_t width;
    uint16_t height;
};

struct UiPage {
    lv_obj_t* screen;
    lv_obj_t* background;
    lv_obj_t* settings;
    lv_obj_t* backgroundColorLabel;
};

struct BackgroundColor {
    const char* name;
    uint32_t rgb;
};

constexpr BackgroundColor kBackgroundColors[] = {
    {"White", 0xFFFFFF},
    {"Black", 0x000000},
    {"50% gray", 0x808080},
    {"Orange", 0xE98300},
    {"Teal", 0x008294},
};

esp_lcd_panel_handle_t panelHandle = nullptr;
spi_device_handle_t panelCommandDevice = nullptr;
lv_display_t* displayHandle = nullptr;
lv_obj_t* sleepOverlay = nullptr;
TaskHandle_t lvglTaskHandle = nullptr;
void* frameBuffer0 = nullptr;
void* frameBuffer1 = nullptr;

std::vector<UiImage> uiImages;
std::vector<UiPage> uiPages;
size_t currentPage = 0;
bool settingsOpen = false;
bool touchAvailable = false;
bool displaySleeping = false;
bool touchDown = false;
bool previousTouchDown = false;
bool ignoreTouchUntilRelease = false;
bool gestureEligible = false;
bool holdEligible = false;
bool holdWarningPlayed = false;
bool beepActive = false;
bool dimmed = false;
int16_t touchX = 0;
int16_t touchY = 0;
int16_t gestureStartX = 0;
int16_t gestureStartY = 0;
uint32_t gestureStartMs = 0;
uint32_t touchPollAt = 0;
uint32_t beepOffAt = 0;
uint32_t lastInteractionAt = 0;
uint32_t lastAutoRotateAt = 0;
uint32_t ignoredTouchReleaseAt = 0;
uint8_t selectedBrightness = board::kDefaultBrightness;
uint8_t autoRotateSelection = 2;
uint8_t autoSleepSelection = 2;
size_t backgroundColorSelection = 0;
std::string cardError;
std::string imageError;

lv_obj_t* brightnessSlider = nullptr;
lv_obj_t* autoRotateDropdown = nullptr;
lv_obj_t* autoSleepDropdown = nullptr;
lv_obj_t* backgroundColorLabel = nullptr;

constexpr uint16_t kDiagnosticColors[5] = {
    0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000,
};
constexpr const char* kDiagnosticNames[5] = {
    "red", "green", "blue", "white", "black",
};

bool expanderSet(uint8_t bit, bool high) {
    if (!expanderWriteBit(bit, high)) {
        Serial.printf("[ERROR] TCA9554 write failed for bit %u\n", bit);
        return false;
    }
    return true;
}

esp_err_t panelWriteWord(bool data, uint8_t value) {
    spi_transaction_t transaction = {};
    transaction.cmd = data ? 1 : 0;
    transaction.addr = value;
    return spi_device_polling_transmit(panelCommandDevice, &transaction);
}

bool panelWrite(uint8_t command, std::initializer_list<uint8_t> data = {}) {
    if (!expanderSet(board::kLcdChipSelectBit, false)) {
        return false;
    }

    esp_err_t result = panelWriteWord(false, command);
    for (const uint8_t value : data) {
        if (result != ESP_OK) {
            break;
        }
        result = panelWriteWord(true, value);
    }

    const bool deselected = expanderSet(board::kLcdChipSelectBit, true);
    if (result != ESP_OK) {
        Serial.printf("[ERROR] ST7701 command 0x%02X failed: %s\n",
                      command, esp_err_to_name(result));
        return false;
    }
    return deselected;
}

bool initializePanelCommandBus() {
    spi_bus_config_t busConfig = {};
    busConfig.mosi_io_num = board::kLcdCommandMosi;
    busConfig.miso_io_num = -1;
    busConfig.sclk_io_num = board::kLcdCommandClock;
    busConfig.quadwp_io_num = -1;
    busConfig.quadhd_io_num = -1;
    busConfig.max_transfer_sz = 64;

    esp_err_t result = spi_bus_initialize(board::kLcdCommandHost,
                                          &busConfig, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) {
        Serial.printf("[ERROR] LCD command SPI bus init failed: %s\n",
                      esp_err_to_name(result));
        return false;
    }

    spi_device_interface_config_t deviceConfig = {};
    deviceConfig.command_bits = 1;
    deviceConfig.address_bits = 8;
    deviceConfig.mode = 0;
    deviceConfig.clock_speed_hz = 4'000'000;
    deviceConfig.spics_io_num = -1;
    deviceConfig.queue_size = 1;
    result = spi_bus_add_device(board::kLcdCommandHost, &deviceConfig,
                                &panelCommandDevice);
    if (result != ESP_OK) {
        Serial.printf("[ERROR] LCD command SPI device init failed: %s\n",
                      esp_err_to_name(result));
        return false;
    }
    Serial.println("[DISPLAY] ST7701 command SPI ready (GPIO 1/2)");
    return true;
}

bool initializeSt7701() {
    if (!expanderSet(board::kLcdResetBit, false)) {
        return false;
    }
    delay(20);
    if (!expanderSet(board::kLcdResetBit, true)) {
        return false;
    }
    delay(120);

    const bool configured =
        panelWrite(0xFF, {0x77, 0x01, 0x00, 0x00, 0x13}) &&
        panelWrite(0xEF, {0x08}) &&
        panelWrite(0xFF, {0x77, 0x01, 0x00, 0x00, 0x10}) &&
        panelWrite(0xC0, {0x3B, 0x00}) &&
        panelWrite(0xC1, {0x10, 0x0C}) &&
        panelWrite(0xC2, {0x07, 0x0A}) &&
        panelWrite(0xC7, {0x00}) &&
        panelWrite(0xCC, {0x10}) &&
        panelWrite(0xCD, {0x08}) &&
        panelWrite(0xB0, {0x05, 0x12, 0x98, 0x0E, 0x0F, 0x07, 0x07, 0x09,
                          0x09, 0x23, 0x05, 0x52, 0x0F, 0x67, 0x2C, 0x11}) &&
        panelWrite(0xB1, {0x0B, 0x11, 0x97, 0x0C, 0x12, 0x06, 0x06, 0x08,
                          0x08, 0x22, 0x03, 0x51, 0x11, 0x66, 0x2B, 0x0F}) &&
        panelWrite(0xFF, {0x77, 0x01, 0x00, 0x00, 0x11}) &&
        panelWrite(0xB0, {0x5D}) &&
        panelWrite(0xB1, {0x3E}) &&
        panelWrite(0xB2, {0x81}) &&
        panelWrite(0xB3, {0x80}) &&
        panelWrite(0xB5, {0x4E}) &&
        panelWrite(0xB7, {0x85}) &&
        panelWrite(0xB8, {0x20}) &&
        panelWrite(0xC1, {0x78}) &&
        panelWrite(0xC2, {0x78}) &&
        panelWrite(0xD0, {0x88}) &&
        panelWrite(0xE0, {0x00, 0x00, 0x02}) &&
        panelWrite(0xE1, {0x06, 0x30, 0x08, 0x30, 0x05, 0x30, 0x07, 0x30,
                          0x00, 0x33, 0x33}) &&
        panelWrite(0xE2, {0x11, 0x11, 0x33, 0x33, 0xF4, 0x00, 0x00, 0x00,
                          0xF4, 0x00, 0x00, 0x00}) &&
        panelWrite(0xE3, {0x00, 0x00, 0x11, 0x11}) &&
        panelWrite(0xE4, {0x44, 0x44}) &&
        panelWrite(0xE5, {0x0D, 0xF5, 0x30, 0xF0, 0x0F, 0xF7, 0x30, 0xF0,
                          0x09, 0xF1, 0x30, 0xF0, 0x0B, 0xF3, 0x30, 0xF0}) &&
        panelWrite(0xE6, {0x00, 0x00, 0x11, 0x11}) &&
        panelWrite(0xE7, {0x44, 0x44}) &&
        panelWrite(0xE8, {0x0C, 0xF4, 0x30, 0xF0, 0x0E, 0xF6, 0x30, 0xF0,
                          0x08, 0xF0, 0x30, 0xF0, 0x0A, 0xF2, 0x30, 0xF0}) &&
        panelWrite(0xE9, {0x36, 0x01}) &&
        panelWrite(0xEB, {0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40}) &&
        panelWrite(0xED, {0xFF, 0x10, 0xAF, 0x76, 0x54, 0x2B, 0xCF, 0xFF,
                          0xFF, 0xFC, 0xB2, 0x45, 0x67, 0xFA, 0x01, 0xFF}) &&
        panelWrite(0xEF, {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}) &&
        panelWrite(0xFF, {0x77, 0x01, 0x00, 0x00, 0x00}) &&
        panelWrite(0x11);
    if (!configured) {
        return false;
    }
    delay(120);

    if (!panelWrite(0x3A, {0x66}) ||
        !panelWrite(0x36, {0x00}) ||
        !panelWrite(0x35, {0x00}) ||
        !panelWrite(0x29)) {
        return false;
    }
    Serial.println("[DISPLAY] ST7701 initialized (0x3A=0x66, 0x36=0x00)");
    return true;
}

bool initializeRgbPanel() {
    esp_lcd_rgb_panel_config_t config = {};
    config.clk_src = LCD_CLK_SRC_PLL160M;
    config.data_width = 16;
    config.num_fbs = 2;
    config.bounce_buffer_size_px =
        board::kWidth * board::kRgbBounceBufferLines;
    config.hsync_gpio_num = board::kRgbHsync;
    config.vsync_gpio_num = board::kRgbVsync;
    config.de_gpio_num = board::kRgbDe;
    config.pclk_gpio_num = board::kRgbPclk;
    config.disp_gpio_num = -1;
    for (size_t index = 0; index < 16; ++index) {
        config.data_gpio_nums[index] = board::kRgbData[index];
    }
    config.timings.pclk_hz = board::kPixelClockHz;
    config.timings.h_res = board::kWidth;
    config.timings.v_res = board::kHeight;
    config.timings.hsync_pulse_width = board::kHsyncPulse;
    config.timings.hsync_back_porch = board::kHsyncBackPorch;
    config.timings.hsync_front_porch = board::kHsyncFrontPorch;
    config.timings.vsync_pulse_width = board::kVsyncPulse;
    config.timings.vsync_back_porch = board::kVsyncBackPorch;
    config.timings.vsync_front_porch = board::kVsyncFrontPorch;
    config.timings.flags.pclk_active_neg = 0;
    config.flags.fb_in_psram = true;

    esp_err_t result = esp_lcd_new_rgb_panel(&config, &panelHandle);
    if (result != ESP_OK) {
        Serial.printf("[ERROR] RGB panel creation failed: %s\n",
                      esp_err_to_name(result));
        return false;
    }
    result = esp_lcd_panel_reset(panelHandle);
    if (result == ESP_OK) {
        result = esp_lcd_panel_init(panelHandle);
    }
    if (result != ESP_OK) {
        Serial.printf("[ERROR] RGB panel initialization failed: %s\n",
                      esp_err_to_name(result));
        return false;
    }

    result = esp_lcd_rgb_panel_get_frame_buffer(panelHandle, 2,
                                                &frameBuffer0, &frameBuffer1);
    if (result != ESP_OK || frameBuffer0 == nullptr || frameBuffer1 == nullptr) {
        Serial.printf("[ERROR] RGB framebuffer allocation failed: %s\n",
                      esp_err_to_name(result));
        return false;
    }
    Serial.printf("[DISPLAY] RGB driver ready: two PSRAM framebuffers (%u bytes each)\n",
                  static_cast<unsigned>(board::kWidth * board::kHeight * 2));
    return true;
}

bool initializeBacklight() {
    if (!ledcAttach(board::kLcdBacklight, board::kBacklightPwmHz,
                    board::kBacklightPwmBits)) {
        Serial.println("[ERROR] Backlight PWM setup failed");
        return false;
    }
    return true;
}

void setBrightness(uint8_t brightness) {
    const uint32_t maximum = (1U << board::kBacklightPwmBits) - 1U;
    const uint32_t duty = (static_cast<uint32_t>(brightness) * maximum) / 255U;
    ledcWrite(board::kLcdBacklight, duty);
}

uint32_t lvglTick() {
    return millis();
}

bool writeTouchRegister(uint16_t address, uint8_t value) {
    Wire.beginTransmission(board::kTouchAddress);
    Wire.write(static_cast<uint8_t>(address >> 8));
    Wire.write(static_cast<uint8_t>(address));
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool readTouchRegister(uint16_t address, uint8_t* buffer, size_t length) {
    Wire.beginTransmission(board::kTouchAddress);
    Wire.write(static_cast<uint8_t>(address >> 8));
    Wire.write(static_cast<uint8_t>(address));
    if (Wire.endTransmission(true) != 0) {
        return false;
    }
    const size_t received = Wire.requestFrom(
        static_cast<uint8_t>(board::kTouchAddress),
        static_cast<uint8_t>(length));
    if (received != length) {
        while (Wire.available()) {
            Wire.read();
        }
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        buffer[index] = static_cast<uint8_t>(Wire.read());
    }
    return true;
}

bool readGt911(int16_t& x, int16_t& y, bool& isDown) {
    uint8_t status = 0;
    if (!readTouchRegister(board::kTouchStatusRegister, &status, 1)) {
        return false;
    }
    if ((status & 0x80U) == 0) {
        isDown = false;
        return true;
    }

    const uint8_t pointCount = status & 0x0FU;
    if (pointCount > 0) {
        uint8_t point[4];
        if (!readTouchRegister(board::kTouchPointRegister, point, sizeof(point))) {
            writeTouchRegister(board::kTouchStatusRegister, 0);
            return false;
        }
        x = static_cast<int16_t>(point[0] | (point[1] << 8));
        y = static_cast<int16_t>(point[2] | (point[3] << 8));
        x = std::clamp<int16_t>(x, 0, board::kWidth - 1);
        y = std::clamp<int16_t>(y, 0, board::kHeight - 1);
        isDown = true;
    } else {
        isDown = false;
    }

    return writeTouchRegister(board::kTouchStatusRegister, 0);
}

void lvglReadTouch(lv_indev_t*, lv_indev_data_t* data) {
    data->state = touchDown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = touchX;
    data->point.y = touchY;
}

void lvglLogPrint(lv_log_level_t, const char* message) {
    Serial.printf("[LVGL] %s", message);
}

static bool IRAM_ATTR onFrameBufferComplete(
    esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*) {
    if (lvglTaskHandle == nullptr) {
        return false;
    }
    BaseType_t shouldYield = pdFALSE;
    vTaskNotifyGiveFromISR(lvglTaskHandle, &shouldYield);
    return shouldYield == pdTRUE;
}

void displayFlush(lv_display_t* display, const lv_area_t*, uint8_t* pixels) {
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }

    ulTaskNotifyTake(pdTRUE, 0);
    const esp_err_t result = esp_lcd_panel_draw_bitmap(
        panelHandle, 0, 0, board::kWidth, board::kHeight, pixels);
    if (result != ESP_OK) {
        Serial.printf("[ERROR] RGB framebuffer flip failed: %s\n",
                      esp_err_to_name(result));
        lv_display_flush_ready(display);
        abort();
    }
}

void displayFlushWait(lv_display_t* display) {
    if (lv_display_flush_is_last(display)) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
            Serial.println("[FATAL] Timed out waiting for RGB frame-buffer completion");
            abort();
        }
    }
    lv_display_flush_ready(display);
}

void initializeLvgl() {
    lv_init();
    lv_log_register_print_cb(lvglLogPrint);
    lv_tick_set_cb(lvglTick);
    displayHandle = lv_display_create(board::kWidth, board::kHeight);
    lv_display_set_color_format(displayHandle, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(
        displayHandle,
        frameBuffer1,
        frameBuffer0,
        board::kWidth * board::kHeight * sizeof(uint16_t),
        LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(displayHandle, displayFlush);
    lv_display_set_flush_wait_cb(displayHandle, displayFlushWait);

    sleepOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(sleepOverlay, board::kWidth, board::kHeight);
    lv_obj_set_pos(sleepOverlay, 0, 0);
    lv_obj_set_style_bg_color(sleepOverlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sleepOverlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sleepOverlay, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sleepOverlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sleepOverlay, 0, LV_PART_MAIN);
    lv_obj_add_flag(sleepOverlay, LV_OBJ_FLAG_HIDDEN);

    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_frame_buf_complete = onFrameBufferComplete,
    };
    const esp_err_t result = esp_lcd_rgb_panel_register_event_callbacks(
        panelHandle, &callbacks, nullptr);
    if (result != ESP_OK) {
        Serial.printf("[ERROR] RGB frame callback registration failed: %s\n",
                      esp_err_to_name(result));
        abort();
    }
    lvglTaskHandle = xTaskGetCurrentTaskHandle();
    Serial.println("[LVGL] Direct mode starts on the inactive scanout framebuffer");
}

void runColorDiagnostic() {
    lv_obj_t* screen = lv_scr_act();
    for (size_t index = 0; index < 5; ++index) {
        lv_obj_set_style_bg_color(
            screen,
            lv_color_make(
                static_cast<uint8_t>(((kDiagnosticColors[index] >> 11) & 0x1FU) * 255U / 31U),
                static_cast<uint8_t>(((kDiagnosticColors[index] >> 5) & 0x3FU) * 255U / 63U),
                static_cast<uint8_t>((kDiagnosticColors[index] & 0x1FU) * 255U / 31U)),
            LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_invalidate(screen);
        lv_refr_now(displayHandle);
        Serial.printf("[DISPLAY] Solid-color test: %s\n", kDiagnosticNames[index]);
        delay(600);
    }
}

uint32_t readBigEndian32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

bool matchesUiPng(const char* filename) {
    const size_t length = std::strlen(filename);
    if (length < 6 || std::toupper(static_cast<unsigned char>(filename[0])) != 'U' ||
        std::toupper(static_cast<unsigned char>(filename[1])) != 'I') {
        return false;
    }
    return std::tolower(static_cast<unsigned char>(filename[length - 4])) == '.' &&
           std::tolower(static_cast<unsigned char>(filename[length - 3])) == 'p' &&
           std::tolower(static_cast<unsigned char>(filename[length - 2])) == 'n' &&
           std::tolower(static_cast<unsigned char>(filename[length - 1])) == 'g';
}

bool inspectPng(const std::string& filename, UiImage& result) {
    char path[96];
    const int pathLength = std::snprintf(
        path, sizeof(path), "%s/%s", kSdDirectory, filename.c_str());
    if (pathLength < 0 || static_cast<size_t>(pathLength) >= sizeof(path)) {
        Serial.printf("[SD] Skipping overlong path: %s\n", filename.c_str());
        return false;
    }

    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        Serial.printf("[SD] Could not open %s\n", path);
        return false;
    }
    uint8_t header[24];
    const size_t bytesRead = std::fread(header, 1, sizeof(header), file);
    std::fclose(file);
    const uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytesRead != sizeof(header) ||
        std::memcmp(header, signature, sizeof(signature)) != 0 ||
        std::memcmp(header + 12, "IHDR", 4) != 0) {
        Serial.printf("[SD] Skipping invalid PNG header: %s\n", filename.c_str());
        return false;
    }

    const uint32_t width = readBigEndian32(header + 16);
    const uint32_t height = readBigEndian32(header + 20);
    if (width == 0 || height == 0 ||
        width > static_cast<uint32_t>(board::kWidth) ||
        height > static_cast<uint32_t>(board::kHeight) ||
        static_cast<uint64_t>(width) * height * 4U > kMaximumImageBytes) {
        Serial.printf("[SD] Skipping unsupported PNG dimensions %lu x %lu: %s\n",
                      static_cast<unsigned long>(width),
                      static_cast<unsigned long>(height), filename.c_str());
        return false;
    }

    result = {filename, static_cast<uint16_t>(width),
              static_cast<uint16_t>(height)};
    return true;
}

std::string lowerCase(const std::string& value) {
    std::string result = value;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return result;
}

int compareNatural(const std::string& lhs, const std::string& rhs) {
    size_t left = 0;
    size_t right = 0;
    while (left < lhs.size() && right < rhs.size()) {
        const unsigned char leftChar = static_cast<unsigned char>(lhs[left]);
        const unsigned char rightChar = static_cast<unsigned char>(rhs[right]);
        if (std::isdigit(leftChar) && std::isdigit(rightChar)) {
            size_t leftEnd = left;
            size_t rightEnd = right;
            while (leftEnd < lhs.size() &&
                   std::isdigit(static_cast<unsigned char>(lhs[leftEnd]))) {
                ++leftEnd;
            }
            while (rightEnd < rhs.size() &&
                   std::isdigit(static_cast<unsigned char>(rhs[rightEnd]))) {
                ++rightEnd;
            }

            size_t leftSignificant = left;
            size_t rightSignificant = right;
            while (leftSignificant + 1 < leftEnd &&
                   lhs[leftSignificant] == '0') {
                ++leftSignificant;
            }
            while (rightSignificant + 1 < rightEnd &&
                   rhs[rightSignificant] == '0') {
                ++rightSignificant;
            }
            const size_t leftDigits = leftEnd - leftSignificant;
            const size_t rightDigits = rightEnd - rightSignificant;
            if (leftDigits != rightDigits) {
                return leftDigits < rightDigits ? -1 : 1;
            }
            const int numberOrder = lhs.compare(leftSignificant, leftDigits,
                                                rhs, rightSignificant,
                                                rightDigits);
            if (numberOrder != 0) {
                return numberOrder < 0 ? -1 : 1;
            }
            left = leftEnd;
            right = rightEnd;
            continue;
        }

        const char leftLower = static_cast<char>(std::tolower(leftChar));
        const char rightLower = static_cast<char>(std::tolower(rightChar));
        if (leftLower != rightLower) {
            return leftLower < rightLower ? -1 : 1;
        }
        ++left;
        ++right;
    }
    if (left != lhs.size() || right != rhs.size()) {
        return left == lhs.size() ? -1 : 1;
    }
    const std::string leftFolded = lowerCase(lhs);
    const std::string rightFolded = lowerCase(rhs);
    if (leftFolded != rightFolded) {
        return leftFolded < rightFolded ? -1 : 1;
    }
    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

void scanImages() {
    uiImages.clear();
    if (!sdcard::isMounted()) {
        imageError = cardError.empty() ? "SD card is not mounted." : cardError;
        return;
    }

    DIR* directory = opendir(kSdDirectory);
    if (directory == nullptr) {
        imageError = "Could not scan the SD card root directory.";
        Serial.printf("[SD] %s\n", imageError.c_str());
        return;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (!matchesUiPng(entry->d_name)) {
            continue;
        }
        UiImage image = {};
        if (inspectPng(entry->d_name, image)) {
            uiImages.push_back(std::move(image));
        }
    }
    closedir(directory);

    std::sort(uiImages.begin(), uiImages.end(),
              [](const UiImage& lhs, const UiImage& rhs) {
                  return compareNatural(lhs.filename, rhs.filename) < 0;
              });

    if (uiImages.empty()) {
        imageError = "No valid UI*.png images found in the SD card root.";
        Serial.printf("[SD] %s\n", imageError.c_str());
        return;
    }

    Serial.printf("[SD] Found %u image(s):\n",
                  static_cast<unsigned>(uiImages.size()));
    for (const UiImage& image : uiImages) {
        Serial.printf("  %s (%u x %u)\n", image.filename.c_str(),
                      image.width, image.height);
    }
}

void applyBrightness(uint8_t brightness) {
    selectedBrightness = std::clamp<uint8_t>(brightness, 20, 255);
    if (!dimmed && !displaySleeping) {
        setBrightness(selectedBrightness);
    }
}

void noteInteraction() {
    lastInteractionAt = millis();
    if (dimmed) {
        dimmed = false;
        if (!displaySleeping) {
            setBrightness(selectedBrightness);
        }
    }
}

void showSettings(bool visible) {
    settingsOpen = visible;
    lastAutoRotateAt = millis();
    if (currentPage < uiPages.size()) {
        if (visible) {
            lv_obj_remove_flag(uiPages[currentPage].settings, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(uiPages[currentPage].settings, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void loadPage(size_t page) {
    if (uiPages.empty()) {
        return;
    }
    currentPage = page % uiPages.size();
    lastAutoRotateAt = millis();
    lv_scr_load(uiPages[currentPage].screen);
    if (settingsOpen) {
        lv_obj_remove_flag(uiPages[currentPage].settings, LV_OBJ_FLAG_HIDDEN);
    }
}

void navigateNext() {
    if (uiImages.size() < 2 || uiPages.empty()) {
        return;
    }
    loadPage((currentPage + 1) % uiPages.size());
}

void navigatePrevious() {
    if (uiImages.size() < 2 || uiPages.empty()) {
        return;
    }
    loadPage((currentPage + uiPages.size() - 1) % uiPages.size());
}

void enterSleep(const char* reason) {
    if (displaySleeping) {
        return;
    }
    Serial.printf("[POWER] Entering sleep: %s\n", reason);
    if (beepActive) {
        setBuzzer(false);
        beepActive = false;
    }
    lv_obj_remove_flag(sleepOverlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(sleepOverlay);
    lv_refr_now(displayHandle);
    setBrightness(0);
    if (!panelWrite(0x28)) {
        Serial.println("[ERROR] Could not send ST7701 display-off command");
    }
    delay(kPanelCommandDelayMs);
    if (!panelWrite(0x10)) {
        Serial.println("[ERROR] Could not send ST7701 sleep-in command");
    }
    delay(kPanelSleepTransitionDelayMs);
    displaySleeping = true;
    ignoreTouchUntilRelease = true;
    ignoredTouchReleaseAt = 0;
    gestureEligible = false;
    holdEligible = false;
    holdWarningPlayed = false;
    Serial.println("[POWER] LCD in sleep mode; touch the panel to wake");
}

void wakeDisplay() {
    if (!displaySleeping) {
        return;
    }
    if (!panelWrite(0x11)) {
        Serial.println("[ERROR] Could not send ST7701 sleep-out command");
        return;
    }
    delay(kPanelSleepTransitionDelayMs);
    if (!panelWrite(0x29)) {
        Serial.println("[ERROR] Could not send ST7701 display-on command");
        return;
    }
    lv_obj_add_flag(sleepOverlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(sleepOverlay);
    lv_refr_now(displayHandle);
    displaySleeping = false;
    ignoreTouchUntilRelease = true;
    ignoredTouchReleaseAt = 0;
    gestureEligible = false;
    holdEligible = false;
    holdWarningPlayed = false;
    noteInteraction();
    setBrightness(selectedBrightness);
    Serial.println("[POWER] Display awake");
}

void pollTouchAndGestures(uint32_t now) {
    if (!touchAvailable || now - touchPollAt < kTouchPollMs) {
        return;
    }
    touchPollAt = now;

    int16_t x = touchX;
    int16_t y = touchY;
    bool nextTouchDown = touchDown;
    if (!readGt911(x, y, nextTouchDown)) {
        Serial.println("[TOUCH] GT911 read failed");
        gestureEligible = false;
        holdEligible = false;
        if (beepActive) {
            setBuzzer(false);
            beepActive = false;
        }
    } else {
        touchDown = nextTouchDown;
        if (touchDown) {
            touchX = x;
            touchY = y;
        }
    }

    if (ignoreTouchUntilRelease) {
        if (touchDown) {
            ignoredTouchReleaseAt = 0;
        } else {
            if (ignoredTouchReleaseAt == 0) {
                ignoredTouchReleaseAt = now;
            } else if (now - ignoredTouchReleaseAt >=
                       kIgnoredTouchReleaseMs) {
                ignoreTouchUntilRelease = false;
                ignoredTouchReleaseAt = 0;
                gestureEligible = false;
                holdEligible = false;
                holdWarningPlayed = false;
            }
        }
    }

    if (touchDown && !previousTouchDown) {
        if (displaySleeping) {
            wakeDisplay();
            ignoreTouchUntilRelease = true;
        } else {
            noteInteraction();
            gestureStartX = touchX;
            gestureStartY = touchY;
            gestureStartMs = now;
            gestureEligible = true;
            holdEligible = true;
            holdWarningPlayed = false;
            ignoreTouchUntilRelease = false;
        }
    }

    if (touchDown && !ignoreTouchUntilRelease && gestureEligible) {
        const int16_t deltaX = touchX - gestureStartX;
        const int16_t deltaY = touchY - gestureStartY;
        if (std::abs(deltaX) > kHoldMovementLimit ||
            std::abs(deltaY) > kHoldMovementLimit) {
            holdEligible = false;
            if (beepActive) {
                setBuzzer(false);
                beepActive = false;
            }
        }
        if (holdEligible) {
            const uint32_t heldFor = now - gestureStartMs;
            if (!holdWarningPlayed && heldFor >= kLongHoldWarningMs) {
                setBuzzer(true);
                beepActive = true;
                beepOffAt = now + kBeepMs;
                holdWarningPlayed = true;
            }
            if (!settingsOpen && heldFor >= kLongHoldSleepMs) {
                enterSleep("long touch");
            }
        }
    }

    if (!touchDown && previousTouchDown) {
        if (!ignoreTouchUntilRelease && gestureEligible) {
            const int16_t deltaX = touchX - gestureStartX;
            const int16_t deltaY = touchY - gestureStartY;
            const uint32_t duration = now - gestureStartMs;
            const bool isTap =
                duration < kSettingsTapMaxDurationMs &&
                std::abs(deltaX) <= kSettingsTapMaxMovement &&
                std::abs(deltaY) <= kSettingsTapMaxMovement;
            if (isTap && gestureStartY < kSettingsTapAreaHeight) {
                showSettings(!settingsOpen);
                noteInteraction();
            } else if (isTap && !settingsOpen &&
                       gestureStartX < board::kWidth / 4) {
                navigatePrevious();
            } else if (isTap && !settingsOpen &&
                       gestureStartX >= board::kWidth * 3 / 4) {
                navigateNext();
            } else if (!settingsOpen && duration < kLongHoldWarningMs &&
                       std::abs(deltaX) >= kSwipeDistance &&
                       std::abs(deltaY) < 55) {
                if (deltaX < 0) {
                    navigateNext();
                } else {
                    navigatePrevious();
                }
            }
        }
        gestureEligible = false;
        holdEligible = false;
        holdWarningPlayed = false;
    }

    previousTouchDown = touchDown;
}

lv_obj_t* addLabel(lv_obj_t* parent, const char* text, int32_t y,
                   const lv_font_t* font = &lv_font_montserrat_16) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0x18212B), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

void onBrightnessChanged(lv_event_t* event) {
    lv_obj_t* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
    applyBrightness(static_cast<uint8_t>(lv_slider_get_value(slider)));
    noteInteraction();
}

void onAutoRotateDropdownChanged(lv_event_t* event) {
    autoRotateDropdown = static_cast<lv_obj_t*>(lv_event_get_target(event));
    autoRotateSelection = lv_dropdown_get_selected(autoRotateDropdown);
    lastAutoRotateAt = millis();
    noteInteraction();
}

void onAutoSleepDropdownChanged(lv_event_t* event) {
    autoSleepDropdown = static_cast<lv_obj_t*>(lv_event_get_target(event));
    autoSleepSelection = lv_dropdown_get_selected(autoSleepDropdown);
    noteInteraction();
}

void applyBackgroundColor() {
    const BackgroundColor& selected =
        kBackgroundColors[backgroundColorSelection];
    const lv_color_t color = lv_color_hex(selected.rgb);
    for (const UiPage& page : uiPages) {
        lv_obj_set_style_bg_color(page.screen, color, LV_PART_MAIN);
        if (page.background != page.screen) {
            lv_obj_set_style_bg_color(page.background, color, LV_PART_MAIN);
        }
    }
    for (const UiPage& page : uiPages) {
        lv_label_set_text(page.backgroundColorLabel, selected.name);
    }
}

void onPreviousBackgroundColor(lv_event_t*) {
    backgroundColorSelection =
        (backgroundColorSelection +
         sizeof(kBackgroundColors) / sizeof(kBackgroundColors[0]) - 1) %
        (sizeof(kBackgroundColors) / sizeof(kBackgroundColors[0]));
    applyBackgroundColor();
    noteInteraction();
}

void onNextBackgroundColor(lv_event_t*) {
    backgroundColorSelection =
        (backgroundColorSelection + 1) %
        (sizeof(kBackgroundColors) / sizeof(kBackgroundColors[0]));
    applyBackgroundColor();
    noteInteraction();
}

lv_obj_t* createSettingsPanel(lv_obj_t* screen) {
    lv_obj_t* panel = lv_obj_create(screen);
    lv_obj_set_size(panel, 430, 430);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0xF4F7FB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x667788), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 24, LV_PART_MAIN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    addLabel(panel, "Brightness", 30);

    brightnessSlider = lv_slider_create(panel);
    lv_slider_set_range(brightnessSlider, 20, 255);
    lv_slider_set_value(brightnessSlider, selectedBrightness, LV_ANIM_OFF);
    lv_obj_set_width(brightnessSlider, 260);
    lv_obj_align(brightnessSlider, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_add_event_cb(brightnessSlider, onBrightnessChanged,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    addLabel(panel, "Background color", 94);
    lv_obj_t* previousColorButton = lv_button_create(panel);
    lv_obj_set_size(previousColorButton, 48, 40);
    lv_obj_align(previousColorButton, LV_ALIGN_TOP_MID, -112, 116);
    lv_obj_add_event_cb(previousColorButton, onPreviousBackgroundColor,
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_t* previousColorArrow = lv_label_create(previousColorButton);
    lv_label_set_text(previousColorArrow, LV_SYMBOL_LEFT);
    lv_obj_center(previousColorArrow);

    backgroundColorLabel = lv_label_create(panel);
    lv_obj_set_style_text_color(backgroundColorLabel,
                                lv_color_hex(0x18212B), LV_PART_MAIN);
    lv_obj_align(backgroundColorLabel, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_t* nextColorButton = lv_button_create(panel);
    lv_obj_set_size(nextColorButton, 48, 40);
    lv_obj_align(nextColorButton, LV_ALIGN_TOP_MID, 112, 116);
    lv_obj_add_event_cb(nextColorButton, onNextBackgroundColor,
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_t* nextColorArrow = lv_label_create(nextColorButton);
    lv_label_set_text(nextColorArrow, LV_SYMBOL_RIGHT);
    lv_obj_center(nextColorArrow);
    lv_label_set_text(backgroundColorLabel,
                      kBackgroundColors[backgroundColorSelection].name);

    addLabel(panel, "Auto sleep", 166);
    autoSleepDropdown = lv_dropdown_create(panel);
    lv_dropdown_set_options(autoSleepDropdown, "Never\n30 min\n60 min\n120 min");
    lv_dropdown_set_selected(autoSleepDropdown, autoSleepSelection);
    lv_obj_set_width(autoSleepDropdown, 170);
    lv_obj_align(autoSleepDropdown, LV_ALIGN_TOP_MID, 0, 188);
    lv_obj_add_event_cb(autoSleepDropdown, onAutoSleepDropdownChanged,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    addLabel(panel, "Auto rotate", 230);
    autoRotateDropdown = lv_dropdown_create(panel);
    lv_dropdown_set_options(autoRotateDropdown,
                            "Off\n10s\n5s\nHome 15s\nHome 30s\nHome 60s");
    lv_dropdown_set_selected(autoRotateDropdown, autoRotateSelection);
    lv_obj_set_width(autoRotateDropdown, 170);
    lv_obj_align(autoRotateDropdown, LV_ALIGN_TOP_MID, 0, 252);
    lv_obj_add_event_cb(autoRotateDropdown, onAutoRotateDropdownChanged,
                        LV_EVENT_VALUE_CHANGED, nullptr);
    return panel;
}

void createFallback(lv_obj_t* screen, const char* title, const std::string& detail) {
    lv_obj_t* heading = addLabel(screen, title, 145, &lv_font_montserrat_24);
    lv_obj_set_style_text_color(heading, lv_color_hex(0x9C2F28), LV_PART_MAIN);
    lv_obj_t* body = lv_label_create(screen);
    lv_label_set_text(body, detail.c_str());
    lv_obj_set_width(body, 350);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(body, lv_color_hex(0x28343E), LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 12);
    addLabel(screen, "Add UI*.png files to the SD card root", 322);
}

void buildPages() {
    uiPages.clear();
    if (uiImages.empty()) {
        lv_obj_t* screen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
        const bool mounted = sdcard::isMounted();
        createFallback(screen,
                       mounted ? "No display images" : "SD card unavailable",
                       mounted ? imageError : cardError);
        lv_obj_t* settings = createSettingsPanel(screen);
        uiPages.push_back({screen, screen, settings, backgroundColorLabel});
        return;
    }

    for (const UiImage& image : uiImages) {
        lv_obj_t* screen = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t* circularClip = lv_obj_create(screen);
        lv_obj_set_size(circularClip, board::kWidth, board::kHeight);
        lv_obj_center(circularClip);
        lv_obj_set_style_radius(circularClip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(circularClip, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(circularClip, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(circularClip, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(circularClip, 0, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(circularClip, true, LV_PART_MAIN);

        lv_obj_t* imageObject = lv_image_create(circularClip);
        char source[80];
        std::snprintf(source, sizeof(source), "S:%s", image.filename.c_str());
        lv_image_set_src(imageObject, source);
        lv_obj_center(imageObject);

        lv_obj_t* settings = createSettingsPanel(screen);
        uiPages.push_back(
            {screen, circularClip, settings, backgroundColorLabel});
    }
}

void processInactivity(uint32_t now) {
    if (displaySleeping) {
        return;
    }

    static constexpr uint16_t autoActionSeconds[] = {0, 10, 5, 15, 30, 60};
    static constexpr uint16_t autoSleepMinutes[] = {0, 30, 60, 120};
    const uint32_t idleFor = now - lastInteractionAt;
    const uint16_t autoSleepMinutesSelected =
        autoSleepMinutes[autoSleepSelection];
    if (autoSleepMinutesSelected > 0 &&
        idleFor >= static_cast<uint32_t>(autoSleepMinutesSelected) *
                      60U * 1000U) {
        enterSleep("inactivity timeout");
        return;
    }

    const uint32_t autoSleepMs =
        static_cast<uint32_t>(autoSleepMinutesSelected) * 60U * 1000U;
    const uint32_t dimAfterMs =
        autoSleepMs > 0 ? autoSleepMs - kDimBeforeSleepMs
                        : 60U * 60U * 1000U;
    if (idleFor >= dimAfterMs && !dimmed) {
        setBrightness(static_cast<uint8_t>(selectedBrightness / 2U));
        dimmed = true;
        Serial.println(autoSleepMs > 0
                           ? "[POWER] Dimming before scheduled sleep"
                           : "[POWER] Auto-dim active; sleep disabled");
    }

    const bool returnHomeSelected = autoRotateSelection >= 3;
    const uint16_t returnHomeSeconds =
        returnHomeSelected ? autoActionSeconds[autoRotateSelection] : 0;
    if (returnHomeSeconds > 0 && currentPage != 0 &&
        idleFor >= static_cast<uint32_t>(returnHomeSeconds) * 1000U) {
        loadPage(0);
    }

    const uint16_t autoRotateSecondsSelected =
        autoRotateSelection < 3 ? autoActionSeconds[autoRotateSelection] : 0;
    if (!settingsOpen && uiImages.size() > 1 &&
        autoRotateSecondsSelected > 0 &&
        now - lastAutoRotateAt >=
            static_cast<uint32_t>(autoRotateSecondsSelected) * 1000U) {
        loadPage((currentPage + 1) % uiPages.size());
        lastAutoRotateAt = now;
    }

    if (beepActive && static_cast<int32_t>(now - beepOffAt) >= 0) {
        setBuzzer(false);
        beepActive = false;
    }
}

bool initializeTouch() {
    pinMode(board::kTouchInterrupt, OUTPUT);
    digitalWrite(board::kTouchInterrupt, LOW);
    if (!expanderSet(board::kTouchResetBit, false)) {
        return false;
    }
    delay(10);
    if (!expanderSet(board::kTouchResetBit, true)) {
        return false;
    }
    delay(200);

    digitalWrite(board::kTouchInterrupt, HIGH);
    pinMode(board::kTouchInterrupt, INPUT);
    uint8_t status = 0;
    if (!readTouchRegister(board::kTouchStatusRegister, &status, 1)) {
        Serial.println("[ERROR] GT911 not responding at I2C address 0x5D");
        return false;
    }
    Serial.println("[TOUCH] GT911 initialized at I2C address 0x5D");
    return true;
}

[[noreturn]] void failStop(const char* message) {
    Serial.printf("[FATAL] %s\n", message);
    while (true) {
        delay(1000);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("Round Demo Display UI boot");

    const size_t psramBytes = ESP.getPsramSize();
    const size_t freePsramBytes = ESP.getFreePsram();
    Serial.printf("[MEMORY] PSRAM total=%u free=%u bytes\n",
                  static_cast<unsigned>(psramBytes),
                  static_cast<unsigned>(freePsramBytes));
    if (psramBytes < 2U * board::kWidth * board::kHeight * sizeof(uint16_t)) {
        failStop("PSRAM unavailable or too small for two full-frame buffers");
    }

    if (!Wire.begin(board::kI2cSda, board::kI2cScl, 400000)) {
        failStop("I2C initialization failed");
    }
    if (!expanderInit()) {
        failStop("TCA9554 at 0x20 is not responding");
    }
    Serial.println("[BOARD] TCA9554 initialized; SD_MMC D3 enabled");

    if (!initializePanelCommandBus()) {
        failStop("LCD command SPI setup failed");
    }
    if (!initializeSt7701()) {
        failStop("ST7701 initialization failed");
    }
    if (!initializeRgbPanel()) {
        failStop("RGB display/framebuffer initialization failed");
    }
    if (!initializeBacklight()) {
        failStop("Backlight PWM initialization failed");
    }

    initializeLvgl();
    setBrightness(board::kDefaultBrightness);
    runColorDiagnostic();
    Serial.println("[STARTUP] Color diagnostic complete");

    touchAvailable = initializeTouch();
    if (!touchAvailable) {
        Serial.println("[TOUCH] Disabled; display remains available without gestures");
    }

    Serial.println("[STARTUP] Initializing SD card");
    if (!sdcard::mount(cardError)) {
        Serial.printf("[SD] Fallback screen will be used: %s\n",
                      cardError.c_str());
    }
    Serial.println("[STARTUP] SD initialization returned");
    scanImages();
    Serial.printf("[STARTUP] Image scan returned: %u image(s)\n",
                  static_cast<unsigned>(uiImages.size()));
    buildPages();
    Serial.println("[STARTUP] Pages built");
    loadPage(0);
    lv_refr_now(displayHandle);
    Serial.println("[STARTUP] Initial page rendered");

    lv_indev_t* touchInput = lv_indev_create();
    lv_indev_set_type(touchInput, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touchInput, lvglReadTouch);
    lv_indev_set_display(touchInput, displayHandle);

    lastInteractionAt = millis();
    Serial.println("[APP] Ready; swipe horizontally to change screens");
    Serial.println("[APP] Hold still for 4s to beep and 5s to sleep");
    Serial.println("[APP] Auto sleep defaults to 60 minutes; dims 5 minutes before");
    Serial.println("[APP] Swipe down near center to open settings");
}

void loop() {
    const uint32_t now = millis();
    pollTouchAndGestures(now);
    processInactivity(millis());
    const uint32_t nextTimerMs = lv_timer_handler();
    const uint32_t sleepMs = std::clamp<uint32_t>(nextTimerMs, 5, 20);
    delay(sleepMs);
}
