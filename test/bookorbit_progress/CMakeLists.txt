add_executable(BookOrbitProgressTest
  BookOrbitProgressTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitProgress.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
  ${REPO_ROOT}/lib/BookOrbit/NativePosition.cpp
)

target_include_directories(BookOrbitProgressTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitProgressTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitProgressTest)
