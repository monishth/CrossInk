add_executable(BookStateStoreTest
  BookStateStoreTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateStore.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
)

target_include_directories(BookStateStoreTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookStateStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookStateStoreTest)
