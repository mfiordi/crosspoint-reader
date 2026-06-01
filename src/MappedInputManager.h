#pragma once

#include <HalGPIO.h>

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down, Power, PageBack, PageForward };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  explicit MappedInputManager(HalGPIO& gpio) : gpio(gpio) {}

  void update() const { gpio.update(); }
  bool wasPressed(Button button) const;
  bool wasReleased(Button button) const;
  bool isPressed(Button button) const;
  bool wasAnyPressed() const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const;
  // Returns the raw front button index that was pressed this frame (or -1 if none).
  int getPressedFrontButton() const;

  // When enabled, a power-button press/release is treated as a Confirm edge
  // (so a short power tap acts as "Select" on menu screens). Set per loop by main.
  void setPowerAsConfirm(bool v) { powerAsConfirm = v; }

 private:
  HalGPIO& gpio;
  bool powerAsConfirm = false;

  bool mapButton(Button button, bool (HalGPIO::*fn)(uint8_t) const) const;
};
