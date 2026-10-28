// Portable UTC timestamps and identifiers (no gmtime_r/gmtime_s differences between platforms).
#pragma once

#include <cstdint>
#include <string>

namespace fit {

// "2026-10-25T21:04:05Z"
std::string utc_iso_from_unix(int64_t unix_seconds);
std::string utc_now_iso();
int64_t unix_now();
// Parses the format above; returns false on malformed input.
bool parse_utc_iso(const std::string& s, int64_t& unix_seconds);
// Compact form for file names: "20261025-210405".
std::string utc_compact_now();

// Random identifier "prefix-1a2b3c4d5e6f7a8b" (time + process-local counter seeded entropy).
std::string make_id(const std::string& prefix);

}  // namespace fit
