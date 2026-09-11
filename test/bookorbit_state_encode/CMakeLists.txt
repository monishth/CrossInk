add_executable(BookStateEncodeTest
  BookStateEncodeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateCodec.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(BookStateEncodeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookStateEncodeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookStateEncodeTest)
