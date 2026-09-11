add_executable(BookOrbitXPointerDomTest
  XPointerDomTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
)

target_include_directories(BookOrbitXPointerDomTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/test/bookorbit_xpointer_corpus
)

target_compile_definitions(BookOrbitXPointerDomTest PRIVATE
  CORPUS_FIXTURE_DIR="${REPO_ROOT}/test/bookorbit_xpointer_corpus/fixtures"
)

target_link_libraries(BookOrbitXPointerDomTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitXPointerDomTest)
