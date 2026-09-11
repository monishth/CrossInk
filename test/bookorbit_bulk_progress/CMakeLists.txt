add_executable(BookOrbitBulkProgressTest
  BookOrbitBulkProgressTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBulkProgress.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitProgress.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(BookOrbitBulkProgressTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitBulkProgressTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitBulkProgressTest)
