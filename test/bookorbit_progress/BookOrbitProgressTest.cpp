#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookOrbitProgress.h"
#include "lib/BookOrbit/NativePosition.h"

using bookorbit::decodeProgressResponse;
using bookorbit::encodePutProgress;
using bookorbit::progressGetPath;
using bookorbit::ProgressRecord;

namespace {

ProgressRecord sample() {
  ProgressRecord record;
  record.document = "d18e399f0f79f24d68a8f70b76d59914";
  record.percentage = 0.001859f;
  record.progress = "/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0";
  record.device = "CrossInk X4 Pro";
  record.deviceId = "A1B2C3D4E5F6";
  record.timestamp = 1787561453u;
  return record;
}

}  // namespace

TEST(BookOrbitProgress, GetPathCarriesTheDigest) {
  EXPECT_EQ(progressGetPath("d18e399f0f79f24d68a8f70b76d59914"),
            "/koreader/syncs/progress/d18e399f0f79f24d68a8f70b76d59914");
}

TEST(BookOrbitProgress, GetPathRejectsEmptyDigest) { EXPECT_EQ(progressGetPath(""), ""); }

TEST(BookOrbitProgress, EncodesEveryRequestField) {
  EXPECT_EQ(encodePutProgress(sample()),
            "{\"document\":\"d18e399f0f79f24d68a8f70b76d59914\","
            "\"percentage\":0.001859,"
            "\"progress\":\"/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0\","
            "\"device\":\"CrossInk X4 Pro\","
            "\"device_id\":\"A1B2C3D4E5F6\","
            "\"timestamp\":1787561453}");
}

TEST(BookOrbitProgress, ClampsPercentageIntoRange) {
  ProgressRecord high = sample();
  high.percentage = 1.7f;
  EXPECT_NE(encodePutProgress(high).find("\"percentage\":1.000000"), std::string::npos);

  ProgressRecord low = sample();
  low.percentage = -0.4f;
  EXPECT_NE(encodePutProgress(low).find("\"percentage\":0.000000"), std::string::npos);
}

// Never degrade silently: an encode with no xpointer is a programming error
// upstream, and the encoder refuses rather than shipping a percentage-only
// record that a KOReader client would then read back as the whole truth.
TEST(BookOrbitProgress, RefusesToEncodeWithoutAnXpointer) {
  ProgressRecord noProgress = sample();
  noProgress.progress.clear();
  EXPECT_EQ(encodePutProgress(noProgress), "");
}

TEST(BookOrbitProgress, RefusesToEncodeWithoutADocument) {
  ProgressRecord noDocument = sample();
  noDocument.document.clear();
  EXPECT_EQ(encodePutProgress(noDocument), "");
}

TEST(BookOrbitProgress, EscapesQuotesAndBackslashesInDeviceNames) {
  ProgressRecord quoted = sample();
  quoted.device = "Jo\"s \\ Reader";
  EXPECT_NE(encodePutProgress(quoted).find("\"device\":\"Jo\\\"s \\\\ Reader\""), std::string::npos);
}

TEST(BookOrbitProgress, DecodesEveryResponseField) {
  const char* json =
      "{\"percentage\":0.2052,"
      "\"progress\":\"/body[1]/DocFragment[8]/body[1]/p[4]/text()[1].96\","
      "\"device\":\"KOReader\","
      "\"device_id\":\"9F8E7D\","
      "\"timestamp\":1787407272}";

  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse(json, out));
  EXPECT_FLOAT_EQ(out.percentage, 0.2052f);
  EXPECT_EQ(out.progress, "/body[1]/DocFragment[8]/body[1]/p[4]/text()[1].96");
  EXPECT_EQ(out.device, "KOReader");
  EXPECT_EQ(out.deviceId, "9F8E7D");
  EXPECT_EQ(out.timestamp, 1787407272u);
}

// The unindexed form still arrives from older CrossInk writes; decoding
// normalizes it so callers only ever compare one shape.
TEST(BookOrbitProgress, NormalizesTheProgressFieldOnDecode) {
  const char* json = "{\"percentage\":0.5,\"progress\":\"/body/DocFragment[2]/body/p[3]\",\"timestamp\":1}";
  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse(json, out));
  EXPECT_EQ(out.progress, "/body[1]/DocFragment[2]/body[1]/p[3]");
}

TEST(BookOrbitProgress, KeepsAnUnparseableProgressStringVerbatim) {
  const char* json = "{\"percentage\":0.5,\"progress\":\"something-else\",\"timestamp\":1}";
  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse(json, out));
  EXPECT_EQ(out.progress, "something-else");
}

TEST(BookOrbitProgress, EmptyResponseDecodesToAZeroedRecord) {
  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse("{}", out));
  EXPECT_EQ(out.progress, "");
  EXPECT_FLOAT_EQ(out.percentage, 0.0f);
  EXPECT_EQ(out.timestamp, 0u);
}

TEST(BookOrbitProgress, MalformedJsonIsRejected) {
  ProgressRecord out;
  EXPECT_FALSE(decodeProgressResponse("{\"percentage\":", out));
}

TEST(BookOrbitProgress, EncodeDecodeRoundTrip) {
  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse(encodePutProgress(sample()), out));
  EXPECT_EQ(out.progress, sample().progress);
  EXPECT_EQ(out.device, sample().device);
  EXPECT_EQ(out.deviceId, sample().deviceId);
  EXPECT_EQ(out.timestamp, sample().timestamp);
}

TEST(BookOrbitProgress, EncodesTheNativePositionWhenPresent) {
  ProgressRecord record = sample();
  record.position.pctQ = 205200u;
  record.position.spine = 7u;
  record.position.page = 3u;
  record.position.pages = 12u;
  record.position.para = 42u;
  record.position.xpath = "/body[1]/DocFragment[8]/body[1]/p[42]";
  record.position.present = true;

  const std::string body = encodePutProgress(record);
  EXPECT_NE(body.find("\"position\":{\"pctQ\":205200"), std::string::npos);
  // The two Approach A fields are still there. Approach B never replaces them.
  EXPECT_NE(body.find("\"progress\":"), std::string::npos);
  EXPECT_NE(body.find("\"percentage\":"), std::string::npos);
}

TEST(BookOrbitProgress, OmitsTheNativePositionWhenAbsent) {
  EXPECT_EQ(encodePutProgress(sample()).find("\"position\""), std::string::npos);
}

TEST(BookOrbitProgress, DecodesTheNativePositionFromAResponse) {
  const char* json =
      "{\"percentage\":0.2052,\"progress\":\"/body[1]/DocFragment[8]/body[1]/p[42]\",\"timestamp\":1,"
      "\"position\":{\"pctQ\":205200,\"spine\":7,\"page\":3,\"pages\":12,\"para\":42,"
      "\"xpath\":\"/body[1]/DocFragment[8]/body[1]/p[42]\"}}";

  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse(json, out));
  EXPECT_TRUE(out.position.present);
  EXPECT_EQ(out.position.spine, 7u);
  EXPECT_EQ(out.position.para, 42u);
}

TEST(BookOrbitProgress, ResponseWithoutAPositionStillDecodes) {
  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse("{\"percentage\":0.5,\"progress\":\"/body[1]/DocFragment[1]/body[1]\"}", out));
  EXPECT_FALSE(out.position.present);
}
