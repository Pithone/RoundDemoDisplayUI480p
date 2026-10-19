#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include <SD.h>
#include <FS.h>
#include <vector>
#include <algorithm>

// --- HARDWARE CONFIGURATION VIA LOVYANGFX ---
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7701 _panel_instance;
  lgfx::Bus_RGB      _bus_instance;
  lgfx::Light_PWM    _light_instance;
  lgfx::Touch_GT911  _touch_instance;

public:
  LGFX() {
    auto cfg = _bus_instance.config();
    cfg.panel = &_panel_instance;
    cfg.pin_d0 = GPIO_NUM_14; cfg.pin_d1 = GPIO_NUM_38; cfg.pin_d2 = GPIO_NUM_18;
    cfg.pin_d3 = GPIO_NUM_17; cfg.pin_d4 = GPIO_NUM_16; cfg.pin_d5 = GPIO_NUM_15;
    cfg.pin_hstsync = GPIO_NUM_39;
    cfg.pin_hsa     = GPIO_NUM_48;
    cfg.pin_haact   = GPIO_NUM_40;
    cfg.pin_vstsync = GPIO_NUM_41;
    cfg.pin_vsa     = GPIO_NUM_42;
    cfg.pin_vaact   = GPIO_NUM_2;
    cfg.pin_pclk    = GPIO_NUM_21;
    cfg.freq_write  = 14000000;
    _bus_instance.config(cfg);
    _panel_instance.setBus(&_bus_instance);

    auto p_cfg = _panel_instance.config();
    p_cfg.memory_width  = 480;
    p_cfg.memory_height = 480;
    p_cfg.panel_width   = 480;
    p_cfg.panel_height  = 480;
    _panel_instance.config(p_cfg);

    auto l_cfg = _light_instance.config();
    l_cfg.pin_bl = GPIO_NUM_6;
    _light_instance.config(l_cfg);
    _panel_instance.light(&_light_instance);

    auto t_cfg = _touch_instance.config();
    t_cfg.pin_sda = GPIO_NUM_15;
    t_cfg.pin_scl = GPIO_NUM_7;
    t_cfg.i2c_port = I2C_NUM_0;
    t_cfg.i2c_addr = 0x5D;
    t_cfg.x_max = 480; t_cfg.y_max = 480;
    _touch_instance.config(t_cfg);
    _panel_instance.touch(&_touch_instance);

    setPanel(&_panel_instance);
  }
};

LGFX lcd;

static const uint32_t screenWidth = 480;
static const uint32_t screenHeight = 480;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1;
static lv_color_t *buf2;

// Pins & Constants
const int BUZZER_PIN = 4;

// UI State Variables
std::vector<String> pngFiles;
int currentScreenIndex = 0;
std::vector<lv_obj_t*> screens;

// User Preferences & Settings
int currentBrightness = 180;
int dimTimeoutSec = 60;
int returnHomeSec = 30;
bool isDimmed = false;
bool isSleeping = false;
bool hasReturnedHome = false;
unsigned long lastInteractionTime = 0;

// Touch Tracking Variables
unsigned long touchStartTime = 0;
int16_t touchStartX = 0, touchStartY = 0;
int16_t touchCurrentX = 0, touchCurrentY = 0;
bool isTouching = false;
bool warningBeepPlayed = false;

// UI Elements for Settings Modal
lv_obj_t *settingsModal = NULL;
lv_obj_t *sliderBrightness = NULL;
lv_obj_t *ddDimTimeout = NULL;
lv_obj_t *ddReturnTimeout = NULL;

// Forward declarations
void createSettingsMenu();
void toggleSettingsMenu(bool show);

// LVGL Display Flush
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  lcd.startWrite();
  lcd.setAddrWindow(area->x1, area->y1, w, h);
  lcd.writePixels((lgfx::rgb565_t*)&color_p->full, w * h, true);
  lcd.endWrite();
  lv_disp_flush_ready(disp);
}

