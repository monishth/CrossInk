add_executable(BookStateDecodeTest
  BookStateDecodeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateCodec.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(BookStateDecodeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookStateDecodeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookStateDecodeTest)
