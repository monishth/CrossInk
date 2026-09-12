#include "BookOrbitCatalogActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>

#include "BookOrbitCapabilities.h"
#include "BookOrbitClient.h"
#include "CatalogApi.h"
#include "CatalogBookDetail.h"
#include "CatalogDownload.h"
#include "CatalogQuery.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "network/BookOrbitCredentialStore.h"
#include "network/BookOrbitFileSink.h"
#include "network/WifiUtils.h"

#ifdef SIMULATOR
#include "network/SimulatorHttpTransport.h"
#include "network/SimulatorStreamDownloader.h"
using BookOrbitTransport = SimulatorHttpTransport;
using BookOrbitDownloader = SimulatorStreamDownloader;
#else
#include "network/BookOrbitHttpTransport.h"
#include "network/BookOrbitStreamDownloader.h"
using BookOrbitTransport = BookOrbitHttpTransport;
using BookOrbitDownloader = BookOrbitStreamDownloader;
#endif

namespace fui = freeink::ui;

namespace {
constexpr char kModule[] = "BORB";
constexpr fui::ActionId ACTION_ROW = 1;
constexpr uint32_t kPageSize = 20;

// Books land beside everything else the user already browses.
constexpr char kDownloadDir[] = "/Books";

// Catalog titles are arbitrary text, but the card is FAT: "/" would silently
// redirect the write into a subdirectory, and \ : * ? " < > | are rejected
// outright. Trailing dots and spaces are also invalid. Map anything unusable to
// "-" so a book is never undownloadable because of its title.
std::string sanitizeFileName(const std::string& title) {
  static constexpr char kIllegal[] = "\\/:*?\"<>|";
  std::string out;
  out.reserve(title.size());
  for (const char c : title) {
    const auto unsignedChar = static_cast<unsigned char>(c);
    if (unsignedChar < 0x20 || std::strchr(kIllegal, c) != nullptr) {
      out.push_back('-');
    } else {
      out.push_back(c);
    }
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  // Keep well inside FAT's 255-character limit once the extension is appended.
  if (out.size() > 120) out.resize(120);
  return out.empty() ? std::string("book") : out;
}
}  // namespace

BookOrbitCatalogActivity::BookOrbitCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity(NAME, renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

size_t BookOrbitCatalogActivity::rowCount() const { return page.items.size() + (page.hasNext ? 1 : 0); }

bool BookOrbitCatalogActivity::isNextPageRow(const size_t index) const {
  return page.hasNext && index == page.items.size();
}

void BookOrbitCatalogActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitCatalogActivity*>(user);
  if (event.value < 0 || static_cast<size_t>(event.value) >= self->rowCount()) return;
  self->selectedIndex = static_cast<size_t>(event.value);
  self->app.clearTapFlash();
  self->handleSelection();
}

void BookOrbitCatalogActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  pageNumber = 1;
  loading = false;
  loadRequested = false;
  networkReady = false;
  statusMessage.clear();
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &BookOrbitCatalogActivity::onRowEvent, this);
  app.setScreen(&BookOrbitCatalogActivity::listScreen, this);
  BOOKORBIT_STORE.ensureLoaded();
  sdFontSystem.releaseLoadedFont(renderer);

  // A network boot target boots minimally so the activity has heap to bring
  // Wi-Fi up; it does not connect for us. Requests issued before that panic
  // inside the network stack on a null semaphore.
  if (hasActiveStationWifiConnection()) {
    networkReady = true;
    loadRequested = true;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             statusMessage = tr(STR_WIFI_CONN_FAILED);
                           } else {
                             WiFi.setSleep(false);
                             networkReady = true;
                             loadRequested = true;
                             statusMessage = tr(STR_LOADING);
                           }
                           requestUpdate();
                         });
}

void BookOrbitCatalogActivity::onExit() {
  page.items.clear();
  statusMessage.clear();
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // Entered from a minimal network boot; hand the full app back.
  silentRestartAfterNetwork();
}

