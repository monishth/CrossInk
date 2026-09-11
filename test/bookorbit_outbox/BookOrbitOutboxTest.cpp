#include <gtest/gtest.h>

#include "lib/BookOrbit/BookOrbitError.h"
#include "lib/BookOrbit/BookOrbitOutbox.h"

using bookorbit::classify;
using bookorbit::SyncOutbox;
using bookorbit::SyncPhase;

TEST(BookOrbitOutbox, StartsAtMatch) {
  const SyncOutbox outbox;
  EXPECT_EQ(outbox.currentPhase(), SyncPhase::Match);
}

TEST(BookOrbitOutbox, PhaseOrderMatchesSpec) {
  SyncOutbox outbox;
  EXPECT_EQ(outbox.nextPhase(SyncPhase::Match), SyncPhase::Stats);
  EXPECT_EQ(outbox.nextPhase(SyncPhase::Stats), SyncPhase::Progress);
  EXPECT_EQ(outbox.nextPhase(SyncPhase::Progress), SyncPhase::State);
  EXPECT_EQ(outbox.nextPhase(SyncPhase::State), SyncPhase::Annotations);
  EXPECT_EQ(outbox.nextPhase(SyncPhase::Annotations), SyncPhase::Bookmarks);
  EXPECT_EQ(outbox.nextPhase(SyncPhase::Bookmarks), SyncPhase::Done);
}

TEST(BookOrbitOutbox, SuccessAdvancesToNextPhase) {
  SyncOutbox outbox;
  ASSERT_TRUE(outbox.advance(classify(200, false)));
  EXPECT_EQ(outbox.currentPhase(), SyncPhase::Stats);
}

// An auth failure aborts the WHOLE sync, not just this phase.
TEST(BookOrbitOutbox, AuthErrorAbortsEntireSync) {
  SyncOutbox outbox;
  EXPECT_FALSE(outbox.advance(classify(401, false)));
  EXPECT_TRUE(outbox.isAborted());
}

TEST(BookOrbitOutbox, TransportErrorAbortsEntireSync) {
  SyncOutbox outbox;
  EXPECT_FALSE(outbox.advance(classify(0, true)));
  EXPECT_TRUE(outbox.isAborted());
}

// A non-auth HTTP error fails just this phase and moves on. The watermark
// stays unadvanced, so the data retries on the next sync trigger.
TEST(BookOrbitOutbox, ServerErrorSkipsPhaseButContinues) {
  SyncOutbox outbox;
  ASSERT_TRUE(outbox.advance(classify(500, false)));
  EXPECT_EQ(outbox.currentPhase(), SyncPhase::Stats);
  EXPECT_FALSE(outbox.isAborted());
  EXPECT_TRUE(outbox.hadErrors());
}

TEST(BookOrbitOutbox, RunsToCompletion) {
  SyncOutbox outbox;
  for (int i = 0; i < 6; i++) {
    ASSERT_TRUE(outbox.advance(classify(200, false))) << "stopped at step " << i;
  }
  EXPECT_EQ(outbox.currentPhase(), SyncPhase::Done);
  EXPECT_FALSE(outbox.hadErrors());
}

TEST(BookOrbitOutbox, AdvanceAfterDoneIsANoOp) {
  SyncOutbox outbox;
  for (int i = 0; i < 6; i++) outbox.advance(classify(200, false));
  EXPECT_FALSE(outbox.advance(classify(200, false)));
  EXPECT_EQ(outbox.currentPhase(), SyncPhase::Done);
}
