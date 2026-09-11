add_executable(BookOrbitXPointerTest
  XPointerTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
)

target_include_directories(BookOrbitXPointerTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/test/bookorbit_xpointer_corpus
)

target_compile_definitions(BookOrbitXPointerTest PRIVATE
  CORPUS_FIXTURE_DIR="${REPO_ROOT}/test/bookorbit_xpointer_corpus/fixtures"
)

target_link_libraries(BookOrbitXPointerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitXPointerTest)
