add_executable(BookStateMergeTest
  BookStateMergeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateMerge.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateCodec.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(BookStateMergeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookStateMergeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookStateMergeTest)
