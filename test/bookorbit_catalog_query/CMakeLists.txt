add_executable(CatalogQueryTest
  CatalogQueryTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogQuery.cpp
)

target_include_directories(CatalogQueryTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(CatalogQueryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogQueryTest)
