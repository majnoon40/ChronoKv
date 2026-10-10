#pragma once
// tests/test_decls.hpp — cross-TU test battery entry points (v29 M2 item 5:
// the test-suite TU split, unlocked by API-1's ODR-safe header). Declared
// once here; main.cpp calls these (full-suite order + the CKV_ONLY_* gates);
// each tests/tests_*.cpp TU defines exactly one battery family. Bodies ride
// under CHRONOKV_TEST_HOOKS in their TUs; hooks-off builds see declarations
// that are never called (main.cpp's call sites are hooks-on-only), so the
// smoke build links clean.
int run_lincheck_test();      // tests/tests_lincheck.cpp   (v27 M1)
int run_dst_test();           // tests/tests_dst.cpp        (v27 M0)
int run_remediation_tests();  // tests/tests_remediation.cpp (v28 + Audit-2)
