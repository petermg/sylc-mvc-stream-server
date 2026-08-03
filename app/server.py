#!/usr/bin/env python3
"""SyLC MVC Stream Service — Phase 6.

A dependency-free LAN control plane around a direct streaming MVC MKV ->
selectable stereo-layout/anaglyph H.264 + AC-3 HLS pipeline. MVC MKV and unencrypted Blu-ray 3D ISO sources are streamed into edge264 as they
are read, so full-movie playback no longer waits for complete Annex-B extraction
or a duplicate whole-file validation pass.
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import math
import mimetypes
import os
import re
import shutil
import signal
import subprocess
import threading
import time
import traceback
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import parse_qs, unquote, urlparse

APP_VERSION = "0.7.0-alpha.2"
OUTPUT_MODES = ("half-sbs", "full-sbs", "half-ou", "full-ou", "left-eye", "right-eye", "anaglyph-color", "anaglyph-dubois", "passive-rows-left-top", "passive-rows-right-top")
APP_DIR = Path(__file__).resolve().parent
STATIC_DIR = APP_DIR / "static"


def env_int(name: str, default: int, minimum: int = 0, maximum: int = 1_000_000) -> int:
    raw = os.getenv(name, str(default)).strip()
    try:
        value = int(raw)
    except ValueError as exc:
        raise RuntimeError(f"{name} must be an integer, got {raw!r}") from exc
    if not minimum <= value <= maximum:
        raise RuntimeError(f"{name} must be between {minimum} and {maximum}")
    return value


def env_float(name: str, default: float, minimum: float = 0.0) -> float:
    raw = os.getenv(name, str(default)).strip()
    try:
        value = float(raw)
    except ValueError as exc:
        raise RuntimeError(f"{name} must be numeric, got {raw!r}") from exc
    if value < minimum:
        raise RuntimeError(f"{name} must be at least {minimum}")
    return value


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def is_relative_to(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.{os.getpid()}.{threading.get_ident()}.tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def atomic_write_json(path: Path, value: Any) -> None:
    atomic_write(path, (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8"))


@dataclass(frozen=True)
class Config:
    bind_host: str
    port: int
    # Bootstrap-only roots retained for migration and test compatibility. Runtime
    # media libraries live in config.json and can be changed from the web UI.
    media_roots: tuple[Path, ...]
    state_root: Path
    phase5_project: Path
    phase5_runner: Path
    phase5_wrapper: Path
    legacy_work_root: Path
    legacy_report_path: Path
    minimum_ready_segments: int
    scan_cache_seconds: int
    max_media_results: int
    session_ttl_hours: float
    stop_grace_seconds: float
    startup_timeout_seconds: float
    stall_timeout_seconds: float
    cleanup_interval_seconds: float
    minimum_free_gib: float
    emergency_free_gib: float
    config_file: Path | None = None
    browse_roots: tuple[Path, ...] = ()
    encoder_mode: str = "vaapi"
    vaapi_device: Path = Path("/dev/dri/renderD128")

    @classmethod
    def from_env(cls) -> "Config":
        roots_raw = os.getenv("SYLC_MEDIA_ROOTS", "")
        roots: list[Path] = []
        for raw in roots_raw.split("|"):
            raw = raw.strip()
            if raw:
                roots.append(Path(raw).expanduser().resolve(strict=False))

        browse_raw = os.getenv("SYLC_BROWSE_ROOTS", "/mnt|/media|/srv|/home")
        browse_roots: list[Path] = []
        for raw in browse_raw.split("|"):
            raw = raw.strip()
            if raw:
                browse_roots.append(Path(raw).expanduser().resolve(strict=False))

        phase5_project = Path(
            os.getenv("SYLC_ENGINE_PROJECT", "/srv/sylc-mvc-stream/engine/phase6-streaming")
        ).expanduser().resolve(strict=False)
        encoder_mode = os.getenv("SYLC_ENCODER_MODE", "vaapi").strip()
        if encoder_mode not in {"vaapi", "software"}:
            raise RuntimeError("SYLC_ENCODER_MODE must be vaapi or software")
        return cls(
            bind_host=os.getenv("SYLC_BIND_HOST", "0.0.0.0").strip(),
            port=env_int("SYLC_PORT", 8097, 1, 65535),
            media_roots=tuple(roots),
            state_root=Path(os.getenv("SYLC_STATE_ROOT", "/srv/sylc-mvc-stream/state")).expanduser().resolve(strict=False),
            phase5_project=phase5_project,
            phase5_runner=Path(os.getenv("SYLC_ENGINE_RUNNER", "/srv/sylc-mvc-stream/engine/run-phase6-streaming-session.sh")).expanduser().resolve(strict=False),
            phase5_wrapper=Path(os.getenv("SYLC_ENGINE_WRAPPER", "/srv/sylc-mvc-stream/engine/run-phase6-streaming-session.sh")).expanduser().resolve(strict=False),
            legacy_work_root=Path(os.getenv("SYLC_PHASE5_LEGACY_WORK_ROOT", "/var/cache/sylc-mvc-stream/legacy")).expanduser().resolve(strict=False),
            legacy_report_path=Path(os.getenv("SYLC_PHASE5_LEGACY_REPORT", "/var/lib/sylc-mvc-stream/legacy-report.txt")).expanduser().resolve(strict=False),
            minimum_ready_segments=env_int("SYLC_MIN_READY_SEGMENTS", 2, 1, 20),
            scan_cache_seconds=env_int("SYLC_SCAN_CACHE_SECONDS", 30, 0, 3600),
            max_media_results=env_int("SYLC_MAX_MEDIA_RESULTS", 5000, 1, 100000),
            session_ttl_hours=env_float("SYLC_SESSION_TTL_HOURS", 24.0, 0.0),
            stop_grace_seconds=env_float("SYLC_STOP_GRACE_SECONDS", 5.0, 0.5),
            startup_timeout_seconds=env_float("SYLC_STARTUP_TIMEOUT_SECONDS", 60.0, 5.0),
            stall_timeout_seconds=env_float("SYLC_STALL_TIMEOUT_SECONDS", 30.0, 5.0),
            cleanup_interval_seconds=env_float("SYLC_CLEANUP_INTERVAL_SECONDS", 3600.0, 0.0),
            minimum_free_gib=env_float("SYLC_MINIMUM_FREE_GIB", 10.0, 0.0),
            emergency_free_gib=env_float("SYLC_EMERGENCY_FREE_GIB", 5.0, 0.0),
            config_file=Path(os.getenv("SYLC_CONFIG_FILE", "/var/lib/sylc-mvc-stream/config.json")).expanduser().resolve(strict=False),
            browse_roots=tuple(browse_roots),
            encoder_mode=encoder_mode,
            vaapi_device=Path(os.getenv("SYLC_VAAPI_DEVICE", "/dev/dri/renderD128")).expanduser().resolve(strict=False),
        )


class ApiError(Exception):
    def __init__(self, status: int, message: str, detail: str | None = None):
        super().__init__(message)
        self.status = status
        self.message = message
        self.detail = detail


@dataclass(frozen=True)
class MediaLibrary:
    id: str
    name: str
    path: Path
    recursive: bool = True
    enabled: bool = True

    def public(self) -> dict[str, Any]:
        readable = self.path.is_dir() and os.access(self.path, os.R_OK | os.X_OK)
        return {
            "id": self.id,
            "name": self.name,
            "path": str(self.path),
            "recursive": self.recursive,
            "enabled": self.enabled,
            "exists": self.path.is_dir(),
            "readable": readable,
        }


class SettingsStore:
    """Mutable application settings stored outside the read-only app tree."""

    SCHEMA_VERSION = 1

    def __init__(self, config: Config):
        self.config = config
        self.path = config.config_file
        self._lock = threading.RLock()
        self._libraries: list[MediaLibrary] = []
        self._token_sha256 = ""
        self._load()

    @staticmethod
    def _hash_token(token: str) -> str:
        return hashlib.sha256(token.encode("utf-8")).hexdigest()

    @staticmethod
    def _clean_name(value: Any, fallback: str) -> str:
        name = str(value or "").strip()
        if not name:
            name = fallback
        return name[:120]

    @staticmethod
    def _canonical_directory(raw: Any, require_readable: bool = True) -> Path:
        if not isinstance(raw, str) or not raw.strip():
            raise ApiError(400, "A media-library folder is required.")
        candidate = Path(raw.strip()).expanduser()
        if not candidate.is_absolute():
            raise ApiError(400, "Media-library folders must be absolute server paths.")
        try:
            path = candidate.resolve(strict=True)
        except FileNotFoundError as exc:
            raise ApiError(400, "The selected folder does not exist.", str(candidate)) from exc
        if not path.is_dir():
            raise ApiError(400, "The selected path is not a folder.", str(path))
        if require_readable and not os.access(path, os.R_OK | os.X_OK):
            raise ApiError(403, "The SyLC service user cannot read this folder.", str(path))
        try:
            with os.scandir(path) as entries:
                next(entries, None)
        except PermissionError as exc:
            raise ApiError(403, "The SyLC service user cannot list this folder.", str(path)) from exc
        return path

    def _load(self) -> None:
        data: dict[str, Any] = {}
        if self.path and self.path.is_file():
            try:
                loaded = json.loads(self.path.read_text(encoding="utf-8"))
                if isinstance(loaded, dict):
                    data = loaded
            except (OSError, json.JSONDecodeError) as exc:
                raise RuntimeError(f"Cannot read SyLC settings from {self.path}: {exc}") from exc
        libraries: list[MediaLibrary] = []
        for value in data.get("mediaLibraries", []):
            if not isinstance(value, dict):
                continue
            try:
                library_id = str(value.get("id") or uuid.uuid4().hex[:16])
                raw_path = str(value.get("path") or "").strip()
                if not raw_path:
                    continue
                path = Path(raw_path).expanduser().resolve(strict=False)
                libraries.append(MediaLibrary(
                    id=library_id,
                    name=self._clean_name(value.get("name"), path.name or str(path)),
                    path=path,
                    recursive=bool(value.get("recursive", True)),
                    enabled=bool(value.get("enabled", True)),
                ))
            except Exception:
                continue
        if not libraries:
            for root in self.config.media_roots:
                libraries.append(MediaLibrary(
                    id=uuid.uuid4().hex[:16],
                    name=root.name or str(root),
                    path=root,
                    recursive=True,
                    enabled=True,
                ))
        self._libraries = libraries
        token_hash = data.get("apiTokenSha256", "")
        self._token_sha256 = token_hash if isinstance(token_hash, str) and len(token_hash) == 64 else ""
        if self.path and (not self.path.exists()) and (libraries or self._token_sha256):
            self._persist_locked()

    def _persist_locked(self) -> None:
        if not self.path:
            return
        payload = {
            "schemaVersion": self.SCHEMA_VERSION,
            "mediaLibraries": [
                {"id": item.id, "name": item.name, "path": str(item.path), "recursive": item.recursive, "enabled": item.enabled}
                for item in self._libraries
            ],
            "apiTokenSha256": self._token_sha256,
            "updatedAt": utc_now(),
        }
        atomic_write_json(self.path, payload)

    def setup_required(self) -> bool:
        with self._lock:
            return not self._libraries

    def token_configured(self) -> bool:
        with self._lock:
            return bool(self._token_sha256)

    def authenticate(self, token: str | None) -> bool:
        with self._lock:
            expected = self._token_sha256
        if not expected:
            return True
        supplied = self._hash_token(token or "")
        return hmac.compare_digest(expected, supplied)

    def libraries(self, enabled_only: bool = False) -> list[MediaLibrary]:
        with self._lock:
            values = list(self._libraries)
        return [item for item in values if item.enabled] if enabled_only else values

    def get(self, library_id: str) -> MediaLibrary:
        with self._lock:
            for item in self._libraries:
                if item.id == library_id:
                    return item
        raise ApiError(404, "Media library not found.")

    def create(self, body: dict[str, Any]) -> MediaLibrary:
        path = self._canonical_directory(body.get("path"))
        with self._lock:
            if any(item.path == path for item in self._libraries):
                raise ApiError(409, "That folder is already configured as a media library.")
            item = MediaLibrary(
                id=uuid.uuid4().hex[:16],
                name=self._clean_name(body.get("name"), path.name or str(path)),
                path=path,
                recursive=bool(body.get("recursive", True)),
                enabled=bool(body.get("enabled", True)),
            )
            self._libraries.append(item)
            self._persist_locked()
            return item

    def update(self, library_id: str, body: dict[str, Any]) -> MediaLibrary:
        with self._lock:
            index = next((i for i, item in enumerate(self._libraries) if item.id == library_id), None)
            if index is None:
                raise ApiError(404, "Media library not found.")
            current = self._libraries[index]
        path = current.path if "path" not in body else self._canonical_directory(body.get("path"))
        with self._lock:
            if any(item.id != library_id and item.path == path for item in self._libraries):
                raise ApiError(409, "That folder is already configured as a media library.")
            updated = MediaLibrary(
                id=current.id,
                name=self._clean_name(body.get("name", current.name), path.name or str(path)),
                path=path,
                recursive=bool(body.get("recursive", current.recursive)),
                enabled=bool(body.get("enabled", current.enabled)),
            )
            self._libraries[index] = updated
            self._persist_locked()
            return updated

    def delete(self, library_id: str) -> None:
        with self._lock:
            before = len(self._libraries)
            self._libraries = [item for item in self._libraries if item.id != library_id]
            if len(self._libraries) == before:
                raise ApiError(404, "Media library not found.")
            self._persist_locked()

    def set_token(self, token: Any) -> None:
        if token is None:
            token = ""
        if not isinstance(token, str):
            raise ApiError(400, "API token must be text.")
        token = token.strip()
        if token and len(token) < 8:
            raise ApiError(400, "API tokens must contain at least 8 characters, or be left blank to disable authentication.")
        if len(token) > 512:
            raise ApiError(400, "API token is too long.")
        with self._lock:
            self._token_sha256 = self._hash_token(token) if token else ""
            self._persist_locked()

    def initial_setup(self, body: dict[str, Any]) -> MediaLibrary:
        with self._lock:
            if self._libraries:
                raise ApiError(409, "Initial setup has already been completed.")
        item = self.create(body)
        try:
            self.set_token(body.get("apiToken", ""))
        except Exception:
            with self._lock:
                self._libraries = [lib for lib in self._libraries if lib.id != item.id]
                self._persist_locked()
            raise
        return item



class MediaCatalog:
    EXTENSIONS = {".mkv", ".mk3d", ".iso"}

    def __init__(self, config: Config, settings: SettingsStore | None = None):
        self.config = config
        self.settings = settings or SettingsStore(config)
        self._lock = threading.Lock()
        self._cached_at = 0.0
        self._cache: list[dict[str, Any]] = []
        self._library_counts: dict[str, int] = {}
        self._library_type_counts: dict[str, dict[str, int]] = {}

    @staticmethod
    def _encode_id(library_id: str, relative: str) -> str:
        raw = json.dumps([library_id, relative], separators=(",", ":")).encode("utf-8")
        return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")

    @staticmethod
    def _decode_id(media_id: str) -> tuple[str | int, str]:
        if not media_id or len(media_id) > 8192:
            raise ApiError(400, "Invalid media ID.")
        try:
            padded = media_id + "=" * (-len(media_id) % 4)
            value = json.loads(base64.urlsafe_b64decode(padded).decode("utf-8"))
            library_key, relative = value
            if not isinstance(library_key, (str, int)) or not isinstance(relative, str):
                raise ValueError("wrong media ID structure")
            return library_key, relative
        except Exception as exc:
            raise ApiError(400, "Invalid media ID.", str(exc)) from exc

    def invalidate(self) -> None:
        with self._lock:
            self._cache = []
            self._cached_at = 0.0
            self._library_counts = {}
            self._library_type_counts = {}

    def scan(self, force: bool = False) -> list[dict[str, Any]]:
        now = time.monotonic()
        with self._lock:
            if not force and self._cache and now - self._cached_at < self.config.scan_cache_seconds:
                return list(self._cache)

            items: list[dict[str, Any]] = []
            counts: dict[str, int] = {}
            type_counts: dict[str, dict[str, int]] = {}
            libraries = self.settings.libraries(enabled_only=True)
            for library in libraries:
                root = library.path
                counts[library.id] = 0
                type_counts[library.id] = {"mkv": 0, "mk3d": 0, "iso": 0}
                if not root.is_dir() or not os.access(root, os.R_OK | os.X_OK):
                    continue
                if library.recursive:
                    walkers: Iterable[tuple[str, list[str], list[str]]] = os.walk(root, followlinks=False)
                else:
                    try:
                        names = [entry.name for entry in os.scandir(root) if entry.is_file(follow_symlinks=False)]
                    except OSError:
                        continue
                    walkers = [(str(root), [], names)]
                for dirpath, dirnames, filenames in walkers:
                    # Never descend through symlinked directories.
                    dirnames[:] = [d for d in dirnames if not (Path(dirpath) / d).is_symlink()]
                    for filename in filenames:
                        path = Path(dirpath) / filename
                        if path.suffix.lower() not in self.EXTENSIONS or path.is_symlink():
                            continue
                        try:
                            canonical = path.resolve(strict=True)
                            if not is_relative_to(canonical, root):
                                continue
                            stat = canonical.stat()
                            relative = canonical.relative_to(root).as_posix()
                        except OSError:
                            continue
                        items.append({
                            "id": self._encode_id(library.id, relative),
                            "name": canonical.name,
                            "relativePath": relative,
                            "root": str(root),
                            "libraryId": library.id,
                            "libraryName": library.name,
                            "type": canonical.suffix.lower().lstrip("."),
                            "sizeBytes": stat.st_size,
                            "modified": datetime.fromtimestamp(stat.st_mtime, tz=timezone.utc).isoformat(timespec="seconds"),
                        })
                        counts[library.id] += 1
                        type_counts[library.id][canonical.suffix.lower().lstrip(".")] += 1
                        if len(items) >= self.config.max_media_results:
                            break
                    if len(items) >= self.config.max_media_results:
                        break
                if len(items) >= self.config.max_media_results:
                    break
            items.sort(key=lambda item: (item.get("libraryName", "").casefold(), item["relativePath"].casefold()))
            self._cache = items
            self._library_counts = counts
            self._library_type_counts = type_counts
            self._cached_at = now
            return list(items)

    def library_count(self, library_id: str) -> int | None:
        with self._lock:
            return self._library_counts.get(library_id)

    def library_type_counts(self, library_id: str) -> dict[str, int] | None:
        with self._lock:
            value = self._library_type_counts.get(library_id)
            return dict(value) if value is not None else None

    def resolve(self, media_id: str) -> tuple[Path, dict[str, Any]]:
        library_key, relative = self._decode_id(media_id)
        libraries = self.settings.libraries(enabled_only=False)
        library: MediaLibrary | None = None
        if isinstance(library_key, int):
            # Compatibility with private Phase 6 IDs that encoded a root index.
            if 0 <= library_key < len(libraries):
                library = libraries[library_key]
        else:
            library = next((item for item in libraries if item.id == library_key), None)
        if library is None:
            raise ApiError(400, "Media library is no longer configured.")
        if not library.enabled:
            raise ApiError(409, "The selected media library is disabled.")
        rel_path = Path(relative)
        if rel_path.is_absolute() or ".." in rel_path.parts:
            raise ApiError(400, "Unsafe media path.")
        root = library.path
        candidate = root / rel_path
        if candidate.is_symlink():
            raise ApiError(400, "Symlinked media files are not allowed.")
        try:
            path = candidate.resolve(strict=True)
        except FileNotFoundError as exc:
            raise ApiError(404, "The selected media file no longer exists.") from exc
        if not is_relative_to(path, root) or path.is_symlink():
            raise ApiError(400, "The selected path leaves the configured media library.")
        if not path.is_file() or path.suffix.lower() not in self.EXTENSIONS:
            raise ApiError(400, "Only MVC MKV/MK3D files and unencrypted Blu-ray 3D ISO files are supported.")
        stat = path.stat()
        return path, {
            "id": media_id,
            "name": path.name,
            "relativePath": path.relative_to(root).as_posix(),
            "root": str(root),
            "libraryId": library.id,
            "libraryName": library.name,
            "type": path.suffix.lower().lstrip("."),
            "sizeBytes": stat.st_size,
        }


@dataclass
class Session:
    id: str
    source_path: Path
    media: dict[str, Any]
    session_dir: Path
    output_mode: str = "half-sbs"
    swap_eyes: bool = False
    audio_stream: int = 0
    requested_start_seconds: float = 0.0
    effective_start_seconds: float | None = None
    replacement_of: str | None = None
    source_duration_seconds: float | None = None
    source_fps: float = 24000.0 / 1001.0
    state: str = "starting"
    created_at: str = field(default_factory=utc_now)
    started_monotonic: float = field(default_factory=time.monotonic)
    ended_at: str | None = None
    ended_monotonic: float | None = field(default=None, repr=False)
    playable: bool = False
    segment_count: int = 0
    pair_count: int = 0
    output_pair_count: int = 0
    skipped_pair_count: int = 0
    pair_fps: float | None = None
    realtime_x: float | None = None
    ffmpeg_fps: float | None = None
    ffmpeg_speed_x: float | None = None
    last_error: str | None = None
    stop_requested: bool = False
    exit_code: int | None = None
    source_verified: bool | None = None
    process: subprocess.Popen[bytes] | None = field(default=None, repr=False)
    thread: threading.Thread | None = field(default=None, repr=False)
    log_offset: int = field(default=0, repr=False)
    progress_buffer: str = field(default="", repr=False)

    @property
    def hls_dir(self) -> Path:
        return self.session_dir / "hls"

    @property
    def work_dir(self) -> Path:
        return self.session_dir / "work"

    @property
    def log_path(self) -> Path:
        return self.session_dir / "engine.log"

    @property
    def report_path(self) -> Path:
        return self.session_dir / "diagnostic-report.txt"

    @property
    def status_path(self) -> Path:
        return self.session_dir / "status.json"

    def public(self, config: Config) -> dict[str, Any]:
        elapsed_end = self.ended_monotonic if self.ended_monotonic is not None else time.monotonic()
        elapsed = max(0.0, elapsed_end - self.started_monotonic)
        generated = self.output_pair_count / self.source_fps if self.source_fps > 0 else 0.0
        playback_origin = (self.effective_start_seconds
                           if self.effective_start_seconds is not None
                           else self.requested_start_seconds)
        source_position = playback_origin + generated
        if self.source_duration_seconds is not None:
            source_position = min(source_position, self.source_duration_seconds)
        return {
            "id": self.id,
            "source": self.media,
            "outputMode": self.output_mode,
            "swapEyes": self.swap_eyes,
            "audioStream": self.audio_stream,
            "state": self.state,
            "createdAt": self.created_at,
            "endedAt": self.ended_at,
            "elapsedSeconds": round(elapsed, 3),
            "playable": self.playable,
            "playlistUrl": f"/hls/{self.id}/stream.m3u8" if self.playable else None,
            "segmentCount": self.segment_count,
            "pairCount": self.pair_count,
            "outputPairCount": self.output_pair_count,
            "skippedPairCount": self.skipped_pair_count,
            "pairFps": self.pair_fps,
            "sourceFps": round(self.source_fps, 9),
            "requestedStartSeconds": round(self.requested_start_seconds, 6),
            "cleanReleaseStartSeconds": (round(self.effective_start_seconds, 6)
                                           if self.effective_start_seconds is not None else None),
            "replacesSessionId": self.replacement_of,
            "sourceDurationSeconds": self.source_duration_seconds,
            "generatedDurationSeconds": round(generated, 3),
            "sourcePositionSeconds": round(source_position, 3),
            "realtimeFactor": self.realtime_x,
            "ffmpegFps": self.ffmpeg_fps,
            "ffmpegSpeedFactor": self.ffmpeg_speed_x,
            "lastError": self.last_error,
            "stopRequested": self.stop_requested,
            "exitCode": self.exit_code,
            "sourceVerified": self.source_verified,
            "minimumReadySegments": config.minimum_ready_segments,
            "reportUrl": f"/api/sessions/current/report",
        }


class SessionManager:
    RESULT_RE = re.compile(
        r"(?:PROGRESS|RESULT)\b[^\n]*?pairs=(\d+)[^\n]*?pair_fps=([0-9.]+)[^\n]*?realtime_x=([0-9.]+)"
    )
    OUTPUT_RE = re.compile(r"(?:PROGRESS|RESULT)\b[^\n]*?emitted_pairs=(\d+)[^\n]*?skipped_pairs=(\d+)")
    FRAME_RE = re.compile(r"frame=\s*(\d+).*?fps=\s*([0-9.]+).*?speed=\s*([0-9.]+)x")
    SAMPLE_RE = re.compile(r"^Sample:\s*(.+?)\s*$", re.MULTILINE)
    CLEAN_RELEASE_RE = re.compile(
        r"^Clean visible release source timestamp:\s*([0-9.]+)\s*s\s*$", re.MULTILINE
    )

    def __init__(self, config: Config, catalog: MediaCatalog):
        self.config = config
        self.catalog = catalog
        self.config.state_root.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self._replace_lock = threading.Lock()
        self._probe_lock = threading.Lock()
        self._probe_cache: dict[tuple[str, int, int], dict[str, Any]] = {}
        self._current: Session | None = None
        self._maintenance_stop = threading.Event()
        self._maintenance_thread: threading.Thread | None = None
        self._recover_interrupted_sessions()
        try:
            self.cleanup()
        except Exception as exc:
            print(f"Startup session cleanup failed: {exc}", flush=True)
        if self.config.cleanup_interval_seconds > 0:
            self._maintenance_thread = threading.Thread(
                target=self._maintenance_loop, name="sylc-session-maintenance", daemon=True
            )
            self._maintenance_thread.start()

    def shutdown(self) -> None:
        self._maintenance_stop.set()
        if self._maintenance_thread:
            self._maintenance_thread.join(timeout=2.0)

    def _maintenance_loop(self) -> None:
        interval = max(1.0, self.config.cleanup_interval_seconds)
        while not self._maintenance_stop.wait(interval):
            try:
                self.cleanup()
            except Exception as exc:
                print(f"Automatic session cleanup failed: {exc}", flush=True)

    def _recover_interrupted_sessions(self) -> None:
        for status_path in self.config.state_root.glob("*/status.json"):
            try:
                status = json.loads(status_path.read_text(encoding="utf-8"))
                if status.get("state") in {"starting", "running", "buffering", "playable", "stopping"}:
                    status["state"] = "interrupted"
                    status["endedAt"] = utc_now()
                    status["lastError"] = "Service restarted while this session was active."
                    atomic_write_json(status_path, status)
            except Exception:
                continue

    def current(self) -> Session | None:
        with self._lock:
            return self._current

    def _probe_source_uncached(self, source: Path) -> dict[str, Any]:
        if source.suffix.lower() == ".iso":
            iso_binary = self.config.phase5_project / "build" / "sylc_iso_source"
            command = [str(iso_binary), "--input", str(source), "--probe"]
            try:
                completed = subprocess.run(
                    command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                    timeout=180, check=False,
                )
            except (OSError, subprocess.TimeoutExpired) as exc:
                raise ApiError(500, "Could not inspect the selected Blu-ray ISO.", str(exc)) from exc
            if completed.returncode != 0:
                raise ApiError(400, "The selected ISO could not be resolved as an unencrypted Blu-ray 3D feature.", completed.stderr.strip())
            try:
                data = json.loads(completed.stdout)
                duration = float(data["durationSeconds"])
                fps = float(data["fps"])
                width = int(data["width"])
                height = int(data["height"])
                if not data.get("hasMVC"):
                    raise ValueError("selected feature has no MVC dependent view")
            except Exception as exc:
                raise ApiError(400, "The selected ISO has incomplete Blu-ray feature metadata.", str(exc)) from exc
            if not math.isfinite(duration) or duration <= 0 or not math.isfinite(fps) or fps <= 0:
                raise ApiError(400, "The selected ISO has invalid duration or frame-rate metadata.")
            return {
                "duration": duration, "fps": fps, "width": width, "height": height,
                "playlist": data.get("playlist"), "selectionMethod": data.get("selectionMethod"),
                "segmentCount": data.get("segmentCount"), "decoysFiltered": data.get("decoysFiltered"),
                "audioTracks": data.get("audioTracks") or [], "sourceType": "bluray-iso",
                "libblurayAuthoritative": data.get("libblurayAuthoritative"),
                "libblurayVersion": data.get("libblurayVersion"),
                "selectedTitleIndex": data.get("selectedTitleIndex"),
                "titleCount": data.get("titleCount"), "mainTitleHint": data.get("mainTitleHint"),
            }

        command = [
            "ffprobe", "-v", "error", "-select_streams", "v:0",
            "-show_entries", "format=duration:stream=avg_frame_rate,start_time,duration:stream_tags=DURATION",
            "-of", "json", str(source),
        ]
        try:
            completed = subprocess.run(
                command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                timeout=30, check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise ApiError(500, "Could not inspect the selected source.", str(exc)) from exc
        if completed.returncode != 0:
            raise ApiError(400, "The selected file could not be inspected as media.", completed.stderr.strip())
        try:
            data = json.loads(completed.stdout)
            stream = (data.get("streams") or [])[0]
            format_duration = float(data["format"]["duration"])
            duration = stream.get("duration")
            if duration in (None, "N/A", ""):
                duration_tag = (stream.get("tags") or {}).get("DURATION")
                if duration_tag:
                    hours, minutes, seconds = duration_tag.split(":", 2)
                    duration = float(hours) * 3600 + float(minutes) * 60 + float(seconds)
                else:
                    duration = format_duration
            duration = float(duration)
            fps_raw = str(stream.get("avg_frame_rate") or "24000/1001")
            if "/" in fps_raw:
                numerator, denominator = fps_raw.split("/", 1)
                fps = float(numerator) / float(denominator)
            else:
                fps = float(fps_raw)
        except Exception as exc:
            raise ApiError(400, "The selected file has no usable duration or frame rate.", str(exc)) from exc
        if not math.isfinite(duration) or duration <= 0 or not math.isfinite(fps) or fps <= 0:
            raise ApiError(400, "The selected file has invalid duration or frame-rate metadata.")
        return {"duration": duration, "fps": fps, "sourceType": "mvc-mkv"}

    def _probe_source(self, source: Path) -> dict[str, Any]:
        stat = source.stat()
        key = (str(source.resolve()), stat.st_size, stat.st_mtime_ns)
        with self._probe_lock:
            cached = self._probe_cache.get(key)
            if cached is not None:
                return dict(cached)
        result = self._probe_source_uncached(source)
        with self._probe_lock:
            self._probe_cache = {key: dict(result)}
        return result

    @staticmethod
    def _parse_audio_stream(raw_audio: Any) -> int:
        if raw_audio is None:
            return 0
        if isinstance(raw_audio, bool):
            raise ApiError(400, "audioStream must be a non-negative integer.")
        try:
            value = int(raw_audio)
        except (TypeError, ValueError) as exc:
            raise ApiError(400, "audioStream must be a non-negative integer.") from exc
        if value < 0 or str(value) != str(raw_audio).strip():
            raise ApiError(400, "audioStream must be a non-negative integer.")
        return value

    @staticmethod
    def _validate_audio_stream(source: Path, probe: dict[str, Any], audio_stream: int) -> None:
        if source.suffix.lower() == ".iso":
            tracks = probe.get("audioTracks") or []
            if audio_stream >= len(tracks):
                raise ApiError(400, f"ISO audio track {audio_stream} is unavailable; this title exposes {len(tracks)} tracks.")
            track = tracks[audio_stream]
            if not track.get("supported"):
                raise ApiError(400, f"ISO audio track {audio_stream} is not supported by the HLS bridge.")
        elif audio_stream != 0:
            raise ApiError(400, "MKV audio-track selection is not yet exposed; audioStream must be 0 for MKV/MK3D.")

    @staticmethod
    def _parse_start_seconds(raw_start: Any) -> float:
        try:
            start_seconds = float(0 if raw_start is None else raw_start)
        except (TypeError, ValueError) as exc:
            raise ApiError(400, "startSeconds must be a non-negative number.") from exc
        if isinstance(raw_start, bool) or not math.isfinite(start_seconds) or start_seconds < 0:
            raise ApiError(400, "startSeconds must be a finite non-negative number.")
        return start_seconds

    @staticmethod
    def _parse_output_mode(raw_mode: Any, default: str = "half-sbs") -> str:
        mode = default if raw_mode is None else raw_mode
        if not isinstance(mode, str) or mode not in OUTPUT_MODES:
            raise ApiError(400, "mode must be one of: " + ", ".join(OUTPUT_MODES) + ".")
        return mode

    @staticmethod
    def _parse_swap_eyes(raw_swap: Any, default: bool = False) -> bool:
        value = default if raw_swap is None else raw_swap
        if value is True or value == 1:
            return True
        if value is False or value == 0:
            return False
        raise ApiError(400, "swapEyes must be true or false.")

    def inspect_media(self, media_id: str) -> dict[str, Any]:
        source, media = self.catalog.resolve(media_id)
        probe = self._probe_source(source)
        result = dict(media)
        result["durationSeconds"] = round(probe["duration"], 6)
        result["fps"] = round(probe["fps"], 9)
        for key in ("sourceType", "playlist", "selectionMethod", "segmentCount", "decoysFiltered", "audioTracks", "width", "height", "libblurayAuthoritative", "libblurayVersion", "selectedTitleIndex", "titleCount", "mainTitleHint"):
            if key in probe:
                result[key] = probe[key]
        return result

    def create(self, body: dict[str, Any], replacement_of: str | None = None) -> Session:
        media_id = body.get("mediaId")
        if not isinstance(media_id, str):
            raise ApiError(400, "mediaId is required. Select a file from /api/media.")
        mode = self._parse_output_mode(body.get("mode"), "half-sbs")
        swap_eyes = self._parse_swap_eyes(body.get("swapEyes"), False)
        start_seconds = self._parse_start_seconds(body.get("startSeconds", 0))
        audio_stream = self._parse_audio_stream(body.get("audioStream", 0))

        source, media = self.catalog.resolve(media_id)
        probe = self._probe_source(source)
        self._validate_audio_stream(source, probe, audio_stream)
        duration = probe["duration"]
        if start_seconds >= duration:
            raise ApiError(400, f"Start position must be before the end of the title ({duration:.3f} seconds).")
        media = dict(media)
        media["durationSeconds"] = round(duration, 6)
        for key in ("sourceType", "playlist", "selectionMethod", "segmentCount", "decoysFiltered", "audioTracks", "width", "height", "libblurayAuthoritative", "libblurayVersion", "selectedTitleIndex", "titleCount", "mainTitleHint"):
            if key in probe:
                media[key] = probe[key]
        self.ensure_capacity()
        with self._lock:
            if self._current and self._current.state in {"starting", "running", "buffering", "playable", "stopping"}:
                raise ApiError(409, "A conversion session is already active.")
            session_id = uuid.uuid4().hex[:16]
            session_dir = self.config.state_root / session_id
            session_dir.mkdir(mode=0o750, parents=True, exist_ok=False)
            session = Session(
                id=session_id, source_path=source, media=media, session_dir=session_dir,
                output_mode=mode, swap_eyes=swap_eyes, audio_stream=audio_stream,
                requested_start_seconds=start_seconds, replacement_of=replacement_of,
                source_duration_seconds=duration,
                source_fps=probe["fps"],
            )
            session.hls_dir.mkdir(mode=0o750)
            session.work_dir.mkdir(mode=0o750)
            self._current = session
            self._persist(session)
            thread = threading.Thread(target=self._run_session, args=(session,), name=f"sylc-session-{session_id}", daemon=True)
            session.thread = thread
            thread.start()
            return session

    def stop(self) -> Session:
        with self._lock:
            session = self._current
            if not session:
                raise ApiError(404, "There is no current session.")
            if session.state not in {"starting", "running", "buffering", "playable", "stopping"}:
                return session
            session.stop_requested = True
            session.state = "stopping"
            process = session.process
            self._persist(session)
        if process and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        return session

    def seek(self, body: dict[str, Any]) -> tuple[Session, Session]:
        """Replace the current title with a new session at a source timestamp.

        The old session directory is intentionally retained, so its HLS URL
        remains readable while the replacement starts. Only one conversion
        process runs at a time.
        """
        target = self._parse_start_seconds(body.get("startSeconds"))
        with self._replace_lock:
            with self._lock:
                previous = self._current
                if previous is None:
                    raise ApiError(404, "There is no current title to seek.")
                duration = previous.source_duration_seconds
                media_id = previous.media.get("id")
                active = previous.state in {"starting", "running", "buffering", "playable", "stopping"}
            if not isinstance(media_id, str):
                raise ApiError(500, "The current session has no reusable media ID.")
            if duration is None:
                duration = self._probe_source(previous.source_path)["duration"]
            if target >= duration:
                raise ApiError(400, f"Seek position must be before the end of the title ({duration:.3f} seconds).")
            output_mode = self._parse_output_mode(body.get("mode"), previous.output_mode)
            swap_eyes = self._parse_swap_eyes(body.get("swapEyes"), previous.swap_eyes)
            audio_stream = self._parse_audio_stream(body.get("audioStream", previous.audio_stream))
            self._validate_audio_stream(previous.source_path, self._probe_source(previous.source_path), audio_stream)

            if active:
                self.stop()
                deadline = time.monotonic() + self.config.stop_grace_seconds + 10.0
                thread = previous.thread
                if thread is not None:
                    thread.join(timeout=max(0.0, deadline - time.monotonic()))
                while previous.state in {"starting", "running", "buffering", "playable", "stopping"} and time.monotonic() < deadline:
                    time.sleep(0.05)
                if previous.state in {"starting", "running", "buffering", "playable", "stopping"}:
                    raise ApiError(504, "The active conversion did not stop in time for seeking.")

            replacement = self.create(
                {
                    "mediaId": media_id,
                    "mode": output_mode,
                    "audioStream": audio_stream,
                    "swapEyes": swap_eyes,
                    "startSeconds": target,
                },
                replacement_of=previous.id,
            )
            return previous, replacement

    def _active_id(self) -> str | None:
        with self._lock:
            if self._current and self._current.state in {"starting", "running", "buffering", "playable", "stopping"}:
                return self._current.id
        return None

    def _clear_current_if_removed(self, session_id: str) -> None:
        with self._lock:
            if self._current and self._current.id == session_id:
                self._current = None

    def cleanup(self) -> dict[str, Any]:
        active_id = self._active_id()
        cutoff = time.time() - self.config.session_ttl_hours * 3600.0
        removed: list[str] = []
        kept: list[str] = []
        for path in self.config.state_root.iterdir():
            if not path.is_dir() or path.name == active_id:
                continue
            try:
                if self.config.session_ttl_hours == 0 or path.stat().st_mtime < cutoff:
                    shutil.rmtree(path)
                    removed.append(path.name)
                    self._clear_current_if_removed(path.name)
                else:
                    kept.append(path.name)
            except OSError:
                kept.append(path.name)
        return {"removed": removed, "kept": kept, "active": active_id}

    def disk_status(self) -> dict[str, Any]:
        usage = shutil.disk_usage(self.config.state_root)
        return {
            "totalBytes": usage.total, "usedBytes": usage.used, "freeBytes": usage.free,
            "minimumFreeBytes": int(self.config.minimum_free_gib * 1024 ** 3),
            "emergencyFreeBytes": int(self.config.emergency_free_gib * 1024 ** 3),
        }

    def ensure_capacity(self) -> None:
        self.cleanup()
        minimum = int(self.config.minimum_free_gib * 1024 ** 3)
        if minimum <= 0:
            return
        active_id = self._active_id()
        candidates = []
        for path in self.config.state_root.iterdir():
            if path.is_dir() and path.name != active_id:
                try:
                    candidates.append((path.stat().st_mtime, path))
                except OSError:
                    pass
        candidates.sort()
        while shutil.disk_usage(self.config.state_root).free < minimum and candidates:
            _, path = candidates.pop(0)
            try:
                shutil.rmtree(path)
                self._clear_current_if_removed(path.name)
            except OSError:
                continue
        free = shutil.disk_usage(self.config.state_root).free
        if free < minimum:
            raise ApiError(507, "Insufficient free space for a new streaming session.",
                           f"Free: {free / 1024 ** 3:.2f} GiB; required: {self.config.minimum_free_gib:.2f} GiB")

    def _persist(self, session: Session) -> None:
        atomic_write_json(session.status_path, session.public(self.config))

    def _set_state(self, session: Session, state: str, error: str | None = None) -> None:
        with self._lock:
            session.state = state
            if error:
                session.last_error = error
            if state in {"completed", "failed", "stopped", "interrupted"}:
                session.ended_at = utc_now()
                session.ended_monotonic = time.monotonic()
            self._persist(session)

    def _prepare_legacy_outputs(self, session: Session) -> None:
        """Remove only Phase 5 generated serving artifacts before a new job.

        The proof used fixed cache/report paths.  Without this step, a stale
        playlist or report could be mistaken for output from the requested
        title before the new runner has had time to replace it.  Existing
        reports are archived into the new session first.
        """
        legacy_report = self.config.legacy_report_path
        if legacy_report.is_file():
            try:
                shutil.copy2(legacy_report, session.session_dir / "preexisting-phase5-report.txt")
                legacy_report.unlink()
            except OSError as exc:
                raise RuntimeError(f"Could not archive/reset legacy Phase 5 report: {exc}") from exc

        legacy_hls = self.config.legacy_work_root / "hls"
        if legacy_hls.is_dir():
            for item in legacy_hls.iterdir():
                if not item.is_file():
                    continue
                if item.name == "stream.m3u8" or item.suffix.lower() in {".ts", ".tmp", ".m3u8"}:
                    try:
                        item.unlink()
                    except OSError as exc:
                        raise RuntimeError(f"Could not reset stale Phase 5 HLS object {item}: {exc}") from exc

    def _runner_environment(self, session: Session) -> dict[str, str]:
        env = os.environ.copy()
        source = str(session.source_path)
        work = str(session.work_dir)
        report = str(session.report_path)
        hls = str(session.hls_dir)
        env.update({
            "SYLC_SOURCE_PATH": source,
            "SYLC_SESSION_DIR": str(session.session_dir),
            "SYLC_WORK_ROOT": work,
            "SYLC_HLS_DIR": hls,
            "SYLC_REPORT": report,
            "SYLC_ENGINE_PROJECT": str(self.config.phase5_project),
            "SYLC_ENGINE_BUILD_DIR": str(self.config.phase5_project / "build"),
            "SYLC_ENGINE_BINARY": str(self.config.phase5_project / "build" / "sylc_hsbs_pipe"),
            "SYLC_ISO_BINARY": str(self.config.phase5_project / "build" / "sylc_iso_source"),
            "SYLC_THREADS": "0",
            "SYLC_OUTPUT_MODE": session.output_mode,
            "SYLC_SWAP_EYES": "1" if session.swap_eyes else "0",
            "SYLC_AUDIO_STREAM": str(session.audio_stream),
            "SYLC_ENCODER_MODE": self.config.encoder_mode,
            "SYLC_VAAPI_DEVICE": str(self.config.vaapi_device),
            "SYLC_START_SECONDS": f"{session.requested_start_seconds:.6f}",
        })
        return env

    def _candidate_hls_dirs(self, session: Session) -> Iterable[Path]:
        yielded: set[Path] = set()
        for path in (
            session.hls_dir,
            session.work_dir / "hls",
        ):
            resolved = path.resolve(strict=False)
            if resolved not in yielded:
                yielded.add(resolved)
                yield resolved

    def _candidate_reports(self, session: Session) -> Iterable[Path]:
        yielded: set[Path] = set()
        for path in (
            session.report_path,
            session.work_dir / "report.txt",
        ):
            resolved = path.resolve(strict=False)
            if resolved not in yielded:
                yielded.add(resolved)
                yield resolved

    def _mirror_hls(self, session: Session) -> None:
        for source_dir in self._candidate_hls_dirs(session):
            if source_dir == session.hls_dir.resolve(strict=False) or not source_dir.is_dir():
                continue
            for source in source_dir.iterdir():
                if not source.is_file() or source.suffix.lower() not in {".ts", ".m3u8"}:
                    continue
                destination = session.hls_dir / source.name
                try:
                    if source.suffix.lower() == ".ts":
                        if not destination.exists() or destination.stat().st_size != source.stat().st_size:
                            tmp = destination.with_suffix(destination.suffix + ".copying")
                            shutil.copyfile(source, tmp)
                            os.replace(tmp, destination)
                    else:
                        atomic_write(destination, source.read_bytes())
                except (FileNotFoundError, PermissionError, OSError):
                    continue

    def _mirror_report(self, session: Session) -> str | None:
        for candidate in self._candidate_reports(session):
            if not candidate.is_file():
                continue
            try:
                text = candidate.read_text(encoding="utf-8", errors="replace")
                if candidate != session.report_path:
                    atomic_write(session.report_path, text.encode("utf-8"))
                return text
            except OSError:
                continue
        return None

    def _safe_playlist_dir(self, hls_dir: Path) -> bytes | None:
        playlist = hls_dir / "stream.m3u8"
        if not playlist.is_file():
            return None
        try:
            lines = playlist.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            return None
        safe: list[str] = []
        pending_extinf: str | None = None
        for line in lines:
            stripped = line.strip()
            if stripped.startswith("#EXTINF:"):
                pending_extinf = line
                continue
            if stripped and not stripped.startswith("#") and stripped.lower().endswith(".ts"):
                name = Path(unquote(stripped)).name
                if (hls_dir / name).is_file():
                    if pending_extinf is not None:
                        safe.append(pending_extinf)
                    safe.append(name)
                pending_extinf = None
                continue
            if pending_extinf is not None and stripped.startswith("#"):
                safe.append(line)
                continue
            safe.append(line)
        if not safe or not safe[0].startswith("#EXTM3U"):
            return None
        return ("\n".join(safe) + "\n").encode("utf-8")

    def _safe_playlist(self, session: Session) -> bytes | None:
        return self._safe_playlist_dir(session.hls_dir)

    def _hls_dir_for_id(self, session_id: str) -> Path:
        if not re.fullmatch(r"[0-9a-f]{16}", session_id):
            raise ApiError(400, "Invalid HLS session ID.")
        current = self.current()
        if current and current.id == session_id:
            self._mirror_hls(current)
            return current.hls_dir
        root = self.config.state_root.resolve(strict=False)
        session_dir = (root / session_id).resolve(strict=False)
        if not is_relative_to(session_dir, root) or not session_dir.is_dir():
            raise ApiError(404, "HLS session not found.")
        hls_dir = (session_dir / "hls").resolve(strict=False)
        if not is_relative_to(hls_dir, session_dir) or not hls_dir.is_dir():
            raise ApiError(404, "HLS session not found.")
        return hls_dir

    def playlist_bytes(self, session_id: str) -> bytes:
        hls_dir = self._hls_dir_for_id(session_id)
        data = self._safe_playlist_dir(hls_dir)
        if data is None:
            raise ApiError(404, "The playlist is not ready yet.")
        return data

    def hls_file(self, session_id: str, name: str) -> Path:
        if Path(name).name != name or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            raise ApiError(400, "Invalid HLS object name.")
        hls_dir = self._hls_dir_for_id(session_id)
        path = (hls_dir / name).resolve(strict=False)
        if not is_relative_to(path, hls_dir.resolve(strict=False)) or not path.is_file():
            raise ApiError(404, "HLS object not found.")
        return path

    def _parse_progress(self, session: Session) -> None:
        try:
            with session.log_path.open("r", encoding="utf-8", errors="replace") as source:
                source.seek(session.log_offset)
                chunk = source.read()
                session.log_offset = source.tell()
        except OSError:
            return
        if not chunk:
            return
        data = session.progress_buffer + chunk.replace("\r", "\n")
        session.progress_buffer = data[-16384:]
        result_matches = list(self.RESULT_RE.finditer(data))
        if result_matches:
            match = result_matches[-1]
            session.pair_count = int(match.group(1))
            session.pair_fps = float(match.group(2))
            session.realtime_x = float(match.group(3))
        output_matches = list(self.OUTPUT_RE.finditer(data))
        if output_matches:
            match = output_matches[-1]
            session.output_pair_count = int(match.group(1))
            session.skipped_pair_count = int(match.group(2))
        frame_matches = list(self.FRAME_RE.finditer(data))
        if frame_matches:
            match = frame_matches[-1]
            frame_count = int(match.group(1))
            session.output_pair_count = max(session.output_pair_count, frame_count)
            session.pair_count = max(session.pair_count, frame_count)
            session.ffmpeg_fps = float(match.group(2))
            session.ffmpeg_speed_x = float(match.group(3))
        release_matches = list(self.CLEAN_RELEASE_RE.finditer(data))
        if release_matches:
            session.effective_start_seconds = float(release_matches[-1].group(1))

    def _verify_source(self, session: Session, report_text: str | None) -> bool | None:
        if not report_text:
            return None
        match = self.SAMPLE_RE.search(report_text)
        if not match:
            return None
        reported = Path(match.group(1).strip()).expanduser().resolve(strict=False)
        expected = session.source_path.resolve(strict=False)
        return reported == expected

    def _update_hls_status(self, session: Session) -> None:
        self._mirror_hls(session)
        segments = [p for p in session.hls_dir.glob("*.ts") if p.is_file()]
        session.segment_count = len(segments)
        if session.segment_count >= self.config.minimum_ready_segments and self._safe_playlist(session):
            session.playable = True
            if session.state in {"starting", "running", "buffering"}:
                session.state = "playable"
        elif session.state in {"starting", "running"}:
            session.state = "buffering"

    def _terminate_after_grace(self, process: subprocess.Popen[bytes]) -> None:
        deadline = time.monotonic() + self.config.stop_grace_seconds
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def _append_service_report(self, session: Session) -> None:
        status = session.public(self.config)
        parts = [
            "",
            "============================================================",
            "SyLC MVC Stream Service Phase 6 streaming session summary",
            "============================================================",
            f"Service version: {APP_VERSION}",
            f"Session: {session.id}",
            f"Requested source: {session.source_path}",
            f"Requested start: {session.requested_start_seconds:.6f} s",
            f"Output mode: {session.output_mode}",
            f"Swap eyes: {session.swap_eyes}",
            f"Audio stream: {session.audio_stream}",
            f"Replacement of session: {session.replacement_of or 'none'}",
            f"Source duration: {session.source_duration_seconds}",
            f"Source verification: {session.source_verified}",
            f"State: {session.state}",
            f"Exit code: {session.exit_code}",
            f"Segments: {session.segment_count}",
            f"Pairs processed: {session.pair_count}",
            f"Pairs emitted: {session.output_pair_count}",
            f"Pairs discarded for seek: {session.skipped_pair_count}",
            f"Pair rate: {session.pair_fps}",
            f"Real-time factor: {session.realtime_x}",
            f"FFmpeg speed factor: {session.ffmpeg_speed_x}",
            f"Last error: {session.last_error or 'none'}",
            "",
            "Service status JSON:",
            json.dumps(status, indent=2, sort_keys=True),
            "",
            "Service log:",
        ]
        try:
            log_text = session.log_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            log_text = "(log unavailable)"
        existing = ""
        if session.report_path.is_file():
            try:
                existing = session.report_path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                pass
        atomic_write(session.report_path, (existing + "\n".join(parts) + "\n" + log_text + "\n").encode("utf-8"))

    def _run_session(self, session: Session) -> None:
        try:
            if not self.config.phase5_project.is_dir():
                raise RuntimeError(f"Phase 6 streaming engine project not found: {self.config.phase5_project}")
            if not self.config.phase5_wrapper.is_file():
                raise RuntimeError(f"Phase 6 streaming runner not found: {self.config.phase5_wrapper}")
            binary = self.config.phase5_project / "build" / "sylc_hsbs_pipe"
            if not binary.is_file():
                raise RuntimeError(f"Phase 6 streaming binary not found: {binary}")
            if not session.source_path.is_file():
                raise RuntimeError("Source file disappeared before conversion started")

            self._set_state(session, "running")
            env = self._runner_environment(session)
            command = [str(self.config.phase5_wrapper), str(session.source_path)]
            with session.log_path.open("wb", buffering=0) as log:
                log.write((
                    f"SyLC MVC Stream Service {APP_VERSION}\n"
                    f"Session: {session.id}\n"
                    f"Source: {session.source_path}\n"
                    f"Started: {utc_now()}\n"
                    f"Command: {json.dumps(command)}\n\n"
                ).encode("utf-8"))
                process = subprocess.Popen(
                    command,
                    cwd=self.config.phase5_project,
                    env=env,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                with self._lock:
                    session.process = process
                    self._persist(session)

                stop_killer_started = False
                last_pair_count = session.pair_count
                last_pair_progress = time.monotonic()
                last_disk_check = 0.0
                emergency_free = int(self.config.emergency_free_gib * 1024 ** 3)
                while process.poll() is None:
                    with self._lock:
                        if session.stop_requested and not stop_killer_started:
                            stop_killer_started = True
                            try:
                                os.killpg(process.pid, signal.SIGTERM)
                            except ProcessLookupError:
                                pass
                            threading.Thread(
                                target=self._terminate_after_grace,
                                args=(process,),
                                name=f"sylc-kill-{session.id}",
                                daemon=True,
                            ).start()
                        self._parse_progress(session)
                        self._update_hls_status(session)
                        now = time.monotonic()
                        if emergency_free > 0 and now - last_disk_check >= 5.0:
                            last_disk_check = now
                            free = shutil.disk_usage(self.config.state_root).free
                            if free < emergency_free and not session.stop_requested:
                                session.last_error = (
                                    f"Streaming stopped to protect disk space: only "
                                    f"{free / 1024 ** 3:.2f} GiB remains; emergency threshold is "
                                    f"{self.config.emergency_free_gib:.2f} GiB."
                                )
                                session.stop_requested = True
                        if session.pair_count > last_pair_count:
                            last_pair_count = session.pair_count
                            last_pair_progress = now
                        elif not session.stop_requested:
                            if session.pair_count == 0 and now - session.started_monotonic > self.config.startup_timeout_seconds:
                                session.last_error = (
                                    f"Streaming startup produced no stereo pairs for "
                                    f"{self.config.startup_timeout_seconds:.0f} seconds. "
                                    "The session was stopped by the startup watchdog."
                                )
                                session.stop_requested = True
                            elif session.pair_count > 0 and now - last_pair_progress > self.config.stall_timeout_seconds:
                                session.last_error = (
                                    f"Streaming decoder made no stereo-pair progress for "
                                    f"{self.config.stall_timeout_seconds:.0f} seconds after pair "
                                    f"{session.pair_count}. The session was stopped by the stall watchdog."
                                )
                                session.stop_requested = True
                        report_text = self._mirror_report(session)
                        verified = self._verify_source(session, report_text)
                        if verified is not None:
                            session.source_verified = verified
                            if verified is False and not session.stop_requested:
                                session.last_error = (
                                    "The streaming runner converted a different source than requested. "
                                    "The service stopped rather than serving incorrect media."
                                )
                                session.stop_requested = True
                                try:
                                    os.killpg(process.pid, signal.SIGTERM)
                                except ProcessLookupError:
                                    pass
                        self._persist(session)
                    time.sleep(0.25)

                session.exit_code = process.returncode
                with self._lock:
                    self._parse_progress(session)
                    self._update_hls_status(session)
                    report_text = self._mirror_report(session)
                    verified = self._verify_source(session, report_text)
                    if verified is not None:
                        session.source_verified = verified

                if session.stop_requested:
                    self._set_state(session, "failed" if session.last_error else "stopped", session.last_error)
                elif process.returncode == 0 and session.source_verified is not False:
                    self._set_state(session, "completed")
                else:
                    tail = ""
                    try:
                        tail = "\n".join(session.log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-12:])
                    except OSError:
                        pass
                    self._set_state(
                        session,
                        "failed",
                        session.last_error or f"Phase 6 streaming pipeline exited with status {process.returncode}.\n{tail}".strip(),
                    )
        except Exception as exc:
            self._set_state(session, "failed", f"{exc}\n{traceback.format_exc(limit=5)}")
        finally:
            with self._lock:
                session.process = None
                self._update_hls_status(session)
                self._append_service_report(session)
                self._persist(session)


class ServiceApp:
    def __init__(self, config: Config):
        self.config = config
        self.settings = SettingsStore(config)
        self.catalog = MediaCatalog(config, self.settings)
        self.sessions = SessionManager(config, self.catalog)

    def setup_status(self) -> dict[str, Any]:
        return {
            "ok": True,
            "setupRequired": self.settings.setup_required(),
            "tokenConfigured": self.settings.token_configured(),
            "version": APP_VERSION,
            "browseRoots": [str(path) for path in self._browse_roots()],
        }

    def _browse_roots(self) -> list[Path]:
        values: list[Path] = []
        for root in self.config.browse_roots:
            try:
                canonical = root.resolve(strict=True)
            except OSError:
                continue
            if canonical.is_dir() and canonical not in values:
                values.append(canonical)
        return values

    def browse_directories(self, raw_path: str | None) -> dict[str, Any]:
        roots = self._browse_roots()
        if not raw_path:
            return {
                "ok": True,
                "path": None,
                "parent": None,
                "directories": [
                    {"name": str(root), "path": str(root), "readable": os.access(root, os.R_OK | os.X_OK)}
                    for root in roots
                ],
            }
        candidate = Path(unquote(raw_path)).expanduser()
        if not candidate.is_absolute():
            raise ApiError(400, "Folder-browser paths must be absolute.")
        try:
            path = candidate.resolve(strict=True)
        except FileNotFoundError as exc:
            raise ApiError(404, "Folder not found.", str(candidate)) from exc
        if not path.is_dir():
            raise ApiError(400, "The selected path is not a folder.")
        containing_root = next((root for root in roots if is_relative_to(path, root)), None)
        if containing_root is None:
            raise ApiError(403, "That folder is outside the configured browser roots. Enter it manually in the library form if needed.")
        directories: list[dict[str, Any]] = []
        try:
            for entry in os.scandir(path):
                try:
                    if entry.is_symlink() or not entry.is_dir(follow_symlinks=False):
                        continue
                    child = Path(entry.path).resolve(strict=True)
                    if not is_relative_to(child, containing_root):
                        continue
                    directories.append({
                        "name": entry.name,
                        "path": str(child),
                        "readable": os.access(child, os.R_OK | os.X_OK),
                    })
                except OSError:
                    continue
        except PermissionError as exc:
            raise ApiError(403, "The SyLC service user cannot browse this folder.", str(path)) from exc
        directories.sort(key=lambda item: item["name"].casefold())
        parent: str | None = None
        if path != containing_root:
            parent_path = path.parent
            if is_relative_to(parent_path, containing_root):
                parent = str(parent_path)
        return {"ok": True, "path": str(path), "parent": parent, "directories": directories}

    def test_library(self, body: dict[str, Any]) -> dict[str, Any]:
        path = SettingsStore._canonical_directory(body.get("path"))
        recursive = bool(body.get("recursive", True))
        counts = {"mkv": 0, "mk3d": 0, "iso": 0}
        scanned = 0
        limit = min(self.config.max_media_results, 10000)
        if recursive:
            walkers: Iterable[tuple[str, list[str], list[str]]] = os.walk(path, followlinks=False)
        else:
            walkers = [(str(path), [], [entry.name for entry in os.scandir(path) if entry.is_file(follow_symlinks=False)])]
        for dirpath, dirnames, filenames in walkers:
            dirnames[:] = [name for name in dirnames if not (Path(dirpath) / name).is_symlink()]
            for filename in filenames:
                suffix = Path(filename).suffix.lower().lstrip(".")
                if suffix in counts:
                    counts[suffix] += 1
                    scanned += 1
                    if scanned >= limit:
                        break
            if scanned >= limit:
                break
        return {
            "ok": True,
            "path": str(path),
            "exists": True,
            "readable": True,
            "recursive": recursive,
            "counts": counts,
            "supportedFiles": sum(counts.values()),
            "truncated": scanned >= limit,
        }

    def libraries_public(self, refresh_counts: bool = False) -> list[dict[str, Any]]:
        if refresh_counts:
            self.catalog.scan(force=True)
        result: list[dict[str, Any]] = []
        for item in self.settings.libraries():
            value = item.public()
            count = self.catalog.library_count(item.id)
            value["indexedFiles"] = count
            value["fileCounts"] = self.catalog.library_type_counts(item.id)
            result.append(value)
        return result

    def create_library(self, body: dict[str, Any]) -> MediaLibrary:
        item = self.settings.create(body)
        self.catalog.invalidate()
        return item

    def update_library(self, library_id: str, body: dict[str, Any]) -> MediaLibrary:
        item = self.settings.update(library_id, body)
        self.catalog.invalidate()
        return item

    def delete_library(self, library_id: str) -> None:
        current = self.sessions.current()
        if current and current.media.get("libraryId") == library_id and current.state in {"starting", "running", "buffering", "playable", "stopping"}:
            raise ApiError(409, "Stop the active stream before removing its media library.")
        self.settings.delete(library_id)
        self.catalog.invalidate()

    def health(self) -> dict[str, Any]:
        libraries = self.libraries_public(refresh_counts=False)
        current = self.sessions.current()
        return {
            "ok": True,
            "service": "sylc-mvc-stream",
            "version": APP_VERSION,
            "host": self.config.bind_host,
            "port": self.config.port,
            "setupRequired": self.settings.setup_required(),
            "apiTokenConfigured": self.settings.token_configured(),
            "streamingEngineAvailable": self.config.phase5_project.is_dir(),
            "streamingBinaryAvailable": (self.config.phase5_project / "build" / "sylc_hsbs_pipe").is_file(),
            "isoSourceAdapterAvailable": (self.config.phase5_project / "build" / "sylc_iso_source").is_file(),
            "nativeTrueHdAudio": True,
            "supportedIsoAudioFormats": ["truehd", "dts", "ac3", "eac3"],
            "streamingRunnerAvailable": self.config.phase5_wrapper.is_file(),
            "directStreaming": True,
            "encoderMode": self.config.encoder_mode,
            "vaapiDevice": str(self.config.vaapi_device),
            "supportedOutputModes": list(OUTPUT_MODES),
            "startupTimeoutSeconds": self.config.startup_timeout_seconds,
            "stallTimeoutSeconds": self.config.stall_timeout_seconds,
            "sessionTtlHours": self.config.session_ttl_hours,
            "cleanupIntervalSeconds": self.config.cleanup_interval_seconds,
            "disk": self.sessions.disk_status(),
            "mediaLibraries": libraries,
            "mediaRoots": [{"path": item["path"], "available": item["exists"] and item["readable"]} for item in libraries],
            "stateRoot": str(self.config.state_root),
            "currentSession": current.public(self.config) if current else None,
        }


APP: ServiceApp | None = None


class Handler(BaseHTTPRequestHandler):
    server_version = "SyLCMVCStream/0.7.0-alpha.2"

    @property
    def app(self) -> ServiceApp:
        assert APP is not None
        return APP

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"{self.address_string()} - {fmt % args}", flush=True)

    def _security_headers(self) -> None:
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
            "media-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'",
        )

    def send_json(self, status: int, payload: Any) -> None:
        body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self._security_headers()
        self.end_headers()
        self.wfile.write(body)

    def send_bytes(self, status: int, body: bytes, content_type: str, cache: str = "no-store", disposition: str | None = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", cache)
        if disposition:
            self.send_header("Content-Disposition", disposition)
        self._security_headers()
        self.end_headers()
        self.wfile.write(body)

    def send_path(self, status: int, path: Path, content_type: str, cache: str = "no-store", disposition: str | None = None) -> None:
        size = path.stat().st_size
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(size))
        self.send_header("Cache-Control", cache)
        if disposition:
            self.send_header("Content-Disposition", disposition)
        self._security_headers()
        self.end_headers()
        with path.open("rb") as source:
            shutil.copyfileobj(source, self.wfile, length=1024 * 1024)

    def read_json(self) -> dict[str, Any]:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as exc:
            raise ApiError(400, "Invalid Content-Length.") from exc
        if length <= 0 or length > 65536:
            raise ApiError(400, "Request body must contain at most 64 KiB of JSON.")
        try:
            value = json.loads(self.rfile.read(length).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ApiError(400, "Request body must be valid JSON.") from exc
        if not isinstance(value, dict):
            raise ApiError(400, "JSON request body must be an object.")
        return value

    def _request_token(self) -> str | None:
        authorization = self.headers.get("Authorization", "")
        if authorization.lower().startswith("bearer "):
            return authorization[7:].strip()
        token = self.headers.get("X-SyLC-Token")
        return token.strip() if token else None

    def require_auth(self) -> None:
        if not self.app.settings.authenticate(self._request_token()):
            raise ApiError(401, "A valid SyLC API token is required.")

    def require_setup_or_auth(self) -> None:
        if not self.app.settings.setup_required():
            self.require_auth()

    def handle_error(self, exc: Exception) -> None:
        if isinstance(exc, ApiError):
            payload: dict[str, Any] = {"ok": False, "error": exc.message}
            if exc.detail:
                payload["detail"] = exc.detail
            self.send_json(exc.status, payload)
        else:
            traceback.print_exc()
            self.send_json(500, {"ok": False, "error": "Unexpected server error.", "detail": str(exc)})

    def do_GET(self) -> None:
        try:
            parsed = urlparse(self.path)
            path = parsed.path
            if path == "/api/health":
                self.send_json(200, self.app.health())
                return
            if path == "/api/setup/status":
                self.send_json(200, self.app.setup_status())
                return
            if path == "/api/auth/check":
                self.require_auth()
                self.send_json(200, {"ok": True, "authenticated": True})
                return
            if path == "/api/filesystem/directories":
                self.require_setup_or_auth()
                query = parse_qs(parsed.query)
                raw = query.get("path", [None])[0]
                self.send_json(200, self.app.browse_directories(raw))
                return
            if path == "/api/libraries":
                self.require_auth()
                query = parse_qs(parsed.query)
                refresh = query.get("refresh", ["0"])[0] == "1"
                self.send_json(200, {"ok": True, "libraries": self.app.libraries_public(refresh_counts=refresh)})
                return
            if path == "/api/media":
                self.require_auth()
                query = parse_qs(parsed.query)
                force = query.get("refresh", ["0"])[0] == "1"
                items = self.app.catalog.scan(force=force)
                self.send_json(200, {"ok": True, "items": items, "count": len(items), "truncated": len(items) >= self.app.config.max_media_results})
                return
            if path == "/api/sessions/current":
                self.require_auth()
                session = self.app.sessions.current()
                self.send_json(200, {"ok": True, "session": session.public(self.app.config) if session else None})
                return
            if path == "/api/sessions/current/report":
                self.require_auth()
                session = self.app.sessions.current()
                if not session:
                    raise ApiError(404, "There is no current session.")
                self.app.sessions._mirror_report(session)
                if not session.report_path.is_file():
                    raise ApiError(404, "The diagnostic report is not available yet.")
                self.send_path(200, session.report_path, "text/plain; charset=utf-8", disposition=f'attachment; filename="sylc-session-{session.id}-report.txt"')
                return
            match = re.fullmatch(r"/hls/([0-9a-f]{16})/stream\.m3u8", path)
            if match:
                body = self.app.sessions.playlist_bytes(match.group(1))
                self.send_bytes(200, body, "application/vnd.apple.mpegurl", cache="no-store")
                return
            match = re.fullmatch(r"/hls/([0-9a-f]{16})/([A-Za-z0-9_.-]+)", path)
            if match:
                file_path = self.app.sessions.hls_file(match.group(1), match.group(2))
                content_type = "video/mp2t" if file_path.suffix.lower() == ".ts" else "application/octet-stream"
                self.send_path(200, file_path, content_type, cache="private, max-age=30")
                return
            if path in {"/", "/index.html"}:
                self.send_bytes(200, (STATIC_DIR / "index.html").read_bytes(), "text/html; charset=utf-8")
                return
            if path.startswith("/static/"):
                name = Path(path.removeprefix("/static/")).name
                file_path = STATIC_DIR / name
                if not file_path.is_file() or file_path.parent != STATIC_DIR:
                    raise ApiError(404, "Static file not found.")
                ctype = mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
                if ctype.startswith("text/") or ctype in {"application/javascript", "application/json"}:
                    ctype += "; charset=utf-8"
                self.send_bytes(200, file_path.read_bytes(), ctype, cache="no-cache")
                return
            raise ApiError(404, "Not found.")
        except Exception as exc:
            self.handle_error(exc)

    def do_POST(self) -> None:
        try:
            path = urlparse(self.path).path
            if path == "/api/setup":
                body = self.read_json()
                item = self.app.settings.initial_setup(body)
                self.app.catalog.invalidate()
                self.send_json(201, {"ok": True, "library": item.public(), "tokenConfigured": self.app.settings.token_configured()})
                return
            if path == "/api/libraries/test":
                self.require_setup_or_auth()
                self.send_json(200, self.app.test_library(self.read_json()))
                return
            if path == "/api/libraries":
                self.require_auth()
                item = self.app.create_library(self.read_json())
                self.send_json(201, {"ok": True, "library": item.public()})
                return
            match = re.fullmatch(r"/api/libraries/([0-9a-f]{16})/rescan", path)
            if match:
                self.require_auth()
                self.app.settings.get(match.group(1))
                self.app.catalog.scan(force=True)
                self.send_json(200, {"ok": True, "libraries": self.app.libraries_public(refresh_counts=False)})
                return
            if path == "/api/settings/token":
                self.require_auth()
                body = self.read_json()
                self.app.settings.set_token(body.get("apiToken", ""))
                self.send_json(200, {"ok": True, "tokenConfigured": self.app.settings.token_configured()})
                return
            self.require_auth()
            if path == "/api/media/probe":
                body = self.read_json()
                media_id = body.get("mediaId")
                if not isinstance(media_id, str):
                    raise ApiError(400, "mediaId is required.")
                self.send_json(200, {"ok": True, "media": self.app.sessions.inspect_media(media_id)})
                return
            if path == "/api/sessions":
                session = self.app.sessions.create(self.read_json())
                self.send_json(202, {"ok": True, "session": session.public(self.app.config)})
                return
            if path == "/api/sessions/current/seek":
                previous, session = self.app.sessions.seek(self.read_json())
                self.send_json(202, {
                    "ok": True,
                    "previousSessionId": previous.id,
                    "previousPlaylistUrl": f"/hls/{previous.id}/stream.m3u8" if previous.playable else None,
                    "session": session.public(self.app.config),
                })
                return
            if path == "/api/sessions/current/stop":
                session = self.app.sessions.stop()
                self.send_json(202, {"ok": True, "session": session.public(self.app.config)})
                return
            if path == "/api/sessions/cleanup":
                result = self.app.sessions.cleanup()
                self.send_json(200, {"ok": True, **result})
                return
            raise ApiError(404, "Not found.")
        except Exception as exc:
            self.handle_error(exc)

    def do_PUT(self) -> None:
        try:
            path = urlparse(self.path).path
            match = re.fullmatch(r"/api/libraries/([0-9a-f]{16})", path)
            if not match:
                raise ApiError(404, "Not found.")
            self.require_auth()
            item = self.app.update_library(match.group(1), self.read_json())
            self.send_json(200, {"ok": True, "library": item.public()})
        except Exception as exc:
            self.handle_error(exc)

    def do_DELETE(self) -> None:
        try:
            path = urlparse(self.path).path
            match = re.fullmatch(r"/api/libraries/([0-9a-f]{16})", path)
            if not match:
                raise ApiError(404, "Not found.")
            self.require_auth()
            self.app.delete_library(match.group(1))
            self.send_json(200, {"ok": True})
        except Exception as exc:
            self.handle_error(exc)


def main() -> None:
    global APP
    config = Config.from_env()
    APP = ServiceApp(config)
    server = ThreadingHTTPServer((config.bind_host, config.port), Handler)
    server.daemon_threads = True
    print(f"SyLC MVC Stream Service {APP_VERSION} listening on http://{config.bind_host}:{config.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        current = APP.sessions.current()
        if current and current.state in {"starting", "running", "buffering", "playable", "stopping"}:
            APP.sessions.stop()
        APP.sessions.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
