#include "BootActivity.h"

#include <GfxRenderer.h>

#include "images/BootSplash.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  // BootSplash is pre-rotated 90 degrees by the asset pipeline; drawIcon
  // (not drawImage) is required to reproduce Portrait orientation correctly
  // for a non-square full-screen bitmap.
  renderer.drawIcon(BootSplash, 0, 0, pageWidth, pageHeight);
  renderer.displayBuffer();
}
