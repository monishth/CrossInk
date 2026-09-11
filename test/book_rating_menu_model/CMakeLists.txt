add_executable(BookRatingMenuModelTest
  BookRatingMenuModelTest.cpp
)

target_include_directories(BookRatingMenuModelTest PRIVATE
  ${REPO_ROOT}
)

target_link_libraries(BookRatingMenuModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookRatingMenuModelTest)
