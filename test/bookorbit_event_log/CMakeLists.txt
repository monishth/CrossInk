add_executable(ReadingEventLogTest
  ReadingEventLogTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEventLog.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEvent.cpp
)

target_include_directories(ReadingEventLogTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(ReadingEventLogTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingEventLogTest)