// LVGL Touch Read & Gesture Handler
void my_touch_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
  uint16_t touchX, touchY;
  bool pressed = lcd.getTouch(&touchX, &touchY);

  if (isSleeping) {
    if (pressed) {
      tone(BUZZER_PIN, 1500, 150);
      isSleeping = false;
      lcd.displayWake();
      lcd.setBrightness(currentBrightness);
      lastInteractionTime = millis();
      delay(300);
    }
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  if (pressed) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = touchX;
    data->point.y = touchY;

    lastInteractionTime = millis();
    hasReturnedHome = false;
    if (isDimmed) {
      isDimmed = false;
      lcd.setBrightness(currentBrightness);
    }

    if (!isTouching) {
      isTouching = true;
      touchStartTime = millis();
      touchStartX = touchX;
      touchStartY = touchY;
      warningBeepPlayed = false;
    }
    touchCurrentX = touchX;
    touchCurrentY = touchY;

    unsigned long holdDuration = millis() - touchStartTime;

    // 5-Second Long Hold for Sleep State
    if (abs(touchCurrentX - touchStartX) < 25 && abs(touchCurrentY - touchStartY) < 25) {
      if (holdDuration >= 4000 && holdDuration < 5000 && !warningBeepPlayed) {
        tone(BUZZER_PIN, 2000, 200);
        warningBeepPlayed = true;
      }
      if (holdDuration >= 5000) {
        tone(BUZZER_PIN, 1000, 400);
        isSleeping = true;
        lcd.setBrightness(0);
        lcd.displaySleep();
        isTouching = false;
        data->state = LV_INDEV_STATE_REL;
        return;
      }
    }

  } else {
    if (isTouching) {
      unsigned long holdDuration = millis() - touchStartTime;
      int16_t deltaX = touchCurrentX - touchStartX;
      int16_t deltaY = touchCurrentY - touchStartY;

      if (holdDuration < 1500) {
        // Swipe Top-to-Bottom in Center -> Open/Close Settings
        if (deltaY > 60 && abs(touchStartX - 240) < 100 && abs(deltaX) < 40) {
          if (settingsModal) {
            bool isHidden = lv_obj_has_flag(settingsModal, LV_OBJ_FLAG_HIDDEN);
            toggleSettingsMenu(isHidden);
          }
        }
        // Horizontal Swipe -> Page Navigation
        else if (abs(deltaX) > 60 && abs(deltaY) < 40) {
          if (deltaX < 0) {
            currentScreenIndex = (currentScreenIndex + 1) % screens.size();
          } else {
            currentScreenIndex = (currentScreenIndex - 1 + screens.size()) % screens.size();
          }
          lv_scr_load(screens[currentScreenIndex]);
        }
      }
      isTouching = false;
    }
    data->state = LV_INDEV_STATE_REL;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);

  lcd.init();
  lcd.setBrightness(currentBrightness);

  if (!SD.begin()) {
    Serial.println("SD Card Mount Failed!");
  }

  lv_init();
  buf1 = (lv_color_t*)heap_caps_malloc(screenWidth * 40 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  buf2 = (lv_color_t*)heap_caps_malloc(screenWidth * 40 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, screenWidth * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_drv_register(&indev_drv);

  File root = SD.open("/");
  if (root) {
    while (File file = root.openNextFile()) {
      if (!file.isDirectory()) {
        String fileName = String(file.name());
        if (fileName.startsWith("UI") && fileName.endsWith(".png")) {
          pngFiles.push_back("S:/" + fileName);
        }
      }
    }
    std::sort(pngFiles.begin(), pngFiles.end());
  }

  if (pngFiles.empty()) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "No UIXX.png files found\non SD Card root!");
    lv_obj_center(label);
    screens.push_back(scr);
  } else {
    for (const auto& path : pngFiles) {
      lv_obj_t *scr = lv_obj_create(NULL);
      lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

      lv_obj_t *img = lv_img_create(scr);
      lv_img_set_src(img, path.c_str());
      lv_obj_center(img);

      screens.push_back(scr);
    }
  }

  if (!screens.empty()) {
    lv_scr_load(screens[0]);
  }

  createSettingsMenu();
  lastInteractionTime = millis();
}

void slider_event_cb(lv_event_t *e) {
  lv_obj_t *slider = lv_event_get_target(e);
  currentBrightness = (int)lv_slider_get_value(slider);
  if (!isDimmed) {
    lcd.setBrightness(currentBrightness);
  }
}

void dropdown_dim_event_cb(lv_event_t *e) {
  lv_obj_t *dd = lv_event_get_target(e);
  char buf[16];
  lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
  if (strcmp(buf, "Off") == 0) dimTimeoutSec = 0;
  else if (strcmp(buf, "30s") == 0) dimTimeoutSec = 30;
  else if (strcmp(buf, "60s") == 0) dimTimeoutSec = 60;
  else if (strcmp(buf, "120s") == 0) dimTimeoutSec = 120;
}

