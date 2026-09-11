add_executable(CatalogManifestTest
  CatalogManifestTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogManifest.cpp
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

target_include_directories(CatalogManifestTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogManifestTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogManifestTest)
