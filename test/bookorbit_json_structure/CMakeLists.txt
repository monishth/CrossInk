add_executable(JsonStructureTest
  JsonStructureTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(JsonStructureTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(JsonStructureTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(JsonStructureTest)
