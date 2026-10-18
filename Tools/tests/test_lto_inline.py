from __future__ import annotations

import unittest

from Tools import check_lto_inline

OBJDUMP_INLINED = """
Disassembly of section __TEXT,__text:

00000001000006a8 <_rojorhiLtoProbe>:
1000006a8:     \tldr\tx8, [x0]
1000006ac:     \tldr\tw0, [x8, #0x680]
1000006b0:     \tret
"""

OBJDUMP_CALLED = """
0000000100000790 <_rojorhiLtoProbe>:
100000790:     \tstp\tx29, x30, [sp, #-0x10]!
1000007a0:     \tbl\t0x1000235bc <__ZNK7rojoRHI6Device12capabilitiesEv>
1000007a4:     \tldp\tx29, x30, [sp], #0x10
0000000100000800 <_main>:
100000800:     \tbl\t0x1000235bc <__ZNK7rojoRHI6Device12capabilitiesEv>
"""

DUMPBIN_TAIL_CALLED = """
main:
  0000000140001000: call        ?capabilities@Device@rojoRHI@@QEBAAEBUCapabilities@2@XZ
rojorhiLtoProbe:
  0000000140001010: jmp         ?capabilities@Device@rojoRHI@@QEBAAEBUCapabilities@2@XZ
?capabilities@Device@rojoRHI@@QEBAAEBUCapabilities@2@XZ:
  0000000140001020: mov         rax,qword ptr [rcx]
"""


class LtoInlineTests(unittest.TestCase):
    def test_inlined_body_has_no_entry_call(self) -> None:
        body = check_lto_inline.function_body(OBJDUMP_INLINED, "rojorhiLtoProbe")
        self.assertEqual(len(body), 3)
        self.assertEqual(check_lto_inline.entry_calls(body), [])

    def test_objdump_call_is_reported(self) -> None:
        body = check_lto_inline.function_body(OBJDUMP_CALLED, "rojorhiLtoProbe")
        self.assertEqual(len(body), 3)
        self.assertEqual(len(check_lto_inline.entry_calls(body)), 1)

    def test_dumpbin_tail_call_is_reported_only_inside_the_probe(self) -> None:
        body = check_lto_inline.function_body(DUMPBIN_TAIL_CALLED, "rojorhiLtoProbe")
        self.assertEqual(len(body), 1)
        self.assertIn("jmp", check_lto_inline.entry_calls(body)[0])

    def test_missing_function_has_no_body(self) -> None:
        self.assertEqual(check_lto_inline.function_body(OBJDUMP_INLINED, "other"), [])


if __name__ == "__main__":
    unittest.main()
