find_package(EXPAT REQUIRED)

add_executable(BookOrbitXPointerResolverTest
  XPointerResolverTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointerResolver.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BookOrbitXPointerResolverTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/test/bookorbit_xpointer_corpus
)

target_compile_definitions(BookOrbitXPointerResolverTest PRIVATE
  CORPUS_FIXTURE_DIR="${REPO_ROOT}/test/bookorbit_xpointer_corpus/fixtures"
)

target_link_libraries(BookOrbitXPointerResolverTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  EXPAT::EXPAT
)

gtest_discover_tests(BookOrbitXPointerResolverTest)
