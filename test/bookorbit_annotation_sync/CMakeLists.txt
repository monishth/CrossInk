add_executable(BookOrbitAnnotationSyncTest
  BookOrbitAnnotationSyncTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationSync.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookmarkModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangePolicy.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeRequest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeResponse.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeAck.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitSyncState.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMd5.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(BookOrbitAnnotationSyncTest PRIVATE
  ${REPO_ROOT}/test/minibidi_arabic/stubs  # no-op Logging.h for host tests
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitAnnotationSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitAnnotationSyncTest)
