#pragma once

// Version ordering for the OTA update check. Header-only and free of Arduino
// dependencies so it can be unit-tested on the host (test/version_compare).
//
// Accepts upstream-style versions ("1.6.0", "1.6.0-rc+abc123") as well as this
// fork's release tags and build versions ("v1.6.0-lockscreens.2",
// "1.6.0-lockscreens.2", "1.6.0-lockscreens.2-dev-main-abc1234"). A leading
// "v" is ignored. The "-lockscreens.N" release number orders fork releases that
// share an upstream base; a version without it counts as release 0.

#include <cstdlib>
#include <cstring>

namespace version_compare {

struct Version {
  long major = 0;
  long minor = 0;
  long patch = 0;
  long forkRelease = 0;
  bool releaseCandidate = false;
};

// Parses "MAJOR.MINOR.PATCH" (optionally prefixed with v/V) plus the optional
// fork release number and RC marker. Returns false unless all three numeric
// segments are present.
inline bool parse(const char* text, Version& out) {
  if (text == nullptr) return false;
  const char* p = text;
  if (*p == 'v' || *p == 'V') ++p;

  long* const segments[3] = {&out.major, &out.minor, &out.patch};
  for (int i = 0; i < 3; ++i) {
    if (*p < '0' || *p > '9') return false;
    char* end = nullptr;
    *segments[i] = std::strtol(p, &end, 10);
    p = end;
    if (i < 2) {
      if (*p != '.') return false;
      ++p;
    }
  }

  static constexpr char kForkMarker[] = "-lockscreens.";
  out.forkRelease = 0;
  if (const char* marker = std::strstr(p, kForkMarker)) {
    const char* digits = marker + sizeof(kForkMarker) - 1;
    if (*digits >= '0' && *digits <= '9') out.forkRelease = std::strtol(digits, nullptr, 10);
  }
  out.releaseCandidate = std::strstr(p, "-rc") != nullptr;
  return true;
}

// True only when `latest` is strictly newer than `current`. Input that doesn't
// parse on either side is never treated as an update.
inline bool isNewer(const char* latest, const char* current) {
  Version l;
  Version c;
  if (!parse(latest, l) || !parse(current, c)) return false;
  if (l.major != c.major) return l.major > c.major;
  if (l.minor != c.minor) return l.minor > c.minor;
  if (l.patch != c.patch) return l.patch > c.patch;
  if (l.forkRelease != c.forkRelease) return l.forkRelease > c.forkRelease;
  // Same release: a final build supersedes the release candidate running now.
  return c.releaseCandidate && !l.releaseCandidate;
}

}  // namespace version_compare
