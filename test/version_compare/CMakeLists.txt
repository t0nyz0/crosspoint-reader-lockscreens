add_executable(VersionCompareTest
  VersionCompareTest.cpp
)

target_include_directories(VersionCompareTest PRIVATE
  ${REPO_ROOT}/lib/VersionCompare
)

target_link_libraries(VersionCompareTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VersionCompareTest)
