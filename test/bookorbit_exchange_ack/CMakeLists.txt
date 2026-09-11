add_executable(BookOrbitExchangeAckTest
  BookOrbitExchangeAckTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeAck.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(BookOrbitExchangeAckTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitExchangeAckTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitExchangeAckTest)
