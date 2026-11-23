"""Generator contract and real C++ static-validation tests, using only Python's standard library."""

import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


def declaration():
    return {"format_version": 1, "types": [{
        "id": "0x1071", "cpp_type": "::Fixture", "name": "Game.Fixture", "version": 1,
        "kind": "COMPONENT", "header": "fixture.h",
        "fields": [{"id": 13, "name": "X", "member": "X", "cpp_type": "int"}]}]}


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.schema = self.root / "input.json"
        self.paths = [self.root / "metadata.gen.h", self.root / "metadata.gen.cpp", self.root / "manifest.json"]
        (self.root / "fixture.h").write_text("#pragma once\nstruct Fixture { int X = 0; static int Static; };\n")

    def run_generator(self, data=None, extra_schemas=(), output=None, success=True):
        if data is not None:
            self.schema.write_text(json.dumps(data), encoding="utf-8")
        paths = self.paths if output is None else [output / path.name for path in self.paths]
        command = [sys.executable, str(OPTIONS.generator), "--schema", str(self.schema)]
        for schema in extra_schemas:
            command += ["--schema", str(schema)]
        command += ["--function", "NFixture::Register", "--header", str(paths[0]),
                    "--source", str(paths[1]), "--manifest", str(paths[2])]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result, paths

    def compile(self, source):
        return subprocess.run([str(OPTIONS.compiler), "-std=c++23", "-fsyntax-only", "-I", str(OPTIONS.include),
                               "-I", str(self.root), str(source)], capture_output=True, text=True)

    def test_deterministic_relocated_reordered_and_unchanged_outputs(self):
        data = declaration()
        second = copy.deepcopy(data["types"][0])
        second.update(id=9000, cpp_type="::Other", name="Game.Other", fields=[])
        data["types"].append(second)
        self.run_generator(data)
        contents = [path.read_bytes() for path in self.paths]
        times = [path.stat().st_mtime_ns for path in self.paths]
        data["types"].reverse()
        self.run_generator(data)
        self.assertEqual(contents, [path.read_bytes() for path in self.paths])
        self.assertEqual(times, [path.stat().st_mtime_ns for path in self.paths])
        _, relocated = self.run_generator(data, output=self.root / "relocated")
        self.assertEqual(contents, [path.read_bytes() for path in relocated])
        self.assertNotIn(str(self.root), self.paths[2].read_text())

    def test_renamed_member_preserves_explicit_property_id(self):
        self.run_generator(declaration())
        before = json.loads(self.paths[2].read_text())["types"][0]["fields"][0]
        renamed = declaration()
        renamed["types"][0]["fields"][0].update(member="Horizontal", name="Horizontal")
        self.run_generator(renamed)
        after = json.loads(self.paths[2].read_text())["types"][0]["fields"][0]
        self.assertEqual(before["id"], after["id"])
        self.assertNotEqual(before["member"], after["member"])
        (self.root / "fixture.h").write_text("#pragma once\nstruct Fixture { int Horizontal = 0; };\n")
        compiled = self.compile(self.paths[1])
        self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_duplicate_type_and_field_ids_names_or_members_are_rejected(self):
        for key in ("id", "name", "cpp_type"):
            with self.subTest(type_key=key):
                data = declaration()
                other = copy.deepcopy(data["types"][0])
                other.update(id=9, name="Other", cpp_type="::Other")
                other[key] = data["types"][0][key]
                data["types"].append(other)
                result, _ = self.run_generator(data, success=False)
                self.assertIn("duplicate type", result.stderr)
        for key in ("id", "name", "member"):
            with self.subTest(field_key=key):
                data = declaration()
                other = {"id": 14, "name": "Y", "member": "Y", "cpp_type": "float"}
                other[key] = data["types"][0]["fields"][0][key]
                data["types"][0]["fields"].append(other)
                result, _ = self.run_generator(data, success=False)
                self.assertIn("duplicate field", result.stderr)

    def test_cross_schema_duplicates_are_rejected_before_outputs_change(self):
        self.run_generator(declaration())
        before = [path.read_bytes() for path in self.paths]
        other = self.root / "another.json"
        other.write_text(json.dumps(declaration()))
        self.run_generator(extra_schemas=[other], success=False)
        self.assertEqual(before, [path.read_bytes() for path in self.paths])

    def test_id_bounds_version_unknown_fields_and_header_traversal(self):
        for value in (0, -1, 2**64, True, "automatic"):
            data = declaration()
            data["types"][0]["id"] = value
            self.run_generator(data, success=False)
        for update in ({"version": 0}, {"version": 2**32}, {"header": "../outside.h"},
                       {"header": "/absolute.h"}, {"kind": "UNKNOWN"}, {"typo": 3}):
            data = declaration()
            data["types"][0].update(update)
            self.run_generator(data, success=False)
        data = declaration()
        data["types"][0]["fields"][0]["id"] = 0
        self.run_generator(data, success=False)
        self.assertFalse(any(path.exists() for path in self.paths))

    def test_duplicate_json_keys_and_malformed_input(self):
        for content in ('{"format_version":1,"types":[],"types":[]}', '{'):
            self.schema.write_text(content)
            self.run_generator(success=False)

    def test_real_cpp_compile_accepts_valid_schema_and_rejects_type_drift(self):
        self.run_generator(declaration())
        compiled = self.compile(self.paths[1])
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        data = declaration()
        data["types"][0]["fields"][0]["cpp_type"] = "float"
        self.run_generator(data)
        compiled = self.compile(self.paths[1])
        self.assertNotEqual(compiled.returncode, 0)
        self.assertIn("metadata field type mismatch", compiled.stderr)

    def test_static_members_are_not_instance_metadata(self):
        data = declaration()
        data["types"][0]["fields"][0]["member"] = "Static"
        self.run_generator(data)
        compiled = self.compile(self.paths[1])
        self.assertNotEqual(compiled.returncode, 0)
        self.assertIn("metadata requires a non-static data member", compiled.stderr)

    def test_empty_schema_is_valid(self):
        self.run_generator({"format_version": 1, "types": []})
        compiled = self.compile(self.paths[1])
        self.assertEqual(compiled.returncode, 0, compiled.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", required=True, type=Path)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--include", required=True, type=Path)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0]] + remaining)
