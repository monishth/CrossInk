add_executable(ClampPolicyTest
  ClampPolicyTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ClampPolicy.cpp
)

target_include_directories(ClampPolicyTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(ClampPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClampPolicyTest)
