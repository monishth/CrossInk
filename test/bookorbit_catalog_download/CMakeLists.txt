add_executable(CatalogDownloadTest
  CatalogDownloadTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDownload.cpp
)

target_include_directories(CatalogDownloadTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(CatalogDownloadTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogDownloadTest)
