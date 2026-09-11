add_executable(StatsGroundTruthTest
  StatsGroundTruthTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingStatsQuery.cpp
  ${REPO_ROOT}/lib/BookOrbit/ClampPolicy.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEvent.cpp
)

target_include_directories(StatsGroundTruthTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_compile_definitions(StatsGroundTruthTest PRIVATE
  BOOKORBIT_GROUND_TRUTH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(StatsGroundTruthTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StatsGroundTruthTest)
