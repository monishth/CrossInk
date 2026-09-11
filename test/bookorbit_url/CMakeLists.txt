add_executable(BookOrbitUrlTest
  BookOrbitUrlTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
)

target_include_directories(BookOrbitUrlTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitUrlTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitUrlTest)
