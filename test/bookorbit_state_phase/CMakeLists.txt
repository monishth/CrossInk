add_executable(BookStatePhaseTest
  BookStatePhaseTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStatePhase.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateStore.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateMerge.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateCodec.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitSyncState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/BookOrbit/JsonStructure.cpp
)

target_include_directories(BookStatePhaseTest PRIVATE
  ${REPO_ROOT}/test/minibidi_arabic/stubs  # no-op Logging.h for host tests
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookStatePhaseTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookStatePhaseTest)
