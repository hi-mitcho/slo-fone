add_executable(ReadwiseHtmlExtractorTest
  ReadwiseHtmlExtractorTest.cpp
  ${REPO_ROOT}/src/network/ReadwiseHtmlExtractor.cpp
)

target_include_directories(ReadwiseHtmlExtractorTest PRIVATE
  ${REPO_ROOT}
)

target_link_libraries(ReadwiseHtmlExtractorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadwiseHtmlExtractorTest)
