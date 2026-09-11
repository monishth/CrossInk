add_executable(PageStatsCodecTest
  PageStatsCodecTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/PageStatsCodec.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEvent.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(PageStatsCodecTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(PageStatsCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PageStatsCodecTest)
