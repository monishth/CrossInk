add_executable(BookOrbitOutboxTest
  BookOrbitOutboxTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitOutbox.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitSyncState.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
)

target_include_directories(BookOrbitOutboxTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitOutboxTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitOutboxTest)