void dropdown_home_event_cb(lv_event_t *e) {
  lv_obj_t *dd = lv_event_get_target(e);
  char buf[16];
  lv_dropdown_get_selected_str(dd, buf, sizeof(buf));
  if (strcmp(buf, "Off") == 0) returnHomeSec = 0;
  else if (strcmp(buf, "15s") == 0) returnHomeSec = 15;
  else if (strcmp(buf, "30s") == 0) returnHomeSec = 30;
  else if (strcmp(buf, "60s") == 0) returnHomeSec = 60;
}

void close_btn_event_cb(lv_event_t *e) {
  toggleSettingsMenu(false);
}

void createSettingsMenu() {
  settingsModal = lv_obj_create(lv_scr_act());
  lv_obj_set_size(settingsModal, 360, 360);
  lv_obj_center(settingsModal);
  lv_obj_set_style_bg_color(settingsModal, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(settingsModal, LV_OPA_90, 0);
  lv_obj_set_style_radius(settingsModal, 180, 0);
  lv_obj_set_flex_flow(settingsModal, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(settingsModal, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *title = lv_label_create(settingsModal);
  lv_label_set_text(title, "DEMO SETTINGS");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);

  lv_obj_t *lblB = lv_label_create(settingsModal);
  lv_label_set_text(lblB, "Brightness");
  lv_obj_set_style_text_color(lblB, lv_color_white(), 0);

  sliderBrightness = lv_slider_create(settingsModal);
  lv_obj_set_width(sliderBrightness, 220);
  lv_slider_set_range(sliderBrightness, 20, 255);
  lv_slider_set_value(sliderBrightness, currentBrightness, LV_ANIM_OFF);
  lv_obj_add_event_cb(sliderBrightness, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *lblDim = lv_label_create(settingsModal);
  lv_label_set_text(lblDim, "Auto-Dim (Half Bright)");
  lv_obj_set_style_text_color(lblDim, lv_color_white(), 0);

  ddDimTimeout = lv_dropdown_create(settingsModal);
  lv_dropdown_set_options(ddDimTimeout, "Off\n30s\n60s\n120s");
  lv_dropdown_set_selected(ddDimTimeout, 2);
  lv_obj_set_width(ddDimTimeout, 160);
  lv_obj_add_event_cb(ddDimTimeout, dropdown_dim_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *lblHome = lv_label_create(settingsModal);
  lv_label_set_text(lblHome, "Auto-Return to UI01");
  lv_obj_set_style_text_color(lblHome, lv_color_white(), 0);

  ddReturnTimeout = lv_dropdown_create(settingsModal);
  lv_dropdown_set_options(ddReturnTimeout, "Off\n15s\n30s\n60s");
  lv_dropdown_set_selected(ddReturnTimeout, 2);
  lv_obj_set_width(ddReturnTimeout, 160);
  lv_obj_add_event_cb(ddReturnTimeout, dropdown_home_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t *closeBtn = lv_btn_create(settingsModal);
  lv_obj_set_size(closeBtn, 100, 35);
  lv_obj_add_event_cb(closeBtn, close_btn_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btnLbl = lv_label_create(closeBtn);
  lv_label_set_text(btnLbl, "Close");
  lv_obj_center(btnLbl);

  lv_obj_add_flag(settingsModal, LV_OBJ_FLAG_HIDDEN);
}

void toggleSettingsMenu(bool show) {
  if (show) {
    lv_obj_clear_flag(settingsModal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(settingsModal);
  } else {
    lv_obj_add_flag(settingsModal, LV_OBJ_FLAG_HIDDEN);
  }
}

void loop() {
  lv_timer_handler();

  if (!isSleeping) {
    unsigned long inactiveSec = (millis() - lastInteractionTime) / 1000;

    if (returnHomeSec > 0 && !hasReturnedHome && inactiveSec >= (unsigned long)returnHomeSec && currentScreenIndex != 0) {
      if (settingsModal && lv_obj_has_flag(settingsModal, LV_OBJ_FLAG_HIDDEN)) {
        currentScreenIndex = 0;
        lv_scr_load(screens[currentScreenIndex]);
        hasReturnedHome = true;
      }
    }

    if (dimTimeoutSec > 0 && !isDimmed && inactiveSec >= (unsigned long)dimTimeoutSec) {
      isDimmed = true;
      lcd.setBrightness(currentBrightness / 2);
    }
  }

  delay(5);
}
