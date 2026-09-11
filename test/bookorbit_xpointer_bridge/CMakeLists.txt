add_executable(BookOrbitXPointerBridgeTest
  XPointerBridgeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
)

target_include_directories(BookOrbitXPointerBridgeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitXPointerBridgeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitXPointerBridgeTest)
