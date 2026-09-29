#pragma once

#include <string>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session. The live frontlight
// state rides along in the same flag so the reboot is invisible: the light
// comes back exactly as it was, regardless of the Restore Light on Wake
// preference.

void silentRestart();            // home screen
void silentRestartToReader();    // currently-open EPUB (APP_STATE.openEpubPath)
void silentRestartToSettings();  // settings screen
// Article module, id auto-opened and read once the list resyncs
// (APP_STATE.pendingArticleReadId, set here before rebooting). SLO-15: an
// article read always follows a prior WiFi sync/list load, so — like the
// other WiFi-adjacent flows above — the heap is reliably fragmented by the
// time "Read" is pressed; rebooting first makes the subsequent fetch and
// (styled or plain-text) layout run on a clean heap instead of racing
// whatever's left over.
void silentRestartToArticleRead(const std::string& articleId);

// Reboots immediately after an activity releases exclusive raw storage. The
// RTC target ensures setup() lands on Home instead of resuming a reader.
void restartToHomeAfterStorageHandoff();
