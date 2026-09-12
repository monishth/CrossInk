add_executable(CatalogApiTest
  CatalogApiTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogApi.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogMutations.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogQuery.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecode.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecodeCommon.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCapabilities.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(CatalogApiTest PRIVATE
  ${REPO_ROOT}/test/minibidi_arabic/stubs  # no-op Logging.h for host tests
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogApiTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogApiTest)
