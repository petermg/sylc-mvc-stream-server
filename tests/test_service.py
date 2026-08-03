#!/usr/bin/env python3
from __future__ import annotations
import importlib.util
import dataclasses
import os
import shutil
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("sylc_server", ROOT / "app" / "server.py")
assert SPEC and SPEC.loader
server = importlib.util.module_from_spec(SPEC)
import sys
sys.modules[SPEC.name] = server
SPEC.loader.exec_module(server)


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="sylc-phase6-test-"))
        self.media = self.tmp / "media"; self.media.mkdir()
        self.source = self.media / "Movie 3D.mkv"; self.source.write_bytes(b"not-real-mkv")
        self.phase5 = self.tmp / "phase5"; self.phase5.mkdir()
        shutil.copy2(ROOT / "tests" / "fake-phase5.sh", self.phase5 / "run-phase5.sh")
        (self.phase5 / "run-phase5.sh").chmod(0o755)
        (self.phase5 / "build").mkdir()
        binary = self.phase5 / "build" / "sylc_hsbs_pipe"
        binary.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        binary.chmod(0o755)
        iso_binary = self.phase5 / "build" / "sylc_iso_source"
        iso_binary.write_text(
            "#!/usr/bin/env python3\n"
            "import json,sys\n"
            "args=sys.argv[1:]\n"
            "if '--probe' in args:\n"
            " print(json.dumps({'ok':True,'sourceType':'bluray-iso','playlist':'00001.mpls','selectionMethod':'libbluray-titles-all','durationSeconds':5400.0,'width':1920,'height':1080,'fps':24000/1001,'hasMVC':True,'segmentCount':2,'decoysFiltered':1,'libblurayAuthoritative':True,'libblurayVersion':'1.3.4','selectedTitleIndex':3,'titleCount':8,'mainTitleHint':3,'audioTracks':[{'index':0,'format':'dts','profile':'DTS-HD MA','language':'eng','channels':6,'sampleRate':48000,'supported':True},{'index':1,'format':'ac3','profile':'AC-3','language':'spa','channels':6,'sampleRate':48000,'supported':True},{'index':2,'format':'truehd','profile':'Dolby TrueHD','language':'eng','channels':8,'sampleRate':48000,'supported':True,'decodePath':'native TrueHD extraction and FFmpeg lossless decode','truehdMajorSync':True,'embeddedAc3Core':True}]}))\n"
            "elif '--plan-video-seek' in args:\n"
            " print(json.dumps({'ok':True,'requestedStartSeconds':0,'actualDemuxStartSeconds':0,'firstOutputSeconds':0,'skipPairs':0,'firstClip':'00001','seekDetail':'fake'}))\n"
            "else:\n"
            " sys.exit(0)\n",
            encoding="utf-8",
        )
        iso_binary.chmod(0o755)
        wrapper = self.phase5 / "run-phase5.sh"
        self.config = server.Config(
            bind_host="127.0.0.1", port=18097, media_roots=(self.media.resolve(),),
            state_root=self.tmp / "state", phase5_project=self.phase5,
            phase5_runner=self.phase5 / "run-phase5.sh", phase5_wrapper=wrapper,
            legacy_work_root=self.tmp / "legacy", legacy_report_path=self.tmp / "legacy-report.txt",
            minimum_ready_segments=2, scan_cache_seconds=0, max_media_results=20,
            session_ttl_hours=24, stop_grace_seconds=1.0,
            startup_timeout_seconds=60.0, stall_timeout_seconds=30.0,
            cleanup_interval_seconds=0.0, minimum_free_gib=0.0, emergency_free_gib=0.0,
        )
        self.catalog = server.MediaCatalog(self.config)
        self.manager = server.SessionManager(self.config, self.catalog)
        self.manager._probe_source = lambda source: {"duration": 7200.0, "fps": 24.0}

    def tearDown(self):
        current = self.manager.current()
        if current and current.process and current.process.poll() is None:
            self.manager.stop(); current.thread.join(timeout=3)
        self.manager.shutdown()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_catalog_and_containment(self):
        items = self.catalog.scan(force=True)
        self.assertEqual(len(items), 1)
        resolved, metadata = self.catalog.resolve(items[0]["id"])
        self.assertEqual(resolved, self.source.resolve())
        self.assertEqual(metadata["relativePath"], "Movie 3D.mkv")

    def test_session_becomes_playable_and_completes(self):
        media_id = self.catalog.scan(force=True)[0]["id"]
        session = self.manager.create({"mediaId": media_id, "mode": "half-sbs", "startSeconds": 0})
        deadline = time.time() + 8
        saw_playable = False
        while time.time() < deadline:
            if session.playable: saw_playable = True
            if session.state in {"completed", "failed", "stopped"}: break
            time.sleep(0.05)
        session.thread.join(timeout=2)
        self.assertTrue(saw_playable)
        self.assertEqual(session.state, "completed", session.last_error)
        self.assertTrue(session.source_verified)
        self.assertGreaterEqual(session.segment_count, 4)
        self.assertEqual(session.pair_count, 192)
        self.assertTrue(session.report_path.is_file())
        playlist = self.manager.playlist_bytes(session.id).decode("utf-8")
        self.assertIn("segment-003.ts", playlist)



    def test_media_probe_returns_full_duration(self):
        media_id = self.catalog.scan(force=True)[0]["id"]
        info = self.manager.inspect_media(media_id)
        self.assertEqual(info["durationSeconds"], 7200.0)
        self.assertEqual(info["fps"], 24.0)

    def test_seek_replaces_session_and_preserves_old_hls(self):
        media_id = self.catalog.scan(force=True)[0]["id"]
        first = self.manager.create({"mediaId": media_id, "startSeconds": 0, "mode": "full-sbs", "swapEyes": True})
        deadline = time.time() + 8
        while time.time() < deadline and first.state not in {"completed", "failed", "stopped"}:
            time.sleep(0.05)
        first.thread.join(timeout=2)
        self.assertEqual(first.state, "completed", first.last_error)
        old_playlist = self.manager.playlist_bytes(first.id)

        previous, replacement = self.manager.seek({"startSeconds": 300.0})
        self.assertIs(previous, first)
        self.assertNotEqual(replacement.id, first.id)
        self.assertEqual(replacement.replacement_of, first.id)
        self.assertEqual(replacement.requested_start_seconds, 300.0)
        self.assertEqual(replacement.output_mode, "full-sbs")
        self.assertTrue(replacement.swap_eyes)
        self.assertEqual(self.manager.playlist_bytes(first.id), old_playlist)

        deadline = time.time() + 8
        while time.time() < deadline and replacement.state not in {"completed", "failed", "stopped"}:
            time.sleep(0.05)
        replacement.thread.join(timeout=2)
        self.assertEqual(replacement.state, "completed", replacement.last_error)
        self.assertEqual(replacement.public(self.config)["replacesSessionId"], first.id)
        self.assertIn("Replacement of session: " + first.id, replacement.report_path.read_text(encoding="utf-8"))

    def test_seek_stops_an_active_conversion_before_replacement(self):
        runner = self.phase5 / "slow-runner.sh"
        runner.write_text(
            "#!/usr/bin/env bash\n"
            "set -Eeuo pipefail\n"
            "source_path=${1:?}\n"
            "mkdir -p \"${SYLC_WORK_ROOT:?}\" \"${SYLC_HLS_DIR:?}\"\n"
            "printf 'Sample: %s\\n' \"$source_path\" > \"${SYLC_REPORT:?}\"\n"
            "printf x > \"$SYLC_HLS_DIR/segment-000.ts\"\n"
            "cat > \"$SYLC_HLS_DIR/stream.m3u8\" <<'EOF'\n"
            "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\nsegment-000.ts\nEOF\n"
            "echo 'PROGRESS pairs=24 emitted_pairs=24 skipped_pairs=0 pair_fps=24.0 realtime_x=1.0 base_only=0 mvc_only=0 poc_mismatches=0 errors=0'\n"
            "sleep 30\n",
            encoding="utf-8",
        )
        runner.chmod(0o755)
        config = dataclasses.replace(
            self.config, phase5_runner=runner, phase5_wrapper=runner,
            minimum_ready_segments=1, stop_grace_seconds=0.5,
        )
        manager = server.SessionManager(config, self.catalog)
        manager._probe_source = lambda source: {"duration": 7200.0, "fps": 24.0}
        media_id = self.catalog.scan(force=True)[0]["id"]
        first = manager.create({"mediaId": media_id})
        deadline = time.time() + 4
        while time.time() < deadline and not first.playable:
            time.sleep(0.05)
        self.assertTrue(first.playable)
        previous, replacement = manager.seek({"startSeconds": 600})
        self.assertIs(previous, first)
        self.assertEqual(previous.state, "stopped")
        self.assertEqual(replacement.replacement_of, first.id)
        self.assertEqual(replacement.requested_start_seconds, 600.0)
        manager.stop()
        replacement.thread.join(timeout=3)
        manager.shutdown()

    def test_start_at_is_persisted_and_passed_to_runner(self):
        media_id = self.catalog.scan(force=True)[0]["id"]
        session = self.manager.create({"mediaId": media_id, "startSeconds": 123.5})
        deadline = time.time() + 8
        while time.time() < deadline and session.state not in {"completed", "failed", "stopped"}:
            time.sleep(0.05)
        session.thread.join(timeout=2)
        self.assertEqual(session.state, "completed", session.last_error)
        self.assertEqual(session.requested_start_seconds, 123.5)
        public = session.public(self.config)
        self.assertEqual(public["requestedStartSeconds"], 123.5)
        self.assertEqual(public["sourceDurationSeconds"], 7200.0)
        self.assertIn("Requested start: 123.500000 s", session.report_path.read_text(encoding="utf-8"))

    def test_rejects_start_at_or_beyond_duration(self):
        media_id = self.catalog.scan(force=True)[0]["id"]
        with self.assertRaises(server.ApiError):
            self.manager.create({"mediaId": media_id, "startSeconds": 7200})

    def test_all_output_modes_and_eye_swap_are_passed_to_runner(self):
        media_id = self.catalog.scan(force=True)[0]["id"]
        for index, mode in enumerate(server.OUTPUT_MODES):
            swap = bool(index % 2)
            session = self.manager.create({"mediaId": media_id, "mode": mode, "swapEyes": swap})
            deadline = time.time() + 8
            while time.time() < deadline and session.state not in {"completed", "failed", "stopped"}:
                time.sleep(0.05)
            session.thread.join(timeout=2)
            self.assertEqual(session.state, "completed", session.last_error)
            public = session.public(self.config)
            self.assertEqual(public["outputMode"], mode)
            self.assertEqual(public["swapEyes"], swap)
            report = session.report_path.read_text(encoding="utf-8")
            self.assertIn(f"Output mode: {mode}", report)
            self.assertIn(f"Swap eyes: {1 if swap else 0}", report)

    def test_seek_can_change_output_mode_and_eye_order(self):
        media_id = self.catalog.scan(force=True)[0]["id"]
        first = self.manager.create({"mediaId": media_id, "mode": "half-sbs", "swapEyes": False})
        deadline = time.time() + 8
        while time.time() < deadline and first.state not in {"completed", "failed", "stopped"}:
            time.sleep(0.05)
        first.thread.join(timeout=2)
        previous, replacement = self.manager.seek({"startSeconds": 90, "mode": "anaglyph-dubois", "swapEyes": True})
        self.assertIs(previous, first)
        self.assertEqual(replacement.output_mode, "anaglyph-dubois")
        self.assertTrue(replacement.swap_eyes)
        replacement.thread.join(timeout=8)
        self.assertEqual(replacement.state, "completed", replacement.last_error)

    def test_rejects_invalid_output_options(self):
        media_id = self.catalog.scan(force=True)[0]["id"]
        with self.assertRaises(server.ApiError):
            self.manager.create({"mediaId": media_id, "mode": "over-under"})
        with self.assertRaises(server.ApiError):
            self.manager.create({"mediaId": media_id, "swapEyes": "yes"})
        with self.assertRaises(server.ApiError):
            self.manager.create({"mediaId": media_id, "audioStream": 1})

    def test_iso_probe_audio_selection_and_seek_preservation(self):
        iso = self.media / "Movie 3D.iso"
        iso.write_bytes(b"fake-udf-image")
        iso_manager = server.SessionManager(self.config, self.catalog)
        try:
            items = self.catalog.scan(force=True)
            iso_item = next(item for item in items if item["type"] == "iso")
            info = iso_manager.inspect_media(iso_item["id"])
            self.assertEqual(info["sourceType"], "bluray-iso")
            self.assertEqual(info["playlist"], "00001.mpls")
            self.assertTrue(info["libblurayAuthoritative"])
            self.assertEqual(len(info["audioTracks"]), 3)
            self.assertEqual(info["audioTracks"][2]["format"], "truehd")
            self.assertTrue(info["audioTracks"][2]["supported"])
            self.assertTrue(info["audioTracks"][2]["truehdMajorSync"])

            session = iso_manager.create({"mediaId": iso_item["id"], "audioStream": 1, "mode": "anaglyph-dubois"})
            deadline = time.time() + 8
            while time.time() < deadline and session.state not in {"completed", "failed", "stopped"}:
                time.sleep(0.05)
            session.thread.join(timeout=2)
            self.assertEqual(session.state, "completed", session.last_error)
            self.assertEqual(session.audio_stream, 1)
            self.assertEqual(session.public(self.config)["audioStream"], 1)
            self.assertIn("Audio stream: 1", session.report_path.read_text(encoding="utf-8"))

            previous, replacement = iso_manager.seek({"startSeconds": 600, "audioStream": 0})
            self.assertIs(previous, session)
            self.assertEqual(replacement.audio_stream, 0)
            replacement.thread.join(timeout=8)
            self.assertEqual(replacement.state, "completed", replacement.last_error)

            truehd = iso_manager.create({"mediaId": iso_item["id"], "audioStream": 2})
            truehd.thread.join(timeout=8)
            self.assertEqual(truehd.state, "completed", truehd.last_error)
            self.assertEqual(truehd.audio_stream, 2)

            with self.assertRaises(server.ApiError):
                iso_manager.create({"mediaId": iso_item["id"], "audioStream": 3})
        finally:
            current = iso_manager.current()
            if current and current.process and current.process.poll() is None:
                iso_manager.stop(); current.thread.join(timeout=3)
            iso_manager.shutdown()


    def test_stall_watchdog_fails_instead_of_buffering_forever(self):
        runner = self.phase5 / "stall-runner.sh"
        runner.write_text(
            "#!/usr/bin/env bash\n"
            "set -Eeuo pipefail\n"
            "source_path=${1:?}\n"
            "mkdir -p \"${SYLC_WORK_ROOT:?}\" \"${SYLC_HLS_DIR:?}\"\n"
            "printf 'Sample: %s\\n' \"$source_path\" > \"${SYLC_REPORT:?}\"\n"
            "echo 'PROGRESS pairs=14 pair_fps=17.207 realtime_x=0.718 base_only=0 mvc_only=0 poc_mismatches=0 errors=0'\n"
            "sleep 30\n",
            encoding="utf-8",
        )
        runner.chmod(0o755)
        config = dataclasses.replace(
            self.config,
            phase5_runner=runner,
            phase5_wrapper=runner,
            startup_timeout_seconds=2.0,
            stall_timeout_seconds=0.5,
        )
        manager = server.SessionManager(config, self.catalog)
        manager._probe_source = lambda source: {"duration": 7200.0, "fps": 24.0}
        media_id = self.catalog.scan(force=True)[0]["id"]
        session = manager.create({"mediaId": media_id})
        deadline = time.time() + 5
        while time.time() < deadline and session.state not in {"completed", "failed", "stopped"}:
            time.sleep(0.05)
        session.thread.join(timeout=2)
        self.assertEqual(session.state, "failed")
        self.assertTrue(session.stop_requested)
        self.assertIn("stall watchdog", session.last_error or "")
        self.assertIn("pair 14", session.last_error or "")
        self.assertIsNotNone(session.exit_code)
        manager.shutdown()

    def test_stale_legacy_outputs_are_ignored(self):
        legacy_hls = self.config.legacy_work_root / "hls"
        legacy_hls.mkdir(parents=True)
        (legacy_hls / "segment-000.ts").write_bytes(b"stale")
        (legacy_hls / "stream.m3u8").write_text("#EXTM3U\n#EXTINF:2,\nsegment-000.ts\n", encoding="utf-8")
        self.config.legacy_report_path.write_text("Sample: /old/wrong.mkv\n", encoding="utf-8")
        media_id = self.catalog.scan(force=True)[0]["id"]
        session = self.manager.create({"mediaId": media_id})
        deadline = time.time() + 8
        while time.time() < deadline and session.state not in {"completed", "failed", "stopped"}:
            time.sleep(0.05)
        session.thread.join(timeout=2)
        self.assertEqual(session.state, "completed", session.last_error)
        self.assertFalse((session.session_dir / "preexisting-phase5-report.txt").exists())
        self.assertNotEqual((session.hls_dir / "segment-000.ts").read_bytes(), b"stale")
        self.assertIn(str(self.source.resolve()), session.report_path.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
