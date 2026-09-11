add_executable(BookOrbitNativePositionTest
  NativePositionTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/NativePosition.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitProgress.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(BookOrbitNativePositionTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitNativePositionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitNativePositionTest)
