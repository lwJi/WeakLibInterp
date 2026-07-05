// Self-contained acceptance probe for the DescribeEOSInversionError code->string
// mapping (src/eos/wli_eos_inversion.H, specs/eos-inversion.md:94-106).
//
// Pins the verbatim oracle strings (weaklib/Distributions/EOSSource/
// wlEOSInversionModule.F90:238-244) for each of the 7 valid codes
// {0,1,2,3,10,11,13}, and the fixed sentinel for undefined codes (4-9, 12, >13).
//
// Header-only host math, no HDF5, no amrex::Initialize. Hand-rolled harness
// (mirrors test/test_eos_inversion_family.cpp): local g_failures + check().

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wli_eos_inversion.H"  // the routine under test
#include "wli_real.H"

namespace {

int g_failures = 0;

void check(bool ok, const char* msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failures;
  } else {
    std::printf("  ok: %s\n", msg);
  }
}

// One assertion per code: DescribeEOSInversionError(code) is byte-for-byte the
// expected verbatim oracle string.
void expect(int code, const char* expected, const char* msg) {
  check(std::strcmp(wli::DescribeEOSInversionError(code), expected) == 0, msg);
}

}  // namespace

int main() {
  // The 7 valid codes -> verbatim oracle strings (required assertions).
  expect(0,  "Returned Successfully",                              "code 0");
  expect(1,  "First Argument (D) Outside Table Bounds",           "code 1");
  expect(2,  "Second Argument (E, P, or S) Outside Table Bounds", "code 2");
  expect(3,  "Third Argument (Y) Outside Table Bounds",           "code 3");
  expect(10, "EOS Inversion Not Initialized",                     "code 10");
  expect(11, "NAN in Argument(s)",                                "code 11");
  expect(13, "Unable to Find Any Root",                           "code 13");

  // Undefined codes (4-9, 12, and >13) -> fixed sentinel (default behavior).
  const char* sentinel = "Invalid EOS Inversion Error Code";
  expect(12, sentinel, "code 12 (undefined) -> sentinel");
  expect(99, sentinel, "code 99 (>13) -> sentinel");

  if (g_failures == 0) {
    std::printf("test_eos_inversion_describe: all checks passed\n");
    return EXIT_SUCCESS;
  }
  std::fprintf(stderr, "test_eos_inversion_describe: %d failure(s)\n",
               g_failures);
  return EXIT_FAILURE;
}
