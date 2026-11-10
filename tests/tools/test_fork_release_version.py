import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "fork-release-version.py"
SPEC = importlib.util.spec_from_file_location("fork_release_version", SCRIPT)
release_version = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release_version)


def release(tag, sha, **extra):
    return dict(dict(tag_name=tag, target_commitish=sha, draft=False, prerelease=False), **extra)


class ForkReleaseVersionTests(unittest.TestCase):
    def test_removed_upstream_release_cannot_lower_ancestral_fork_lineage(self):
        # Actual API metadata after upstream removed the 1103 release.
        upstream = [
            release("v2026.1107.122135.杂鱼", "79423092eda6ccd011e58d04236c8f16e02765c5"),
            release("v2026.1031.121458.杂鱼", "204a96826606b3a34cf2cd813aed0f0a9bba7db0"),
        ]
        fork = [release("v2026.1103.100831.14.杂鱼", "1197353909f8237c43f9c1893900caa3cd420c69")]
        ancestors = {upstream[1]["target_commitish"], fork[0]["target_commitish"]}
        self.assertEqual(release_version.select(
            upstream, fork, fork[0]["tag_name"], 17, ancestors.__contains__),
            "v2026.1103.100831.17")

    def test_selection_uses_maximum_not_api_order(self):
        items = [release("v2026.1031.121458", "a" * 40), release("v2026.1103.100831.杂鱼", "b" * 40)]
        self.assertEqual(release_version.select(items, [], "v2026.1031.121458.14", 17, lambda _: True),
                         "v2026.1103.100831.17")

    def test_selection_itself_fails_when_live_latest_is_newer(self):
        with self.assertRaisesRegex(ValueError, "strictly greater"):
            release_version.select([release("v2026.1031.121458", "a" * 40)], [],
                                   "v2026.1103.100831.14", 17, lambda _: True)

    def test_prepublication_gate_rejects_regression_equality_and_race(self):
        for latest in ("v2026.1103.100831.14.杂鱼", "v2026.1031.121458.17",
                       "v2026.1107.122135.18"):
            with self.subTest(latest=latest), self.assertRaises(ValueError):
                release_version.require_newer("v2026.1031.121458.17", latest)

    def test_numeric_comparison_and_suffix(self):
        release_version.require_newer("v2026.1103.100831.17", "v2026.1103.100831.14.杂鱼")
        release_version.require_newer("v2026.1103.100831.100", "v2026.1103.100831.99")

    def test_unknown_latest_and_no_reachable_base_fail_closed(self):
        for latest in ("nightly", "v2026.1103.100831"):
            with self.subTest(latest=latest), self.assertRaises(ValueError):
                release_version.require_newer("v2026.1103.100831.17", latest)
        with self.assertRaises(ValueError):
            release_version.select([], [], "v2026.1103.100831.14", 17, lambda _: False)

    def test_only_stable_reachable_correct_shape_release_commits_supply_lineage(self):
        fork = [
            release("v2026.1103.100831.14", "a" * 40),
            release("v9999.1.1.1", "b" * 40, draft=True),
            release("v9998.1.1.1", "b" * 40, prerelease=True),
            release("v9997.1.1.1", "b" * 40),
            release("v9996.1.1.1", "integration"),
            release("v9995.1.1", "a" * 40),
            release("nightly", "a" * 40),
        ]
        self.assertEqual(release_version.select([], fork, fork[0]["tag_name"], 17,
                                               lambda sha: sha == "a" * 40),
                         "v2026.1103.100831.17")
        for run in (0, -1):
            with self.assertRaises(ValueError):
                release_version.select([], fork, fork[0]["tag_name"], run, lambda _: True)

    def test_setup_and_prepublish_workflow_gates_are_wired(self):
        workflow = (SCRIPT.parents[1] / ".github" / "workflows" / "main.yml").read_text(encoding="utf-8")
        setup = workflow[workflow.index("- name: Setup Release (Fork)"):workflow.index("- name: Filter bot contributors")]
        publish = workflow[workflow.index("  publish_fork_release:"):]
        self.assertIn("--paginate --slurp", setup)
        self.assertIn("scripts/fork-release-version.py", setup)
        self.assertIn("--run-number", setup)
        self.assertIn("--candidate", publish)
        self.assertLess(publish.index("--candidate"), publish.index("gh release create"))
        self.assertIn('releases/latest', publish)
        self.assertIn("actions/checkout@", publish)

    def test_paginated_results_are_all_considered(self):
        pages = [[release("v2026.1031.121458", "a" * 40)],
                 [release("v2026.1103.100831", "b" * 40)]]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pages.json"
            path.write_text(json.dumps(pages), encoding="utf-8")
            loaded = release_version.read_releases(path)
        self.assertEqual(len(loaded), 2)
        self.assertEqual(release_version.select(loaded, [], "v2026.1031.121458.14", 17, lambda _: True),
                         "v2026.1103.100831.17")


if __name__ == "__main__":
    unittest.main()
