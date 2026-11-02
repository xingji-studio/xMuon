#pragma once

#include <cstddef>
#include <string>

// Decode one complete HTTP representation within the caller's output budget.
// Failed decoding preserves the original body. Gzip trailers and optional
// header checksums are validated; trailing bytes/members are rejected.
bool browser_decode_content(std::string &body, const std::string &encoding, std::size_t limit, std::string &error);
