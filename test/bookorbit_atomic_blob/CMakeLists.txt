add_executable(AtomicBlobWriterTest
  AtomicBlobWriterTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
)

target_include_directories(AtomicBlobWriterTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(AtomicBlobWriterTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AtomicBlobWriterTest)
