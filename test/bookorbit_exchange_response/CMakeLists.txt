add_executable(BookOrbitExchangeResponseTest
  BookOrbitExchangeResponseTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeResponse.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(BookOrbitExchangeResponseTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitExchangeResponseTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitExchangeResponseTest)
