add_executable(BookOrbitDateTest
  BookOrbitDateTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
)

target_include_directories(BookOrbitDateTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitDateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitDateTest)
