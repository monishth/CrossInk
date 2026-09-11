add_executable(BookOrbitCapabilitiesTest
  BookOrbitCapabilitiesTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCapabilities.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
)

target_include_directories(BookOrbitCapabilitiesTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitCapabilitiesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitCapabilitiesTest)
