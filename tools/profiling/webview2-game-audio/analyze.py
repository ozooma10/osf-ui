"""Analyze saved game/endpoint observations without controlling the game.

Usage: python analyze.py RUN_DIRECTORY
Required meter labels are listed in TONE_PHASES. Missing evidence is pending
(exit 2), failed evidence is exit 1, and complete required evidence is exit 0.
Optional video and lifecycle records never turn missing required evidence into a pass.
"""
import argparse
import datetime as dt
import hashlib
import json
import math
from pathlib import Path


TONE_PHASES = (
    "quiet-background", "one-tv", "fullscreen", "after-close", "tv-left", "tv-right",
    "fixed-master-full", "fixed-master-half", "fixed-master-restored",
    "distant", "out-of-range", "muted", "unmuted", "two-tvs",
    "remaining-tv", "no-tvs", "restored-tvs",
)
SILENT_PHASES = {"quiet-background", "out-of-range", "muted", "no-tvs"}
VIDEO_PHASES = ("video-world", "video-fullscreen", "video-after-close")


def finite(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def vector(value):
    return isinstance(value, list) and len(value) == 3 and all(map(finite, value))


def norm(value):
    return math.sqrt(sum(x * x for x in value))


def subtract(a, b):
    return [x - y for x, y in zip(a, b)]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def ratio(numerator, denominator):
    return numerator / denominator if finite(numerator) and finite(denominator) and denominator > 0 else None


def utc(value):
    return dt.datetime.fromisoformat(value.replace("Z", "+00:00"))


class Analyzer:
    def __init__(self, run):
        self.run = run
        self.checks = []
        self.inputs = {}
        self.phases = {}
        self.metrics = {}

    def add(self, name, passed, expected, actual=None, scope="required"):
        self.checks.append(dict(name=name, status="pass" if passed else "fail", scope=scope,
                                expected=expected, actual=actual))

    def pending(self, name, reason, scope="required"):
        self.checks.append(dict(name=name, status="pending", scope=scope, reason=reason))

    def skip(self, name, reason, actual=None):
        self.checks.append(dict(name=name, status="skip", scope="conditional", reason=reason, actual=actual))

    def read(self, relative, required=True):
        path = self.run / relative
        if not path.is_file():
            if required:
                self.pending(str(relative), "Evidence file has not been recorded.")
            return None
        data = path.read_bytes()
        self.inputs[str(relative)] = hashlib.sha256(data).hexdigest()
        try:
            value = json.loads(data.decode("utf-8-sig"))
            if not isinstance(value, dict):
                raise ValueError("Evidence root must be an object")
            return value
        except (ValueError, UnicodeError) as error:
            self.add(str(relative), False, "Evidence is complete JSON.", str(error))
            return None

    @staticmethod
    def route(snapshot, content=False):
        audio = snapshot.get("audio") or (snapshot.get("page") or {}).get("browserState", {}).get("audio", {})
        return audio if content else audio.get("chrome", {})

    def load_phase(self, name, video=False):
        values = [self.read(f"meter-{name}/{file}.json") for file in ("before", "result", "after")]
        if any(value is None for value in values):
            return
        before, report, after = values
        meter = report.get("endpoint", {})
        route_before, route_after = self.route(before, video), self.route(after, video)
        scene = route_after.get("spatial", {})
        phase = dict(before=before, after=after, report=report, meter=meter,
                     route_before=route_before, route=route_after, scene=scene)
        self.phases[name] = phase
        silent = name in SILENT_PHASES
        fmt = meter.get("format", {})
        count, rate = fmt.get("channels"), fmt.get("sampleRate")
        valid_format = isinstance(count, int) and count > 0 and isinstance(rate, int) and rate > 0
        valid_arrays = valid_format and all(
            isinstance(meter.get(key), list) and len(meter[key]) == count and
            all(finite(v) and v >= 0 for v in meter[key])
            for key in ("channelRms", "channelToneRms", "channelPeak"))
        valid_numbers = all(finite(meter.get(key)) and meter[key] >= 0
                            for key in ("rms", "toneRms", "peak", "wallSeconds", "seconds", "frames"))
        healthy = (report.get("probe") == "audio-endpoint-meter" and report.get("outcome") == "measured" and
                   report.get("exitCode") == 0 and meter.get("ready") is True and
                   meter.get("running") is True and meter.get("error") is None and
                   all(meter.get(key) == 0 for key in ("discontinuities", "timestampErrors", "invalidSamples")) and
                   valid_arrays and valid_numbers and meter.get("wallSeconds", 0) >= 2)
        if not silent:
            healthy = healthy and valid_format and meter.get("frames", 0) >= rate * 2
            if not video:
                healthy = healthy and meter.get("toneBlocks", 0) >= 10
        self.add(f"{name}: healthy endpoint measurement", healthy,
                 "Measured >=2 seconds without meter errors; active phases include >=2 seconds of PCM.",
                 {key: meter.get(key) for key in ("wallSeconds", "frames", "toneBlocks", "error", "discontinuities", "timestampErrors")})
        self.add(f"{name}: expected tone frequency", video or report.get("toneHz") == 997,
                 "Controlled-tone measurements analyze 997 Hz.", report.get("toneHz"))
        try:
            bracketed = (utc(before["atUtc"]) <= utc(report["measurementStartedAtUtc"]) <=
                         utc(report["measurementEndedAtUtc"]) <= utc(after["atUtc"]))
        except (KeyError, ValueError, TypeError):
            bracketed = False
        self.add(f"{name}: observations bracket measurement", bracketed,
                 "Native/page snapshots surround the endpoint observation interval.")
        pages = [item.get("page") or {} for item in (before, after)]
        same_document = bool(pages[0].get("documentId")) and pages[0].get("documentId") == pages[1].get("documentId")
        self.add(f"{name}: same fresh document", same_document and all(
            isinstance(page.get("reportedTickMs"), (float, int)) for page in pages),
            "Both Read-WorldPage snapshots identify the same fresh trusted document.",
            [page.get("documentId") for page in pages])
        if name != "quiet-background":
            self.add(f"{name}: fresh native route", all(
                route.get("route") == "webview2-tab-capture" and route.get("error") == "" and
                route.get("fresh") is True and route.get("spatial", {}).get("fresh") is True
                for route in (route_before, route_after)),
                "The production route receives fresh validated scenes and reports no capture error.",
                [{key: route.get(key) for key in ("state", "fresh", "muted", "error")} for route in (route_before, route_after)])
            self.add(f"{name}: expected output state", all(
                route.get("state") == ("silent" if silent else "capturing") for route in (route_before, route_after)),
                "Native routing status agrees with the active or silent phase.")
        if not video:
            probes = [page.get("audioProbe") or {} for page in pages]
            if name == "quiet-background":
                self.add(f"{name}: fixture tone stopped", all(probe.get("state") == "closed" and probe.get("toneHz") == 0 for probe in probes),
                         "The quiet reference is taken without the fixture oscillator.")
            else:
                self.add(f"{name}: fixture tone continues", all(probe.get("state") == "running" for probe in probes) and
                         finite(probes[0].get("time")) and finite(probes[1].get("time")) and
                         probes[1]["time"] > probes[0]["time"],
                         "The trusted fixture reports running and advancing across the measurement.", probes)

    def compare_layouts(self):
        reference = self.phases.get("one-tv")
        if not reference:
            return
        reference = reference["meter"]
        for name, phase in self.phases.items():
            meter = phase["meter"]
            self.add(f"{name}: same output device", bool(meter.get("endpointId")) and
                     meter.get("endpointId") == reference.get("endpointId") and meter.get("format") == reference.get("format"),
                     "The endpoint and format match the one-TV reference.", meter.get("endpointId"))

    def geometry_matches(self, first, second):
        for key in ("listener", "front", "up"):
            if not vector(first.get(key)) or not vector(second.get(key)) or norm(subtract(first[key], second[key])) > 0.025:
                return False
        emitters_a = {emitter["id"]: emitter["position"] for emitter in first.get("emitters", [])}
        emitters_b = {emitter["id"]: emitter["position"] for emitter in second.get("emitters", [])}
        return (emitters_a.keys() == emitters_b.keys() and bool(emitters_a) and
                all(norm(subtract(position, emitters_b[key])) < 0.025 for key, position in emitters_a.items()))

    def analyze_tones(self):
        reference = self.phases.get("one-tv")
        if reference:
            base = reference["meter"].get("toneRms")
            self.add("one-tv: audible controlled tone", finite(base) and base > 1e-4,
                     "The one-TV reference contains a measurable 997 Hz tone.", base)
            self.add("one-tv: one emitter", reference["scene"].get("emitterCount") == 1,
                     "The baseline contains exactly one actual TV emitter.")
            for name, phase in self.phases.items():
                if name in VIDEO_PHASES:
                    continue
                level = phase["meter"].get("toneRms")
                if name in SILENT_PHASES:
                    measured = ratio(level, base)
                    self.add(f"{name}: controlled tone silent", measured is not None and measured < 0.01,
                             "997 Hz energy is below 1% of the audible reference; unrelated game sound may remain.",
                             {"toneRms": level, "reference": base, "ratio": measured})
                else:
                    self.add(f"{name}: controlled tone audible", finite(level) and level > 1e-4,
                             "The controlled source remains measurable at the output endpoint.", level)
        for name in ("tv-left", "tv-right"):
            if name not in self.phases:
                continue
            phase = self.phases[name]
            scene, meter = phase["scene"], phase["meter"]
            self.add(f"{name}: stable measured geometry", self.geometry_matches(phase["route_before"].get("spatial", {}), scene),
                     "The camera basis and TV geometry stay fixed across the panning measurement.")
            emitters = scene.get("emitters", [])
            valid = (len(emitters) == 1 and all(vector(scene.get(key)) for key in ("listener", "front", "up")) and
                     vector(emitters[0].get("position")))
            side = lateral_fraction = None
            if valid:
                displacement = subtract(emitters[0]["position"], scene["listener"])
                side = dot(cross(scene["front"], scene["up"]), displacement)
                lateral_fraction = ratio(abs(side), norm(displacement))
            self.add(f"{name}: lateral camera geometry", lateral_fraction is not None and lateral_fraction > 0.4,
                     "A single TV lies clearly to one side in the actual camera basis.",
                     {"sideMeters": side, "lateralFraction": lateral_fraction})
            values = meter.get("channelToneRms", [])
            fmt = meter.get("format", {})
            stereo = isinstance(fmt.get("channelMask"), int) and fmt["channelMask"] & 3 == 3 and len(values) >= 2
            favored = 1 if side is not None and side > 0 else 0
            measured = ratio(values[favored], values[1 - favored]) if stereo else None
            self.add(f"{name}: pan follows actual camera basis", valid and stereo and lateral_fraction is not None and
                     lateral_fraction > 0.4 and values[favored] > 1e-4 and values[favored] > 1.3 * values[1 - favored],
                     "The camera-relative side has at least 1.3 times the opposite channel's 997 Hz energy.",
                     {"favoredChannel": "right" if favored else "left", "channelToneRms": values, "ratio": measured})
            self.metrics[name] = dict(sideMeters=side, channelRatio=measured)
        if all(name in self.metrics for name in ("tv-left", "tv-right")):
            sides = [self.metrics[name]["sideMeters"] for name in ("tv-left", "tv-right")]
            self.add("panning covers both camera-relative sides", all(map(finite, sides)) and sides[0] * sides[1] < 0,
                     "The two panning measurements place a TV on opposite sides of the listener.", sides)
        for name, count in (("two-tvs", 2), ("remaining-tv", 1), ("no-tvs", 0), ("restored-tvs", 2), ("out-of-range", 0)):
            if name in self.phases:
                scene = self.phases[name]["scene"]
                emitters = scene.get("emitters", [])
                self.add(f"{name}: expected emitter count", scene.get("emitterCount") == count and len(emitters) == count and
                         len({emitter.get("id") for emitter in emitters}) == count,
                         f"The validated native scene contains {count} distinct TV emitter(s).", emitters)
        for name, muted in (("muted", True), ("unmuted", False)):
            if name in self.phases:
                scene = self.phases[name]["scene"]
                self.add(f"{name}: logical mute state", self.phases[name]["route"].get("muted") is muted and
                         scene.get("muted") is muted and scene.get("emitterCount", 0) > 0,
                         "Logical mute toggles a nonempty, fresh source scene.")
        names = ("fixed-master-full", "fixed-master-half", "fixed-master-restored")
        if all(name in self.phases for name in names):
            first = self.phases[names[0]]
            for name in names:
                phase = self.phases[name]
                self.add(f"{name}: stable measured geometry", self.geometry_matches(phase["route_before"].get("spatial", {}), phase["scene"]),
                         "Each gain sample has stable camera and emitter geometry before and after measurement.")
            for name in names[1:]:
                phase = self.phases[name]
                expected = ratio(phase["scene"].get("gain"), first["scene"].get("gain"))
                measured = ratio(phase["meter"].get("toneRms"), first["meter"].get("toneRms"))
                self.add(f"{name}: fixed listening geometry", self.geometry_matches(first["scene"], phase["scene"]),
                         "Camera basis, listener and emitter positions remain fixed for the gain comparison.")
                desired = 0.5 if name.endswith("half") else 1.0
                self.add(f"{name}: intended master gain", expected is not None and abs(expected - desired) < 0.01,
                         f"The native scene reports {desired} times baseline master gain.", expected)
                self.add(f"{name}: output follows master gain", expected is not None and measured is not None and
                         abs(measured - expected) <= 0.03 + expected * 0.05,
                         "Measured tone ratio follows reported gain within 5% plus 0.03.",
                         {"expected": expected, "measured": measured})
                self.metrics[name] = dict(expectedGainRatio=expected, measuredToneRatio=measured)
        if "distant" in self.phases and reference:
            distant = self.phases["distant"]
            near_emitters, far_emitters = reference["scene"].get("emitters", []), distant["scene"].get("emitters", [])
            distances_ok = (len(near_emitters) == len(far_emitters) == 1 and
                            finite(near_emitters[0].get("distance")) and finite(far_emitters[0].get("distance")))
            measured = ratio(distant["meter"].get("toneRms"), reference["meter"].get("toneRms"))
            self.add("distant: greater actual distance", distances_ok and far_emitters[0]["distance"] > near_emitters[0]["distance"] + 5,
                     "The same single TV is at least five metres farther from the actual listener.",
                     {"near": near_emitters, "far": far_emitters})
            self.add("distant: same emitter and master gain", distances_ok and near_emitters[0].get("id") == far_emitters[0].get("id") and
                     finite(reference["scene"].get("gain")) and finite(distant["scene"].get("gain")) and
                     abs(reference["scene"]["gain"] - distant["scene"]["gain"]) < 0.001,
                     "Distance comparison retains the same TV and master-volume setting.")
            self.add("distant: audible attenuation", measured is not None and 0 < measured < 0.5,
                     "The farther TV's controlled tone is below half of the near reference.", measured)
            self.metrics["distanceToneRatio"] = measured

    def analyze_camera(self):
        before = self.read("snapshot-free-camera-before.json")
        after = self.read("snapshot-free-camera-player-moved.json")
        if before is None or after is None:
            return
        first, second = self.route(before).get("spatial", {}), self.route(after).get("spatial", {})
        player_a, player_b = before.get("game", {}).get("position", {}), after.get("game", {}).get("position", {})
        player_distance = norm([player_b[key] - player_a[key] for key in "xyz"]) if all(
            finite(player_a.get(key)) and finite(player_b.get(key)) for key in "xyz") else None
        listener_distance = norm(subtract(first["listener"], second["listener"])) if (
            vector(first.get("listener")) and vector(second.get("listener"))) else None
        self.add("world camera remains independent of player movement", player_distance is not None and player_distance > 9.9 and
                 listener_distance is not None and listener_distance < 0.025 and first.get("fresh") is True and second.get("fresh") is True,
                 "Player moves ten metres while the actual camera listener remains within 2.5 cm.",
                 {"playerDistance": player_distance, "listenerDistance": listener_distance})

    def analyze_watch_close(self):
        names = ("one-tv", "fullscreen", "after-close")
        for name in names:
            if name not in self.phases:
                continue
            phase = self.phases[name]
            snapshots = [phase["before"], phase["after"]]
            expected_fullscreen = name == "fullscreen"
            self.add(f"{name}: actual fullscreen state", all(
                (snapshot.get("page") or {}).get("browserState", {}).get("fullscreen") is expected_fullscreen
                for snapshot in snapshots),
                f"Both browser snapshots report fullscreen={expected_fullscreen}.")
            self.add(f"{name}: gameplay pause state", all(
                finite(snapshot.get("game", {}).get("pauseRequestCount")) and
                (snapshot["game"]["pauseRequestCount"] >= 1 if expected_fullscreen else snapshot["game"]["pauseRequestCount"] == 0)
                for snapshot in snapshots),
                "Watch holds a game pause request; world playback and close have none.",
                [snapshot.get("game", {}).get("pauseRequestCount") for snapshot in snapshots])
            if name != "one-tv":
                self.add(f"{name}: interaction state", all(
                    ((snapshot.get("page") or {}).get("interaction") or {}).get("active") is expected_fullscreen
                    for snapshot in snapshots),
                    "Actual UI interaction starts for Watch and ends after browser.close.")
        if not all(name in self.phases for name in names):
            return
        snapshots = [self.phases[name][which] for name in names for which in ("before", "after")]
        pages = [snapshot.get("page") or {} for snapshot in snapshots]
        documents = [page.get("documentId") for page in pages]
        probes = [page.get("audioProbe") or {} for page in pages]
        reply_ids = [(probe.get("reply") or {}).get("id") for probe in probes]
        times = [probe.get("time") for probe in probes]
        self.add("Watch/close preserves source document", all(isinstance(value, str) and value for value in documents) and
                 len(set(documents)) == 1,
                 "One trusted source document survives the world, fullscreen and post-close observations.", documents)
        self.add("Watch/close preserves running source context", all(
            probe.get("state") == "running" and (probe.get("reply") or {}).get("action") == "start" and
            (probe.get("reply") or {}).get("ok") is True for probe in probes) and
            all(isinstance(value, str) and value for value in reply_ids) and len(set(reply_ids)) == 1 and
            all(map(finite, times)) and all(second >= first for first, second in zip(times, times[1:])),
            "The same acknowledged tone start remains running, with nondecreasing context time through Watch/close.",
            {"startReplyIds": reply_ids, "sourceTimes": times})
        reference = self.phases["one-tv"]
        baseline_scene = reference["scene"]
        for name in ("fullscreen", "after-close"):
            phase = self.phases[name]
            scenes = [reference["route_before"].get("spatial", {}), baseline_scene,
                      phase["route_before"].get("spatial", {}), phase["scene"]]
            stable = all(self.geometry_matches(baseline_scene, scene) and
                         finite(scene.get("gain")) and finite(baseline_scene.get("gain")) and
                         abs(scene["gain"] - baseline_scene["gain"]) < 0.001 and
                         scene.get("muted") is False for scene in scenes)
            measured = ratio(phase["meter"].get("toneRms"), reference["meter"].get("toneRms"))
            self.metrics[f"{name}-continuity"] = dict(stableGeometry=stable, toneRatio=measured)
            if stable:
                self.add(f"{name}: fixed-geometry tone continuity", measured is not None and 0.85 <= measured <= 1.15,
                         "With matching camera, emitters and gain, output remains within 15% of world playback.", measured)
            else:
                self.skip(f"{name}: fixed-geometry tone continuity",
                          "Camera/emitter geometry or master gain changed; a level ratio cannot isolate Watch/close continuity.",
                          {"toneRatio": measured, "listenerBefore": baseline_scene.get("listener"),
                           "listenerAfter": phase["scene"].get("listener")})

    def run_analysis(self):
        for name in TONE_PHASES:
            self.load_phase(name)
        for name in VIDEO_PHASES:
            if (self.run / f"meter-{name}").exists():
                self.load_phase(name, video=True)
        self.compare_layouts()
        self.analyze_tones()
        self.analyze_camera()
        self.analyze_watch_close()
        counts = {status: sum(check["status"] == status for check in self.checks) for status in ("pass", "fail", "pending", "skip")}
        status = "fail" if counts["fail"] else "incomplete" if counts["pending"] else "pass-with-skips" if counts["skip"] else "pass"
        return dict(schema=1, analyzer="webview2-game-spatial-audio", run=str(self.run.resolve()), status=status,
                    counts=counts, metrics=self.metrics, checks=self.checks, inputSha256=self.inputs,
                    unreviewedMeterPhases=sorted(path.name[6:] for path in self.run.glob("meter-*")
                                                if path.is_dir() and path.name[6:] not in TONE_PHASES + VIDEO_PHASES),
                    limits=[
                        "Endpoint measurements include all desktop/game sound; the 997 Hz tone reduces interference but is not process attribution.",
                        "Native spatial diagnostics show validated inputs, not captured PCM, per-emitter output or proof of continuous capture during silence.",
                        "Before/after snapshots bound each observation; they do not prove every intervening audio frame or scene update.",
                        "Distance comparison establishes attenuation, not an exact curve when HRTF bearing changes.",
                        "Video, renderer crashes, device switching, save/load and cell retirement require separately reviewed evidence.",
                    ])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    if not args.run.is_dir():
        parser.error("Run directory does not exist")
    result = Analyzer(args.run).run_analysis()
    output = args.run / "audio-results.json"
    output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("status", "counts", "metrics")}, indent=2))
    print(f"Evidence: {output}")
    for check in result["checks"]:
        if check["status"] == "fail":
            print(f"FAIL: {check['name']}: {check.get('actual')}")
    return 1 if result["counts"]["fail"] else 2 if result["counts"]["pending"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
