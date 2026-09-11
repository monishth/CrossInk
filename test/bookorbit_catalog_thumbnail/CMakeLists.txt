add_executable(CatalogThumbnailTest
  CatalogThumbnailTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogThumbnail.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDownload.cpp
)

target_include_directories(CatalogThumbnailTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(CatalogThumbnailTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogThumbnailTest)
