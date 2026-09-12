add_executable(CatalogBookDetailTest
  CatalogBookDetailTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogBookDetail.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(CatalogBookDetailTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogBookDetailTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogBookDetailTest)
