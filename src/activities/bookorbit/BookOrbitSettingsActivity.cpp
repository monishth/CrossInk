#include "BookOrbitSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BookOrbitClient.h"
#include "BookOrbitError.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "network/BookOrbitCredentialStore.h"
#include "util/InputReleaseGuard.h"

#ifndef SIMULATOR
#include "network/BookOrbitHttpTransport.h"
#endif

namespace fui = freeink::ui;

namespace {
constexpr int MENU_ITEMS = 8;

// Row order. Kept as an enum so handleSelection() reads as intent rather than
// as index arithmetic.
enum Row : int {
  ROW_SERVER_URL = 0,
  ROW_USERNAME,
  ROW_PASSWORD,
  ROW_ROOT_CA,
  ROW_ENABLED,
  ROW_TEST,
  ROW_SYNC_NOW,
  ROW_BROWSE,
};

const StrId menuNames[MENU_ITEMS] = {
    StrId::STR_BOOKORBIT_SERVER_URL, StrId::STR_BOOKORBIT_USERNAME, StrId::STR_BOOKORBIT_PASSWORD,
    StrId::STR_BOOKORBIT_ROOT_CA,    StrId::STR_BOOKORBIT_ENABLED,  StrId::STR_BOOKORBIT_TEST_CONNECTION,
    StrId::STR_BOOKORBIT_SYNC_NOW,   StrId::STR_BOOKORBIT_BROWSE,
};

constexpr fui::ActionId ACTION_ROW = 1;

constexpr char kAuthPath[] = "/koreader/users/auth";
constexpr char kVersionPath[] = "/koreader/plugin/version";
}  // namespace

BookOrbitSettingsActivity::BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BookOrbitSettings", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void BookOrbitSettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitSettingsActivity*>(user);
  if (event.value < 0 || event.value >= MENU_ITEMS) return;
  self->selectedIndex = static_cast<size_t>(event.value);
  self->app.clearTapFlash();
  self->handleSelection();
}

void BookOrbitSettingsActivity::onEnter() {
  Activity::onEnter();

  ignoreInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  selectedIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  statusMessage.clear();
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &BookOrbitSettingsActivity::onRowEvent, this);
  app.setScreen(&BookOrbitSettingsActivity::listScreen, this);
  BOOKORBIT_STORE.ensureLoaded();
  requestUpdate();
}

void BookOrbitSettingsActivity::onExit() {
  statusMessage.clear();
  Activity::onExit();
}

void BookOrbitSettingsActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               ignoreInitialConfirmRelease)) {
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
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

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });
}

void BookOrbitSettingsActivity::handleSelection() {
  switch (static_cast<Row>(selectedIndex)) {
    case ROW_SERVER_URL: {
      const std::string current = BOOKORBIT_STORE.getServerUrl();
      const std::string prefill = current.empty() ? "https://" : current;
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(
                                 renderer, mappedInput, tr(STR_BOOKORBIT_SERVER_URL), prefill, 128, InputType::Url),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) return;
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string url = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               BOOKORBIT_STORE.setServerUrl(url);
                               BOOKORBIT_STORE.saveToFile();
                               statusMessage.clear();
                             });
      break;
    }
    case ROW_USERNAME:
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_USERNAME),
                                                  BOOKORBIT_STORE.getUsername(), 64, InputType::Text),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            const auto& kb = std::get<KeyboardResult>(result.data);
            BOOKORBIT_STORE.setCredentials(kb.text, BOOKORBIT_STORE.getPassword());
            BOOKORBIT_STORE.saveToFile();
            statusMessage.clear();
          });
      break;
    case ROW_PASSWORD:
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_PASSWORD),
                                                  BOOKORBIT_STORE.getPassword(), 64, InputType::Text),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            const auto& kb = std::get<KeyboardResult>(result.data);
            BOOKORBIT_STORE.setCredentials(BOOKORBIT_STORE.getUsername(), kb.text);
            BOOKORBIT_STORE.saveToFile();
            statusMessage.clear();
          });
      break;
    case ROW_ROOT_CA:
      // A PEM is long, but typing it is the only input this device has, and the
      // certificate is what makes the connection verifiable at all — so the row
      // is editable rather than read-only. The keyboard cap is generous enough
      // for a single root certificate.
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_ROOT_CA),
                                                  BOOKORBIT_STORE.getRootCaPem(), 2048, InputType::Text),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            const auto& kb = std::get<KeyboardResult>(result.data);
            BOOKORBIT_STORE.setRootCaPem(kb.text);
            BOOKORBIT_STORE.saveToFile();
            statusMessage.clear();
          });
      break;
    case ROW_ENABLED:
      BOOKORBIT_STORE.setEnabled(!BOOKORBIT_STORE.isEnabled());
      BOOKORBIT_STORE.saveToFile();
      requestUpdate();
      break;
    case ROW_TEST:
      runConnectionTest();
      requestUpdate();
      break;
    case ROW_SYNC_NOW:
      // Wi-Fi is only brought up on the network boot path, so syncing means a
      // silent restart into it — the same route KOReader sync takes.
      if (!BOOKORBIT_STORE.hasCredentials() || BOOKORBIT_STORE.getRootCaPem().empty()) {
        statusMessage = tr(STR_BOOKORBIT_NO_CERT);
        requestUpdate();
        break;
      }
      if (APP_STATE.openEpubPath.empty()) {
        statusMessage = tr(STR_BOOKORBIT_NO_BOOK);
        requestUpdate();
        break;
      }
      silentRestartToNetwork(NetworkBootTarget::BOOKORBIT_SYNC);
      break;
    case ROW_BROWSE:
      if (!BOOKORBIT_STORE.hasCredentials() || BOOKORBIT_STORE.getRootCaPem().empty()) {
        statusMessage = tr(STR_BOOKORBIT_NO_CERT);
        requestUpdate();
        break;
      }
      silentRestartToNetwork(NetworkBootTarget::BOOKORBIT_CATALOG);
      break;
  }
}

