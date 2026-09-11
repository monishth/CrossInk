add_executable(BookOrbitBookmarkModelTest
  BookOrbitBookmarkModelTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookmarkModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMd5.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
)

target_include_directories(BookOrbitBookmarkModelTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitBookmarkModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitBookmarkModelTest)
