add_executable(SameOriginTest
  SameOriginTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/SameOrigin.cpp
)

target_include_directories(SameOriginTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(SameOriginTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SameOriginTest)
