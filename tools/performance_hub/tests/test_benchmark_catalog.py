import json
import os
from pathlib import Path
import stat
import tempfile
import time
import unittest

from tools.performance_hub.benchmark_catalog import build_catalog, exact_filter, validate_selection


class BenchmarkCatalogTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.repo = Path(self.directory.name)
        (self.repo / "CMakePresets.json").write_text(json.dumps({
            "configurePresets": [{"name": "benchmark", "binaryDir": "${sourceDir}/cmake-build-${presetName}"}],
            "buildPresets": [{"name": "benchmark", "configurePreset": "benchmark"}],
        }))
        self.binary = self.repo / "cmake-build-benchmark" / "GraphicsEngineBenchmarks"
        self.binary.parent.mkdir()

    def tearDown(self):
        self.directory.cleanup()

    def write_fixture(self, body, exit_code=0):
        self.binary.write_text("#!/bin/sh\n" + body + f"\nexit {exit_code}\n")
        self.binary.chmod(self.binary.stat().st_mode | stat.S_IXUSR)

    def test_reports_not_built_and_unsupported_profiles(self):
        missing = build_catalog(self.repo, "benchmark")
        unsupported = build_catalog(self.repo, "other")
        self.assertEqual("not_built", missing["status"]["state"])
        self.assertEqual("unsupported", unsupported["status"]["state"])

    def test_catalogues_parameterized_fixture_and_escapes_exact_selection(self):
        names = ["BM_Renderer/64/2", "BM_Task[fast]+/1", "BM_Other/0"]
        self.write_fixture("printf '%s\\n' " + " ".join("'" + name + "'" for name in names))
        catalog = build_catalog(self.repo, "benchmark")
        expression = exact_filter(names[:2])
        self.assertEqual("ready", catalog["status"]["state"])
        self.assertEqual(names, [case["name"] for case in catalog["cases"]])
        self.assertTrue(__import__("re").fullmatch(expression, names[0]))
        self.assertFalse(__import__("re").fullmatch(expression, names[2]))
        self.assertEqual("case:BM_Task[fast]+/1", catalog["cases"][1]["id"])

    def test_reports_stale_and_list_errors(self):
        self.write_fixture("echo broken >&2", exit_code=4)
        failed = build_catalog(self.repo, "benchmark")
        self.assertEqual("error", failed["status"]["state"])
        self.assertEqual("list_tests_failed", failed["status"]["diagnostic"]["code"])
        cache = self.binary.parent / "CMakeCache.txt"
        time.sleep(0.01)
        cache.write_text("configured")
        stale = build_catalog(self.repo, "benchmark")
        self.assertEqual("stale", stale["status"]["state"])

    def test_rebuilt_binary_changes_snapshot_identity(self):
        self.write_fixture("echo BM_First")
        first = build_catalog(self.repo, "benchmark")
        time.sleep(0.01)
        self.write_fixture("echo BM_Second")
        second = build_catalog(self.repo, "benchmark")
        self.assertNotEqual(first["catalogVersion"], second["catalogVersion"])
        self.assertNotEqual(first["binary"]["fingerprint"], second["binary"]["fingerprint"])

    def test_selection_rejects_names_absent_from_the_catalog(self):
        self.write_fixture("echo 'BM_Only/1'")
        catalog = build_catalog(self.repo, "benchmark")
        self.assertEqual("^(?:BM_Only/1)$", validate_selection(catalog, ["BM_Only/1"]))
        with self.assertRaisesRegex(ValueError, "unknown benchmark case"):
            validate_selection(catalog, ["BM_Outside"])


if __name__ == "__main__":
    unittest.main()
