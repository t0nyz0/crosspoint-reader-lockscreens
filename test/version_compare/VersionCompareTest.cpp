#include <VersionCompare.h>
#include <gtest/gtest.h>

using version_compare::isNewer;
using version_compare::parse;
using version_compare::Version;

TEST(VersionCompareParse, ReadsForkTagWithLeadingV) {
  Version v;
  ASSERT_TRUE(parse("v1.6.0-lockscreens.2", v));
  EXPECT_EQ(v.major, 1);
  EXPECT_EQ(v.minor, 6);
  EXPECT_EQ(v.patch, 0);
  EXPECT_EQ(v.forkRelease, 2);
  EXPECT_FALSE(v.releaseCandidate);
}

TEST(VersionCompareParse, PlainUpstreamVersionIsForkReleaseZero) {
  Version v;
  ASSERT_TRUE(parse("1.6.5", v));
  EXPECT_EQ(v.patch, 5);
  EXPECT_EQ(v.forkRelease, 0);
}

TEST(VersionCompareParse, RejectsIncompleteOrNonNumericVersions) {
  Version v;
  EXPECT_FALSE(parse(nullptr, v));
  EXPECT_FALSE(parse("", v));
  EXPECT_FALSE(parse("latest", v));
  EXPECT_FALSE(parse("v1.6", v));
  EXPECT_FALSE(parse("1..0", v));
  EXPECT_FALSE(parse("vx.y.z", v));
}

TEST(VersionCompareIsNewer, NewerForkReleaseOnSameBase) {
  EXPECT_TRUE(isNewer("v1.6.0-lockscreens.2", "1.6.0-lockscreens.1"));
  EXPECT_FALSE(isNewer("v1.6.0-lockscreens.1", "1.6.0-lockscreens.2"));
}

TEST(VersionCompareIsNewer, SameReleaseIsNotAnUpdate) {
  // The release tag carries a "v"; the running build doesn't.
  EXPECT_FALSE(isNewer("v1.6.0-lockscreens.2", "1.6.0-lockscreens.2"));
  // Dev builds append branch + sha after the fork release number.
  EXPECT_FALSE(isNewer("v1.6.0-lockscreens.2", "1.6.0-lockscreens.2-dev-main-abc1234"));
}

TEST(VersionCompareIsNewer, UpstreamBaseBumpWins) {
  EXPECT_TRUE(isNewer("v1.6.5-lockscreens.1", "1.6.0-lockscreens.4"));
  EXPECT_FALSE(isNewer("v1.6.0-lockscreens.9", "1.6.5-lockscreens.1"));
}

TEST(VersionCompareIsNewer, ComparesSegmentsNumericallyNotLexically) {
  EXPECT_TRUE(isNewer("v1.10.0-lockscreens.1", "1.9.9-lockscreens.1"));
  EXPECT_TRUE(isNewer("v1.6.0-lockscreens.10", "1.6.0-lockscreens.9"));
}

TEST(VersionCompareIsNewer, FinalReleaseSupersedesReleaseCandidate) {
  EXPECT_TRUE(isNewer("1.6.0", "1.6.0-rc+abc123"));
  EXPECT_FALSE(isNewer("1.6.0-rc+def456", "1.6.0-rc+abc123"));
  EXPECT_FALSE(isNewer("1.6.0", "1.6.0"));
}

TEST(VersionCompareIsNewer, UnparseableVersionsAreNeverUpdates) {
  EXPECT_FALSE(isNewer("latest", "1.6.0-lockscreens.2"));
  EXPECT_FALSE(isNewer("v1.6.0-lockscreens.3", "unknown"));
  EXPECT_FALSE(isNewer(nullptr, "1.6.0"));
}
