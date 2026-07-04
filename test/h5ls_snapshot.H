#pragma once

// Test-only helper: parse a committed `h5ls -r` structural snapshot
// (specs/fixtures/*.h5ls) into a path -> {is_dataset, h5ls C-order shape} map,
// plus the schema-conformance assertion primitives (check/expect_dataset/
// expect_group/expect_offsets). Factored verbatim (behavior-preserving) from the
// original test_eos_reader_schema.cpp so the EOS and opacity structural
// self-tests share ONE snapshot parser. Header-only, inline-linkage; include it
// from a single test translation unit. All state (the failure counter) lives in
// namespace h5ls to avoid the global collisions the original file kept private.

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace h5ls {

inline int g_failures = 0;

inline void check(bool ok, const std::string& msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    ++g_failures;
  }
}

// A parsed h5ls entry: object type and (for datasets) its h5ls C-order shape.
struct Entry {
  bool is_dataset = false;
  std::vector<int> shape;  // empty for groups
};

// Unescape the h5ls path field: "\ " -> " " (h5ls escapes spaces in names).
inline std::string unescape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '\\' && i + 1 < in.size() && in[i + 1] == ' ') {
      out.push_back(' ');
      ++i;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

inline std::string rtrim(const std::string& s) {
  const std::size_t e = s.find_last_not_of(" \t");
  return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}

// Parse "{30, 81, 185}" -> {30,81,185}.
inline std::vector<int> parse_shape(const std::string& braces) {
  std::vector<int> out;
  const std::size_t lo = braces.find('{');
  const std::size_t hi = braces.find('}');
  if (lo == std::string::npos || hi == std::string::npos || hi <= lo) return out;
  std::string inner = braces.substr(lo + 1, hi - lo - 1);
  for (char& c : inner) {
    if (c == ',') c = ' ';
  }
  std::istringstream iss(inner);
  int v;
  while (iss >> v) out.push_back(v);
  return out;
}

// Parse an h5ls -r line into (path, Entry). Locates the whole-word type token
// " Dataset" / " Group"; everything before it (trimmed, unescaped) is the path.
inline bool parse_line(const std::string& line, std::string& path, Entry& e) {
  const std::size_t dpos = line.find(" Dataset");
  const std::size_t gpos = line.find(" Group");
  if (dpos != std::string::npos) {
    path = unescape(rtrim(line.substr(0, dpos)));
    e.is_dataset = true;
    e.shape = parse_shape(line.substr(dpos));
    return true;
  }
  if (gpos != std::string::npos) {
    path = unescape(rtrim(line.substr(0, gpos)));
    e.is_dataset = false;
    return true;
  }
  return false;
}

inline std::map<std::string, Entry> load_snapshot(const std::string& file) {
  std::map<std::string, Entry> m;
  std::ifstream in(file);
  if (!in) {
    std::fprintf(stderr, "FAIL: cannot open snapshot %s\n", file.c_str());
    ++g_failures;
    return m;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (rtrim(line).empty()) continue;
    std::string path;
    Entry e;
    if (parse_line(line, path, e)) m[path] = e;
  }
  return m;
}

inline std::string shape_str(const std::vector<int>& s) {
  std::string out = "{";
  for (std::size_t i = 0; i < s.size(); ++i) {
    out += std::to_string(s[i]);
    if (i + 1 < s.size()) out += ",";
  }
  out += "}";
  return out;
}

// Assert a dataset exists with the exact expected h5ls shape.
inline void expect_dataset(const std::map<std::string, Entry>& m,
                           const std::string& path,
                           const std::vector<int>& shape) {
  auto it = m.find(path);
  if (it == m.end()) {
    check(false, "missing dataset " + path);
    return;
  }
  check(it->second.is_dataset, path + " is not a Dataset");
  if (it->second.shape != shape) {
    check(false, path + " shape mismatch, snapshot=" + shape_str(it->second.shape) +
                     " expected=" + shape_str(shape));
  }
}

inline void expect_group(const std::map<std::string, Entry>& m,
                         const std::string& path) {
  auto it = m.find(path);
  if (it == m.end()) {
    check(false, "missing group " + path);
    return;
  }
  check(!it->second.is_dataset, path + " is not a Group");
}

// Assert an Offsets dataset exists with the exact expected RANK and shape. The
// rank is the load-bearing 1D-vs-2D distinction (spec:162): a scattering Offsets
// read as 1D, or an EmAb/EOS Offsets read as 2D, is incorrect.
inline void expect_offsets(const std::map<std::string, Entry>& m,
                           const std::string& path,
                           const std::vector<int>& shape) {
  auto it = m.find(path);
  if (it == m.end() || !it->second.is_dataset) {
    check(false, "missing dataset " + path);
    return;
  }
  check(it->second.shape.size() == shape.size(),
        path + " must be rank-" + std::to_string(shape.size()) +
            " (1D-vs-2D offset distinction), snapshot rank=" +
            std::to_string(it->second.shape.size()));
  if (it->second.shape != shape) {
    check(false, path + " shape mismatch, snapshot=" +
                     shape_str(it->second.shape) + " expected=" + shape_str(shape));
  }
}

}  // namespace h5ls