void BookOrbitSettingsActivity::runConnectionTest() {
  if (!BOOKORBIT_STORE.hasCredentials()) {
    statusMessage = tr(STR_SET_CREDENTIALS_FIRST);
    return;
  }
  // Refusing early keeps the failure legible: without a PEM the transport
  // would decline the request anyway, and "cannot reach server" would be a
  // misleading way to say "you have not installed a certificate".
  if (BOOKORBIT_STORE.getRootCaPem().empty()) {
    statusMessage = tr(STR_BOOKORBIT_NO_CERT);
    return;
  }

#ifdef SIMULATOR
  // The simulator's SecureHttpClient stub cannot perform this request; the
  // network path is exercised by the native suite and on hardware.
  statusMessage = tr(STR_BOOKORBIT_UNREACHABLE);
#else
  BookOrbitHttpTransport transport(BOOKORBIT_STORE.getRootCaPem());

  bookorbit::DeviceIdentity identity;
  identity.deviceId = BOOKORBIT_STORE.getDeviceId();
  identity.deviceModel = CROSSINK_FIRMWARE_DEVICE_TYPE;
  identity.pluginVersion = CROSSINK_VERSION;

  bookorbit::BookOrbitClient client(transport, BOOKORBIT_STORE.getServerUrl(), BOOKORBIT_STORE.getUsername(),
                                    BOOKORBIT_STORE.getMd5Password(), identity);

  std::string body;
  const bookorbit::Error error = client.get(kAuthPath, body);
  switch (error.status) {
    case bookorbit::Status::Ok:
      break;
    case bookorbit::Status::Unauthorized:
      statusMessage = tr(STR_BOOKORBIT_AUTH_FAILED);
      return;
    case bookorbit::Status::Transport:
      // A rejected certificate surfaces as a transport failure, because the
      // handshake never completes. Both readings are worth offering, but the
      // certificate is the likelier cause once a PEM is configured.
      statusMessage = tr(STR_BOOKORBIT_CERT_INVALID);
      return;
    default:
      statusMessage = tr(STR_BOOKORBIT_UNREACHABLE);
      return;
  }

  // Auth succeeded; seed the capability cache so the first real sync does not
  // have to negotiate from scratch.
  std::string versionBody;
  client.get(kVersionPath, versionBody);
  statusMessage = tr(STR_BOOKORBIT_CONNECTED);
#endif
}

void BookOrbitSettingsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<BookOrbitSettingsActivity*>(user)->buildListScreen(screen);
}

void BookOrbitSettingsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Per-render owned value strings; items point into them for the draw only.
  std::vector<std::string> values(MENU_ITEMS);
  values[ROW_SERVER_URL] = BOOKORBIT_STORE.getServerUrl().empty() ? tr(STR_NOT_SET) : BOOKORBIT_STORE.getServerUrl();
  values[ROW_USERNAME] = BOOKORBIT_STORE.getUsername().empty() ? tr(STR_NOT_SET) : BOOKORBIT_STORE.getUsername();
  values[ROW_PASSWORD] = BOOKORBIT_STORE.getPassword().empty() ? tr(STR_NOT_SET) : "******";
  values[ROW_ROOT_CA] = BOOKORBIT_STORE.getRootCaPem().empty() ? tr(STR_NOT_SET) : tr(STR_BOOKORBIT_ROOT_CA_SET);
  values[ROW_TEST] = statusMessage;

  std::vector<fui::ListItem> items;
  items.reserve(MENU_ITEMS);
  for (int i = 0; i < MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuNames[i]);
    if (!values[i].empty()) item.value = values[i].c_str();
    item.toggle = i == ROW_ENABLED;
    item.toggleChecked = BOOKORBIT_STORE.isEnabled();
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
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, MENU_ITEMS);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookOrbitSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_BOOKORBIT_TITLE), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BOOKORBIT_TITLE));
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