void BookOrbitCatalogActivity::loadPage() {
  BookOrbitTransport transport(BOOKORBIT_STORE.getRootCaPem());
  bookorbit::DeviceIdentity identity;
  identity.deviceId = BOOKORBIT_STORE.getDeviceId();
  identity.deviceModel = CROSSINK_FIRMWARE_DEVICE_TYPE;
  identity.pluginVersion = CROSSINK_VERSION;

  bookorbit::BookOrbitClient client(transport, BOOKORBIT_STORE.getServerUrl(), BOOKORBIT_STORE.getUsername(),
                                    BOOKORBIT_STORE.getMd5Password(), identity);
  bookorbit::CapabilityCache capabilities;
  bookorbit::CatalogApi api(client, capabilities);

  bookorbit::CatalogQuery query;
  query.set("page", static_cast<long>(pageNumber));
  query.set("size", static_cast<long>(kPageSize));

  bookorbit::CatalogPage fetched;
  const bookorbit::Error error = api.books(query, fetched);
  if (error.status != bookorbit::Status::Ok) {
    LOG_ERR(kModule, "catalog page %u failed (%d)", static_cast<unsigned>(pageNumber), static_cast<int>(error.status));
    statusMessage = tr(STR_BOOKORBIT_CATALOG_UNAVAILABLE);
    return;
  }

  page = std::move(fetched);
  statusMessage = page.items.empty() ? tr(STR_BOOKORBIT_EMPTY_SECTION) : "";
  selectedIndex = 0;
  topIndex = 0;
}

void BookOrbitCatalogActivity::downloadSelected() {
  if (selectedIndex >= page.items.size()) return;
  const auto& book = page.items[selectedIndex];

  // The books *list* response carries no file id — only `formats`, a set of
  // extension strings (koreader-catalog.service.ts:676-687). The id lives in
  // the book detail, so fetch that first. Skipping this step is why selecting a
  // book used to fail before any request was made.
  BookOrbitTransport transport(BOOKORBIT_STORE.getRootCaPem());
  bookorbit::DeviceIdentity identity;
  identity.deviceId = BOOKORBIT_STORE.getDeviceId();
  identity.deviceModel = CROSSINK_FIRMWARE_DEVICE_TYPE;
  identity.pluginVersion = CROSSINK_VERSION;

  bookorbit::BookOrbitClient client(transport, BOOKORBIT_STORE.getServerUrl(), BOOKORBIT_STORE.getUsername(),
                                    BOOKORBIT_STORE.getMd5Password(), identity);

  std::string detailBody;
  const bookorbit::Error detailError = client.get(bookorbit::bookDetailPath(book.bookId), detailBody);
  if (detailError.status != bookorbit::Status::Ok) {
    LOG_ERR(kModule, "book detail %u failed (status %d http %d)", static_cast<unsigned>(book.bookId),
            static_cast<int>(detailError.status), detailError.httpStatus);
    statusMessage = tr(STR_BOOKORBIT_DOWNLOAD_FAILED);
    return;
  }

  std::vector<bookorbit::CatalogFile> files;
  if (!bookorbit::decodeBookDetailFiles(detailBody, files)) {
    LOG_ERR(kModule, "book detail %u was not valid JSON", static_cast<unsigned>(book.bookId));
    statusMessage = tr(STR_BOOKORBIT_DOWNLOAD_FAILED);
    return;
  }

  const bookorbit::CatalogFile* file = bookorbit::pickDownloadableFile(files);
  if (file == nullptr) {
    LOG_ERR(kModule, "book %u has no downloadable file", static_cast<unsigned>(book.bookId));
    statusMessage = tr(STR_BOOKORBIT_DOWNLOAD_FAILED);
    return;
  }

  std::string target = std::string(kDownloadDir) + "/" + sanitizeFileName(book.title);
  target += "." + (file->format.empty() ? std::string("epub") : file->format);

  BookOrbitFileSink sink;
  // The writer's third argument is a hard cap, not the expected size: passing
  // sizeBytes verbatim aborts the transfer the instant it reaches the catalog's
  // figure to the byte. maxBytesForExpected() is what turns an expectation into
  // a cap, allowing 1.25x plus a megabyte of slack for a stale or approximate
  // size while still refusing a runaway download.
  bookorbit::PartFileWriter writer(sink, target, bookorbit::maxBytesForExpected(file->sizeBytes));
  if (!writer.begin()) {
    // Every failure path logs: a silent "Download failed" is impossible to
    // diagnose from the screen alone.
    LOG_ERR(kModule, "cannot open %s for writing", target.c_str());
    statusMessage = tr(STR_BOOKORBIT_DOWNLOAD_FAILED);
    return;
  }

  BookOrbitDownloader downloader(BOOKORBIT_STORE.getRootCaPem().c_str(), BOOKORBIT_STORE.getUsername(),
                                 BOOKORBIT_STORE.getMd5Password());
  const std::string url = BOOKORBIT_STORE.getServerUrl() + bookorbit::fileDownloadPath(file->fileId);

  statusMessage = tr(STR_BOOKORBIT_DOWNLOADING);
  requestUpdate();

  const bookorbit::Error error = downloader.downloadTo(url, writer, file->sizeBytes, nullptr, nullptr);
  if (error.status != bookorbit::Status::Ok || !writer.commit()) {
    LOG_ERR(kModule, "download of file %u failed (status %d http %d)", static_cast<unsigned>(file->fileId),
            static_cast<int>(error.status), error.httpStatus);
    // abandon() closes the .part and leaves it where it is. The important part
    // is that the final path is never published, so a failed download cannot
    // leave a truncated EPUB in the library; the stray .part is harmless and is
    // truncated by the next attempt's begin().
    writer.abandon();
    statusMessage = tr(STR_BOOKORBIT_DOWNLOAD_FAILED);
    return;
  }

  statusMessage = tr(STR_BOOKORBIT_ON_DEVICE);
}

