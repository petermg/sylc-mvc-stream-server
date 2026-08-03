#!/usr/bin/env python3
from __future__ import annotations
import dataclasses
import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("sylc_server_public", ROOT / "app" / "server.py")
assert SPEC and SPEC.loader
server = importlib.util.module_from_spec(SPEC)
import sys
sys.modules[SPEC.name] = server
SPEC.loader.exec_module(server)


class PublicAlphaSettingsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="sylc-public-alpha-"))
        self.media1 = self.tmp / "Movies A"; self.media1.mkdir()
        self.media2 = self.tmp / "Movies B"; self.media2.mkdir()
        (self.media1 / "One.mkv").write_bytes(b"x")
        (self.media2 / "Two.iso").write_bytes(b"x")
        phase = self.tmp / "engine"; (phase / "build").mkdir(parents=True)
        for name in ("sylc_hsbs_pipe", "sylc_iso_source"):
            path = phase / "build" / name; path.write_text("#!/bin/sh\nexit 0\n"); path.chmod(0o755)
        runner = self.tmp / "runner.sh"; runner.write_text("#!/bin/sh\nexit 0\n"); runner.chmod(0o755)
        self.config = server.Config(
            bind_host="127.0.0.1", port=18097, media_roots=(),
            state_root=self.tmp / "state", phase5_project=phase,
            phase5_runner=runner, phase5_wrapper=runner,
            legacy_work_root=self.tmp / "legacy", legacy_report_path=self.tmp / "legacy.txt",
            minimum_ready_segments=1, scan_cache_seconds=0, max_media_results=100,
            session_ttl_hours=1, stop_grace_seconds=.5, startup_timeout_seconds=5,
            stall_timeout_seconds=5, cleanup_interval_seconds=0,
            minimum_free_gib=0, emergency_free_gib=0,
            config_file=self.tmp / "config.json", browse_roots=(self.tmp,),
        )

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_first_run_setup_token_and_persistence(self):
        settings = server.SettingsStore(self.config)
        self.assertTrue(settings.setup_required())
        library = settings.initial_setup({
            "name": "Main 3D", "path": str(self.media1), "recursive": True,
            "enabled": True, "apiToken": "correct-horse",
        })
        self.assertFalse(settings.setup_required())
        self.assertTrue(settings.token_configured())
        self.assertTrue(settings.authenticate("correct-horse"))
        self.assertFalse(settings.authenticate("wrong"))
        self.assertEqual(library.path, self.media1.resolve())
        reloaded = server.SettingsStore(self.config)
        self.assertEqual(reloaded.libraries()[0].name, "Main 3D")
        self.assertTrue(reloaded.authenticate("correct-horse"))

    def test_multiple_libraries_stable_ids_and_non_destructive_remove(self):
        settings = server.SettingsStore(self.config)
        first = settings.initial_setup({"name":"A", "path":str(self.media1), "apiToken":""})
        second = settings.create({"name":"B", "path":str(self.media2), "recursive":False})
        catalog = server.MediaCatalog(self.config, settings)
        items = catalog.scan(force=True)
        self.assertEqual({item["libraryName"] for item in items}, {"A", "B"})
        item_b = next(item for item in items if item["libraryId"] == second.id)
        path, metadata = catalog.resolve(item_b["id"])
        self.assertEqual(path.name, "Two.iso")
        self.assertEqual(metadata["libraryId"], second.id)
        settings.delete(first.id)
        catalog.invalidate()
        self.assertTrue((self.media1 / "One.mkv").is_file())
        path2, _ = catalog.resolve(item_b["id"])
        self.assertEqual(path2, path)

    def test_service_folder_browser_and_validation(self):
        app = server.ServiceApp(self.config)
        try:
            roots = app.browse_directories(None)
            self.assertEqual(roots["directories"][0]["path"], str(self.tmp.resolve()))
            children = app.browse_directories(str(self.tmp))
            self.assertIn("Movies A", {entry["name"] for entry in children["directories"]})
            tested = app.test_library({"path": str(self.media1), "recursive": True})
            self.assertEqual(tested["counts"]["mkv"], 1)
            with self.assertRaises(server.ApiError):
                app.test_library({"path": "relative/path"})
        finally:
            app.sessions.shutdown()

    def test_bootstrap_env_roots_migrate_into_config(self):
        config = dataclasses.replace(self.config, media_roots=(self.media1.resolve(),))
        settings = server.SettingsStore(config)
        self.assertFalse(settings.setup_required())
        self.assertEqual(settings.libraries()[0].path, self.media1.resolve())
        self.assertTrue(config.config_file.is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
