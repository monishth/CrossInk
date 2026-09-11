add_executable(PageResolverTest
  PageResolverTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/PageResolver.cpp
)

target_include_directories(PageResolverTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(PageResolverTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PageResolverTest)
