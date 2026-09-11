add_executable(BookOrbitErrorTest
  BookOrbitErrorTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
)

target_include_directories(BookOrbitErrorTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitErrorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitErrorTest)
