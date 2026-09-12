#pragma once

#include <memory>
#include <string>

#include "BookOrbitOutbox.h"
#include "activities/Activity.h"

/**
 * Runs one book's BookOrbit sync and reports the outcome.
 *
 * Entered after a silent restart into the network boot target, the same way
 * KOReaderSyncActivity is, because Wi-Fi is only brought up on that path.
 *
 * The phase chain (match -> stats -> progress -> state -> annotations ->
 * bookmarks) is stepped one phase per loop() tick rather than run in a single
 * blocking call, so the render task keeps servicing the screen and the user can
 * still press Back.
 */
class Epub;

class BookOrbitSyncActivity final : public Activity {
 public:
  static constexpr const char* NAME = "BookOrbitSync";

  // ConnectionTest authenticates and negotiates capabilities without touching a
  // book. It shares this activity because it needs the same thing Sync does:
  // Wi-Fi, which only exists after a silent restart into the network boot path.
  enum class Mode { Sync, ConnectionTest };

  BookOrbitSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                        Mode mode = Mode::Sync);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  Mode mode = Mode::Sync;
  std::string epubPath;
  std::string bookHash;

  bookorbit::SyncOutbox outbox;
  bool finished = false;
  // A network boot target boots minimally; Wi-Fi is this activity's job.
  bool networkReady = false;
  bool started = false;
  bool matched = false;          // the server acknowledged this hash
  bool degradedLanding = false;  // a position resolved only by percentage
  int skippedPhases = 0;         // phases with no device-side source wired yet
  std::string statusMessage;

  // Metadata-only load, shared by the progress and annotation phases. Null
  // until first needed: the phases that do not touch the book never pay for it.
  void ensureEpubLoaded();

  std::shared_ptr<Epub> epub;

  void runConnectionTest();
  void stepOnePhase();
  const char* phaseLabel(bookorbit::SyncPhase phase) const;
};
