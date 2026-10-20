# Refined prompt: Waveshare ESP32-S3 round display demo

Act as a senior embedded-systems engineer specializing in ESP32-S3,
PlatformIO, Arduino, LVGL, RGB displays, and hardware bring-up.

Create a **new, complete PlatformIO project** for a work demo on the **Waveshare
ESP32-S3-Touch-LCD-2.8C**: ESP32-S3, 480 × 480 round IPS display with ST7701
controller, GT911 capacitive touch, microSD slot, PWM backlight, and onboard
buzzer. Do not modify or depend on an existing project; start from a clean
project structure.

## Deliverables

Provide a complete, buildable codebase containing at least:

- `platformio.ini`
- `src/lv_conf.h`
- `src/main.cpp`
- `README.md`

Add only necessary source/configuration files. Include the selected PlatformIO
environment, dependency versions, flash/partition configuration, and the
commands to build, upload, and monitor serial output.

## First: verify the actual board hardware

Before writing application code, confirm the board's actual pin mapping,
display timing, ST7701 initialization sequence, backlight polarity/control,
buzzer connection, touch wiring, and SD interface from the board schematic,
Waveshare documentation, or a known-good vendor example.

- Do not assume the SD card uses `SD_MMC`: this specification requires **SPI
  mode**, so select and configure the correct SPI host and chip-select pin.
- Do not assume buzzer GPIO, expander address, expander pin numbering, RGB
  pinout, or display timing based on a similar Waveshare board.
- If authoritative sources conflict or a hardware detail cannot be confirmed,
  call it out explicitly and isolate it in one clearly named configuration
  section. Do not silently invent pin assignments.
- Confirm the PlatformIO board configuration enables the module's external
  PSRAM and matches its flash size and memory mode.

## 1. Display, touch, storage, and graphics initialization

- Initialize the ST7701 RGB panel using **one display-driver owner**. Do not
  configure the same RGB peripheral independently through multiple drivers.
- Initialize GT911 touch, SPI microSD, PWM backlight, and the buzzer according
  to the verified board wiring.
- Configure LVGL v8 or v9 consistently with the selected decoder and project
  dependencies. Enable native PNG decoding (`LV_USE_PNG`) and allocate two
  LVGL drawing buffers in external PSRAM using the appropriate capability
  allocator.
- Use RGB565 consistently between LVGL, render buffers, and the panel. Do not
  byte-swap unless a physical color test proves it is required.
- The prior board bring-up found that ST7701 register `0x3A = 0x66` restored
  red in a solid-color test, and `0x36 = 0x00` gave better reported colors
  than BGR order. Treat these as known-good starting values, not universal
  truths: retain them unless the actual panel's verified init sequence or a
  repeatable on-device test demonstrates otherwise.
- Include explicit initialization and allocation error handling with useful
  serial diagnostics. Do not show success-shaped fallback behavior if an
  essential display or framebuffer setup failed.

## 2. Tear-free screen updates

The previous implementation displayed images but tore. A later attempt to
layer a second ESP-IDF RGB driver on top of LovyanGFX caused a black screen;
a subsequent replacement attempt produced a solid-white screen. Neither
proved a working page flip.

Implement a synchronization strategy for the **single selected RGB driver**:

- Use VSYNC-boundary double-buffer page flipping or another documented,
  driver-supported synchronization method.
- Ensure LVGL renders only to a buffer that is not currently being scanned out.
- Switch the displayed framebuffer only at a safe frame boundary and observe
  the driver's buffer lifetime/cache synchronization requirements.
- Two LVGL draw buffers by themselves do not constitute two scanout buffers.
- Keep a known-working display initialization path recoverable until the new
  synchronization strategy has been physically checked.
- Do not claim tear-free operation based only on a successful build, upload, or
  serial log. State clearly what still needs visual hardware verification.

## 3. Dynamic SD-card PNG loader

- Mount the microSD card in **SPI mode** using the verified SPI host, pins, and
  chip-select.
- Scan the root directory at boot for case-insensitive filenames matching
  `UI*.png` (for example, `UI01.png`, `UI02.png`).
- Sort deterministically in natural/numeric order so `UI2.png` precedes
  `UI10.png`; keep ordering stable for equal numeric portions.
- Create one LVGL page per image, centered on a solid white background.
- Keep the content within the 480 × 480 display area. Use the round panel's
  visible area appropriately and avoid assuming LVGL clips to the circle
  automatically.
- If mounting or scanning fails, or no matching PNG exists, show a useful
  on-screen fallback and print the specific cause to serial.
- Consider decoded image memory, not just compressed PNG file size. Verify
  image dimensions, color format, and decoder limits before drawing.

## 4. Gesture navigation

- Horizontal left/right swipes navigate between detected UI pages in a
  circular loop.
- Use debouncing and gesture thresholds that avoid accidental navigation from
  stationary holds or the settings gesture.
- Handle zero or one page safely.

## 5. Long-hold sleep and wake

- A stationary touch held anywhere continuously for 5 seconds puts the display
  into sleep by turning off the backlight and using the supported panel/touch
  low-power behavior.
- At 4.0 seconds, sound one short warning beep.
- A touch while asleep wakes the display and restores the saved brightness.
- Avoid repeated warning beeps during the same hold. Reset the hold state on
  release or meaningful movement.
- Use non-blocking timing so LVGL, touch, and buzzer state remain responsive.

## 6. Settings gesture and menu

A top-to-bottom vertical swipe near the horizontal center opens or closes a
round, semi-transparent settings modal containing:

- Brightness slider, range 20–255, connected to the verified PWM backlight.
- Auto-dim dropdown: Off, 30s, 60s, 120s. After inactivity, set brightness to
  half the selected brightness; restore the selected brightness on
  interaction.
- Auto-return dropdown: Off, 15s, 30s, 60s. After inactivity, return to the
  first page (`UI01` according to the resolved sort order).
- Close button.

## 7. Code quality and staged verification

- Keep board pins/timings in one maintainable configuration section.
- Keep hardware setup, LVGL setup, file scanning, UI construction, gesture
  handling, and inactivity logic clearly separated.
- Add comments for non-obvious hardware or synchronization behavior. Prefer
  clear concise comments over comments that merely narrate each line.
- Use fixed-width types, bounds checks, allocation checks, and explicit error
  messages. Avoid unchecked division/modulo or empty-list navigation.
- Build in stages:
  1. Board/serial/PSRAM bring-up.
  2. Display initialization and full-screen red, green, blue, white, and black
     diagnostics using the intended final driver.
  3. Verified display timing/color and a synchronized full-frame update.
  4. SPI SD mount and PNG decode.
  5. Touch gestures and settings.
- After each stage, report the exact build command/result and any physical
  checks needed. Do not flash a non-building image.

## Acceptance criteria

1. PlatformIO can inspect and build the complete project for the intended
   Waveshare environment.
2. Serial output reports actionable success/failure for PSRAM, expander,
   display, touch, SD, image scan, and buffer allocation.
3. The solid-color diagnostic displays all five expected colors with no
   missing channel or obvious color-order error.
4. The card mounts in SPI mode; matching PNGs load in the expected order, and
   missing-card/no-image states are understandable.
5. Swipe navigation, the 4-second warning, 5-second sleep, wake, brightness,
   dimming, home return, and settings modal behave as specified.
6. Repeated full-screen image transitions are visually checked on the
   physical board and show no visible tearing. Clearly mark this criterion
   unverified until that check has occurred.
