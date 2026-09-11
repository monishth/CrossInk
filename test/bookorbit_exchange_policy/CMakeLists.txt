add_executable(BookOrbitExchangePolicyTest
  BookOrbitExchangePolicyTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangePolicy.cpp
)

target_include_directories(BookOrbitExchangePolicyTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitExchangePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitExchangePolicyTest)
