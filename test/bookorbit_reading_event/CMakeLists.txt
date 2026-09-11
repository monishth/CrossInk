add_executable(ReadingEventTest
  ReadingEventTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEvent.cpp
)

target_include_directories(ReadingEventTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(ReadingEventTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingEventTest)
