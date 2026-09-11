add_executable(BookOrbitCorpusFixtureTest
  CorpusFixtureTest.cpp
)

target_include_directories(BookOrbitCorpusFixtureTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(BookOrbitCorpusFixtureTest PRIVATE
  CORPUS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(BookOrbitCorpusFixtureTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitCorpusFixtureTest)
