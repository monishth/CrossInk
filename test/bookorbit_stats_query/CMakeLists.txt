add_executable(ReadingStatsQueryTest
  ReadingStatsQueryTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingStatsQuery.cpp
  ${REPO_ROOT}/lib/BookOrbit/ClampPolicy.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEvent.cpp
)

target_include_directories(ReadingStatsQueryTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(ReadingStatsQueryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsQueryTest)
