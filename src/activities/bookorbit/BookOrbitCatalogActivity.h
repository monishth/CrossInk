#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>
#include <vector>

#include "CatalogTypes.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Browses a BookOrbit library and downloads books over Wi-Fi.
 *
 * Reached through the network boot path, like the OPDS browser, because Wi-Fi
 * is only up on that route. One page of results is held at a time — the C3 has
 * no room for a whole library, and the API pages for exactly this reason.
 */
class BookOrbitCatalogActivity final : public Activity {
 public:
  static constexpr const char* NAME = "BookOrbitCatalog";

  BookOrbitCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using UiApp = freeink::ui::FreeInkApp<16, 4>;

  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;

  // One page of books at a time; `hasNext` drives the "next page" row.
  bookorbit::CatalogPage page;
  uint32_t pageNumber = 1;
  bool loading = false;
  bool loadRequested = false;
  // A network boot target boots minimally; Wi-Fi is this activity's job.
  bool networkReady = false;
  std::string statusMessage;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  void loadPage();
  void handleSelection();
  void downloadSelected();

  // Rows are books plus an optional trailing "next page" row.
  size_t rowCount() const;
  bool isNextPageRow(size_t index) const;
};
