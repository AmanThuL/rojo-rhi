from __future__ import annotations

import unittest

from Tools import header_compile_time as tool


class HeaderCompileTimeTests(unittest.TestCase):
    def test_standard_includes_are_read_in_order(self) -> None:
        header = "#pragma once\n\n#include <cstdint>\n#include <span>\n#include \"Local.h\"\n"
        self.assertEqual(tool.standard_includes(header), ["cstdint", "span"])

    def test_the_public_header_includes_standard_headers(self) -> None:
        self.assertIn("cstdint", tool.standard_includes(tool.HEADER.read_text()))

    def test_no_baseline_never_warns(self) -> None:
        self.assertEqual(tool.compare({"ownMs": 100.0, "ownBytes": 10**9}, None), [])

    def test_bytes_past_the_factor_warn(self) -> None:
        warnings = tool.compare({"ownMs": 3.0, "ownBytes": 50000}, {"ownMs": 3.0, "ownBytes": 30000})
        self.assertEqual(len(warnings), 1)
        self.assertIn("ownBytes", warnings[0])

    def test_small_time_growth_within_noise_does_not_warn(self) -> None:
        self.assertEqual(tool.compare({"ownMs": 9.0, "ownBytes": 1}, {"ownMs": 3.0, "ownBytes": 1}), [])

    def test_large_time_growth_warns(self) -> None:
        warnings = tool.compare({"ownMs": 30.0, "ownBytes": 1}, {"ownMs": 3.0, "ownBytes": 1})
        self.assertIn("ownMs", warnings[0])


if __name__ == "__main__":
    unittest.main()