void BookOrbitCatalogActivity::handleSelection() {
  if (isNextPageRow(selectedIndex)) {
    pageNumber++;
    loadRequested = true;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    return;
  }
  downloadSelected();
  requestUpdate();
}

void BookOrbitCatalogActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  // Deferred fetch: the screen has painted "Loading" by the time this runs.
  if (loadRequested && networkReady) {
    loadRequested = false;
    loading = true;
    loadPage();
    loading = false;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  const size_t rows = rowCount();
  if (rows == 0) return;

  buttonNavigator.onNext([this, rows] {
    selectedIndex = (selectedIndex + 1) % rows;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, static_cast<int>(rows));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, rows] {
    selectedIndex = (selectedIndex + rows - 1) % rows;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, static_cast<int>(rows));
    requestUpdate();
  });
}

void BookOrbitCatalogActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<BookOrbitCatalogActivity*>(user)->buildListScreen(screen);
}

void BookOrbitCatalogActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const size_t rows = rowCount();
  std::vector<fui::ListItem> items;
  items.reserve(rows);

  // Row strings are owned for the draw only; ListItem holds pointers.
  std::vector<std::string> labels(rows);
  std::vector<std::string> values(rows);

  for (size_t i = 0; i < page.items.size(); i++) {
    labels[i] = page.items[i].title.empty() ? tr(STR_BOOKORBIT_UNTITLED) : page.items[i].title;
    values[i] = page.items[i].authors;
  }
  if (page.hasNext) {
    labels[page.items.size()] = tr(STR_BOOKORBIT_NEXT_PAGE);
  }

  for (size_t i = 0; i < rows; i++) {
    fui::ListItem item;
    item.label = labels[i].c_str();
    if (!values[i].empty()) item.value = values[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  const auto visible = configureUiList(props, screen.theme(), screen.body());
  visibleRows = visible > 0 ? visible : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(rows));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookOrbitCatalogActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_BOOKORBIT_CATALOG), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BOOKORBIT_CATALOG));
  }

  if (!statusMessage.empty()) {
    GUI.drawHelpText(renderer, Rect{0, static_cast<int>(pageHeight - metrics.buttonHintsHeight - 24), pageWidth, 20},
                     statusMessage.c_str());
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
