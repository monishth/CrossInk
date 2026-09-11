add_executable(BookOrbitAnnotationModelTest
  BookOrbitAnnotationModelTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMd5.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
)

target_include_directories(BookOrbitAnnotationModelTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitAnnotationModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitAnnotationModelTest)
