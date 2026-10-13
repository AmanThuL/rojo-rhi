from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from Tools import shader_layout as layout


def scalar(kind: str = "uint32") -> dict:
    return {"kind": "scalar", "scalarType": kind}


def vector(count: int, kind: str = "uint32") -> dict:
    return {"kind": "vector", "elementCount": count, "elementType": scalar(kind)}


def field(name: str, offset: int, size: int, type_: dict | None = None) -> dict:
    binding = {"kind": "uniform", "offset": offset, "size": size, "elementStride": 0}
    return {"name": name, "type": type_ or scalar(), "binding": binding}


def buffer(name: str, size: int, fields: list[dict]) -> dict:
    result = {"kind": "struct", "name": name, "fields": fields, "sizes": [{"kind": "uniform", "value": size}]}
    return {"name": "g" + name, "type": {"kind": "resource", "baseShape": "structuredBuffer", "resultType": result}}


def probe(*parameters: dict) -> dict:
    # Module parameters that are not probe buffers sit beside them in real output and must be ignored.
    return {"parameters": [{"name": "gRootBytes", "type": {"kind": "resource"}}, *parameters]}


def record(name: str, size: int, fields: list[dict]) -> dict:
    return buffer(name, size, fields) | {"name": "gRecord_" + name}


def root(name: str, size: int, fields: list[dict]) -> dict:
    return buffer(name, size, fields) | {"name": "gRoot_" + name}


DRAW = record("DrawIndirectArgs", 8, [field("drawId", 0, 4), field("vertexCount", 4, 4)])


class ShaderLayoutTests(unittest.TestCase):
    def render(self, reflections: dict[str, dict], **options) -> str:
        return layout.generate(reflections, **options)

    def test_record_asserts_every_field_offset_and_the_size(self) -> None:
        text = self.render({"metal": probe(DRAW)})
        self.assertIn("static_assert(offsetof(rojoRHI::DrawIndirectArgs, drawId) == 0,", text)
        self.assertIn("static_assert(offsetof(rojoRHI::DrawIndirectArgs, vertexCount) == 4,", text)
        self.assertIn("static_assert(sizeof(rojoRHI::DrawIndirectArgs) == 8,", text)

    def test_messages_name_struct_field_and_target(self) -> None:
        text = self.render({"metal": probe(DRAW)})
        self.assertIn('"DrawIndirectArgs.vertexCount: offset 4 on metal"', text)
        self.assertIn('"DrawIndirectArgs: size 8 on metal"', text)

    def test_every_target_gets_its_own_asserts(self) -> None:
        text = self.render({"metal": probe(DRAW), "spirv": probe(DRAW)})
        self.assertIn("offset 4 on metal", text)
        self.assertIn("offset 4 on spirv", text)

    def test_struct_count_counts_each_struct_once(self) -> None:
        text = self.render({"metal": probe(DRAW), "spirv": probe(DRAW)})
        self.assertIn("inline constexpr int kShaderLayoutStructCount = 1;", text)

    def test_default_type_is_in_rojorhi_and_map_overrides_it(self) -> None:
        text = self.render({"metal": probe(DRAW)}, type_map={"DrawIndirectArgs": "app::Draw"})
        self.assertIn("offsetof(app::Draw, drawId)", text)
        self.assertNotIn("rojoRHI::DrawIndirectArgs", text)

    def test_includes_are_written_in_angle_form(self) -> None:
        text = self.render({"metal": probe(DRAW)}, includes=["rojoRHI/IndirectArgs.h"])
        self.assertIn("#include <rojoRHI/IndirectArgs.h>", text)

    def test_targets_that_disagree_on_the_struct_set_are_rejected(self) -> None:
        with self.assertRaisesRegex(layout.LayoutError, "DrawIndirectArgs.*spirv"):
            self.render({"metal": probe(DRAW), "spirv": probe()})

    def test_record_may_nest_structs_and_use_three_vectors(self) -> None:
        nested = record("Mesh", 32, [field("pos", 0, 12, vector(3)), field("inner", 16, 16, {"kind": "struct"})])
        self.assertIn("offsetof(rojoRHI::Mesh, inner) == 16", self.render({"metal": probe(nested)}))

    def test_valid_root_passes(self) -> None:
        ok = root("R", 16, [field("a", 0, 8, vector(2)), field("b", 8, 4), field("c", 12, 4)])
        self.assertIn("sizeof(rojoRHI::R) == 16", self.render({"metal": probe(ok)}))

    def assertRootRejected(self, parameter: dict, rule: str) -> None:
        with self.assertRaisesRegex(layout.LayoutError, rule):
            self.render({"metal": probe(parameter)})

    def test_root_with_nested_struct_is_rejected(self) -> None:
        self.assertRootRejected(root("R", 16, [field("n", 0, 16, {"kind": "struct"})]), "nested struct")

    def test_root_with_three_vector_is_rejected(self) -> None:
        self.assertRootRejected(root("R", 16, [field("v", 0, 12, vector(3)), field("p", 12, 4)]), "3-vector")

    def test_root_with_64_bit_scalar_is_rejected(self) -> None:
        self.assertRootRejected(root("R", 16, [field("l", 0, 8, scalar("uint64")), field("p", 8, 8)]), "64-bit")

    def test_root_size_not_a_multiple_of_16_is_rejected(self) -> None:
        self.assertRootRejected(root("R", 12, [field("a", 0, 4), field("b", 4, 4), field("c", 8, 4)]), "multiple of 16")

    def test_root_above_65536_bytes_is_rejected(self) -> None:
        self.assertRootRejected(root("R", 65552, [field("a", 0, 4)]), "65536")

    def test_root_member_off_its_alignment_is_rejected(self) -> None:
        self.assertRootRejected(root("R", 16, [field("a", 0, 4), field("v", 4, 8, vector(2)), field("p", 12, 4)]),
                                "aligned")

    def test_error_names_struct_field_and_target(self) -> None:
        with self.assertRaisesRegex(layout.LayoutError, "R.v.*metal"):
            self.render({"metal": probe(root("R", 16, [field("v", 0, 12, vector(3)), field("p", 12, 4)]))})

    def test_records_skip_the_root_rules(self) -> None:
        odd = record("Odd", 12, [field("v", 0, 12, vector(3))])
        self.assertIn("sizeof(rojoRHI::Odd) == 12", self.render({"metal": probe(odd)}))

    def test_main_writes_the_header(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            json_path = Path(directory) / "metal.json"
            json_path.write_text(json.dumps(probe(DRAW)))
            out = Path(directory) / "ShaderLayout.h"
            self.assertEqual(layout.main([f"metal={json_path}", "--out", str(out)]), 0)
            self.assertIn("kShaderLayoutStructCount = 1", out.read_text())


if __name__ == "__main__":
    unittest.main()
