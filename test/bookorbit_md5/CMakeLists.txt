add_executable(BookOrbitMd5Test
  BookOrbitMd5Test.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMd5.cpp
)

target_include_directories(BookOrbitMd5Test PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitMd5Test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitMd5Test)
