#!/usr/bin/env python3
"""Source-level and ranking-model checks for the Linux Blu-ray ISO port."""
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ISO = ROOT / "engine/phase6-streaming/src/iso"
libbluray = (ISO / "sylc_libbluray_probe.cpp").read_text(encoding="utf-8")
feature = (ISO / "sylc_iso_feature_posix.cpp").read_text(encoding="utf-8")
source = (ISO / "sylc_iso_source.cpp").read_text(encoding="utf-8")
runner = (ROOT / "engine/run-phase6-streaming-session.sh").read_text(encoding="utf-8")

checks = {
    "TITLES_ALL selector": "kTitlesAll = 0" in libbluray,
    "replay decoy threshold": "kReplayDecoyRatio = 1.5" in libbluray,
    "MVC title preference": "candidate.has_mvc" in libbluray and "return 5" in libbluray,
    "authoritative UDF mapping": "libbluray authoritative TITLES_ALL MVC/decoy selector" in feature,
    "single clip physical duration fallback": "applySingleClipPhysicalDurationFallback" in feature,
    "base CLPI seek table": "setBaseSeekTable" in source,
    "timeline origin": "setTimelineOriginMs" in source,
    "exact SSIF seek table": "setSsifSeekTable" in source,
    "video seek planner": "--plan-video-seek" in source and r'\"skipPairs\"' in source,
    "runner uses planned preroll": "--plan-video-seek" in runner and '--skip-pairs "$SKIP_PAIRS"' in runner,
    "audio aligned to first emitted video": '--start-seconds "$EFFECTIVE_START"' in runner,
}

@dataclass
class Candidate:
    playlist: int
    duration: float
    unique_duration: float
    clips: int
    unique_items: int
    h264: bool = True
    audio: int = 1
    mvc: bool = False

    @property
    def repeated(self):
        return max(0, self.clips - self.unique_items)

    @property
    def decoy(self):
        return self.repeated > 0 and self.unique_duration > 0 and self.duration > self.unique_duration * 1.5

    @property
    def selection_class(self):
        playable = self.duration > 0 and self.h264 and self.audio > 0
        if playable and not self.decoy and self.mvc: return 5
        if playable and not self.decoy: return 4
        if playable and self.decoy and self.mvc: return 3
        if playable and self.decoy: return 2
        if not self.decoy and self.h264: return 1
        return 0


def select(candidates):
    return max(candidates, key=lambda c: (c.selection_class, c.duration, c.unique_duration, c.clips))

# Known Android-development regression shape: a huge replay-loop playlist must
# rank below the genuine feature despite its inflated nominal duration.
decoy = Candidate(20, 21035.096, 289.373, 152, 2, mvc=True)
feature_title = Candidate(800, 7251.2, 7251.2, 1, 1, audio=4, mvc=True)
checks["known replay-loop decoy rejected"] = decoy.decoy and select([decoy, feature_title]).playlist == 800
single = Candidate(0, 153.278, 153.278, 1, 1, audio=7, mvc=True)
checks["single-title fallback candidate preserved"] = not single.decoy and select([single]).playlist == 0

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL"), name)
if failed:
    raise SystemExit("ISO logic smoke failed: " + ", ".join(failed))
print("ISO logic smoke: PASS")
