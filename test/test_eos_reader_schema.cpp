// Structural-conformance self-test for the EOS HDF5 reader.
//
// Runs WITHOUT the multi-GB production table (spec check #1,
// specs/table-format-and-io.md:171-175). It does NOT open any live .h5. Instead
// it parses the committed structural snapshot
// specs/fixtures/wl-EOS-SFHo-15-25-50.h5ls into a path -> shape map, then
// asserts that the schema the reader depends on (the fixed dataset names +
// ranks/shapes exposed as static data in src/lib/wli_io_eos.H) matches the
// snapshot EXACTLY. This checks reader-expectation vs snapshot, not snapshot
// self-consistency.
//
// The fixture path is injected at compile time via WLI_EOS_H5LS.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "wli_io_eos.H"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& msg) {
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
std::string unescape(const std::string& in) {
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

std::string rtrim(const std::string& s) {
  const std::size_t e = s.find_last_not_of(" \t");
  return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}

// Parse "{30, 81, 185}" -> {30,81,185}.
std::vector<int> parse_shape(const std::string& braces) {
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
bool parse_line(const std::string& line, std::string& path, Entry& e) {
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

std::map<std::string, Entry> load_snapshot(const std::string& file) {
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

// Assert a dataset exists with the exact expected h5ls shape.
void expect_dataset(const std::map<std::string, Entry>& m, const std::string& path,
                    const std::vector<int>& shape) {
  auto it = m.find(path);
  if (it == m.end()) {
    check(false, "missing dataset " + path);
    return;
  }
  check(it->second.is_dataset, path + " is not a Dataset");
  if (it->second.shape != shape) {
    std::string got = "{";
    for (std::size_t i = 0; i < it->second.shape.size(); ++i) {
      got += std::to_string(it->second.shape[i]);
      if (i + 1 < it->second.shape.size()) got += ",";
    }
    got += "}";
    check(false, path + " shape mismatch, snapshot=" + got);
  }
}

void expect_group(const std::map<std::string, Entry>& m, const std::string& path) {
  auto it = m.find(path);
  if (it == m.end()) {
    check(false, "missing group " + path);
    return;
  }
  check(!it->second.is_dataset, path + " is not a Group");
}

}  // namespace

int main() {
  namespace S = wli::io::schema;

  const std::string h5ls = WLI_EOS_H5LS;
  const std::map<std::string, Entry> m = load_snapshot(h5ls);
  if (m.empty()) {
    std::fprintf(stderr, "FAIL: empty/unreadable snapshot\n");
    return EXIT_FAILURE;
  }

  const int nRho = S::kAxisExtent[0];  // 185
  const int nT = S::kAxisExtent[1];    // 81
  const int nYe = S::kAxisExtent[2];   // 30
  const std::vector<int> subShape = {nYe, nT, nRho};  // h5ls C-order {30,81,185}

  // --- Exactly three top-level groups: ThermoState/DependentVariables/Metadata.
  int topGroups = 0;
  for (const auto& kv : m) {
    const std::string& p = kv.first;
    if (p == "/") continue;
    if (!kv.second.is_dataset && p.find('/', 1) == std::string::npos) ++topGroups;
  }
  check(topGroups == 3, "expected exactly 3 top-level groups, found " +
                            std::to_string(topGroups));
  for (const char* g : S::kGroups) expect_group(m, std::string("/") + g);

  // --- /ThermoState axes: Density{185}/Temperature{81}/Electron Fraction{30}
  //     => Fortran (185,81,30) = (nRho,nT,nYe).
  expect_dataset(m, "/ThermoState/Density", {185});
  expect_dataset(m, "/ThermoState/Temperature", {81});
  expect_dataset(m, "/ThermoState/Electron Fraction", {30});
  for (int a = 0; a < 3; ++a) {
    expect_dataset(m, std::string("/ThermoState/") + S::kAxisNames[a],
                   {S::kAxisExtent[a]});
  }
  expect_dataset(m, "/ThermoState/Names", {3});
  expect_dataset(m, "/ThermoState/Units", {3});
  expect_dataset(m, "/ThermoState/Dimensions", {3});
  expect_dataset(m, "/ThermoState/LogInterp", {3});
  expect_dataset(m, "/ThermoState/iRho", {1});
  expect_dataset(m, "/ThermoState/iT", {1});
  expect_dataset(m, "/ThermoState/iYe", {1});

  // --- /DependentVariables scalar/1D metadata.
  //     Offsets MUST be rank-1 length nVariables (guards 1D-vs-2D, spec:162).
  expect_dataset(m, "/DependentVariables/nVariables", {1});
  {
    auto it = m.find("/DependentVariables/Offsets");
    check(it != m.end() && it->second.is_dataset, "missing /DependentVariables/Offsets");
    if (it != m.end()) {
      check(it->second.shape.size() == 1,
            "/DependentVariables/Offsets must be rank-1 (1D offsets), rank=" +
                std::to_string(it->second.shape.size()));
      check(it->second.shape.size() == 1 && it->second.shape[0] == S::kNVariables,
            "/DependentVariables/Offsets length must equal nVariables=15");
    }
  }
  expect_dataset(m, "/DependentVariables/Names", {S::kNVariables});
  expect_dataset(m, "/DependentVariables/Units", {S::kNVariables});
  expect_dataset(m, "/DependentVariables/Dimensions", {3});
  expect_dataset(m, "/DependentVariables/minValues", {S::kNVariables});
  expect_dataset(m, "/DependentVariables/maxValues", {S::kNVariables});
  expect_dataset(m, "/DependentVariables/Repaired", subShape);

  // --- 15 named value sub-tables {30,81,185}, and 15 i<Name> {1} slots.
  for (const char* name : S::kDVNames) {
    expect_dataset(m, std::string("/DependentVariables/") + name, subShape);
  }
  for (const char* iname : S::kIDVNames) {
    expect_dataset(m, std::string("/DependentVariables/") + iname, {1});
  }

  // --- The count of {30,81,185} value datasets EXCLUDING Repaired must equal
  //     15 == len(Names) == len(Offsets) == len(min/maxValues).
  int subTableCount = 0;
  const std::string dvPrefix = "/DependentVariables/";
  for (const auto& kv : m) {
    if (!kv.second.is_dataset) continue;
    if (kv.first.compare(0, dvPrefix.size(), dvPrefix) != 0) continue;
    if (kv.first == "/DependentVariables/Repaired") continue;  // not a named DV
    if (kv.second.shape == subShape) ++subTableCount;
  }
  check(subTableCount == S::kNVariables,
        "expected 15 named {30,81,185} DV sub-tables (excl. Repaired), found " +
            std::to_string(subTableCount));

  // --- 7 /Metadata/* {1} datasets.
  int metaCount = 0;
  const std::string mdPrefix = "/Metadata/";
  for (const auto& kv : m) {
    if (kv.second.is_dataset && kv.first.compare(0, mdPrefix.size(), mdPrefix) == 0)
      ++metaCount;
  }
  check(metaCount == 7, "expected 7 /Metadata/* datasets, found " +
                            std::to_string(metaCount));
  for (const char* md : S::kMetadata) {
    expect_dataset(m, std::string("/Metadata/") + md, {1});
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "FAIL eos_reader_schema: %d check(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf(
      "PASS eos_reader_schema: reader schema matches snapshot "
      "(3 groups, axes (185,81,30), 15 DVs, 1D Offsets{15}, 7 metadata)\n");
  return EXIT_SUCCESS;
}
