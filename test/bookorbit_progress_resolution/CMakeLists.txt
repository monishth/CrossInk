add_executable(BookOrbitProgressResolutionTest
  ProgressResolutionTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ProgressResolution.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitProgress.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(BookOrbitProgressResolutionTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitProgressResolutionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitProgressResolutionTest)
