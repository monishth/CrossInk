add_executable(CatalogDecodeTest
  CatalogDecodeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecode.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecodeCommon.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(CatalogDecodeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogDecodeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogDecodeTest)
