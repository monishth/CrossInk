#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for BookOrbit sync settings: server URL, credentials, the root
 * certificate, an enable toggle, and a connection test.
 *
 * Mirrors KOReaderSettingsActivity's structure — FreeInkApp hosts the list,
 * GUI.drawHeader keeps the battery indicator, physical buttons are handled in
 * loop() while touch routes through the app.
 */
class BookOrbitSettingsActivity final : public Activity {
 public:
  explicit BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using UiApp = freeink::ui::FreeInkApp<12, 4>;

  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;
  bool ignoreInitialConfirmRelease = false;

  // Result of the last connection test, shown against the test row. Owned here
  // so the row builder can point at a stable string.
  std::string statusMessage;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  void handleSelection();
  // Reads /.crosspoint/bookorbit_ca.pem into the store. Returns false (and
  // sets statusMessage) when there is no usable file, so the caller can fall
  // back to keyboard entry.
  bool importRootCaFromSd();

  void runConnectionTest();
};
