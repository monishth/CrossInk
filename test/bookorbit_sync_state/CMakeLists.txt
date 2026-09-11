add_executable(BookOrbitSyncStateTest
  BookOrbitSyncStateTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitSyncState.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
)

target_include_directories(BookOrbitSyncStateTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitSyncStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitSyncStateTest)
