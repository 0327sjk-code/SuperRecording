"""Independent FFmpeg decode validation; reads only synthetic test media."""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np


def probe(path: Path) -> dict:
    return json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)
    ]))


def decode(path: Path) -> np.ndarray:
    data = subprocess.check_output([
        "ffmpeg", "-v", "error", "-i", str(path), "-map", "0:v:0", "-vf", "scale=320:180",
        "-fps_mode", "passthrough", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"
    ])
    return np.frombuffer(data, dtype=np.uint8).reshape(-1, 180, 320, 3).astype(np.int16)


def active_ranges(mask: np.ndarray) -> list[list[int]]:
    indices = np.flatnonzero(mask)
    groups = np.split(indices, np.flatnonzero(np.diff(indices) > 1) + 1)
    return [[int(group[0]), int(group[-1])] for group in groups if group.size]


def main() -> None:
    root = Path(sys.argv[1])
    for name in ["marked.mp4", "edited.mp4", "trimmed.mp4", "trimmed-audio.mp4",
                 "quality50.mp4", "quality50-speed2.mp4", "slow-audio.mp4", "marked.gif"]:
        path = root / name
        info = probe(path)
        streams = info["streams"]
        video = next(stream for stream in streams if stream["codec_type"] == "video")
        frames = decode(path)
        red, green, blue = np.moveaxis(frames, -1, 0)
        red_pixels = ((red > green + 40) & (red > blue + 20) & (red > 120)).sum(axis=(1, 2))
        green_pixels = ((green > red + 55) & (green > blue + 35) & (green > 160)).sum(axis=(1, 2))
        has_red = red_pixels > 30
        has_green = green_pixels > 30
        combined = has_red | has_green
        expected = np.zeros(len(frames), dtype=bool)
        if name in ("marked.mp4", "edited.mp4", "quality50.mp4", "quality50-speed2.mp4"):
            assert len(frames) == 360, (name, "missing frames", len(frames))
            for begin in (60, 120, 180, 240):
                expected[begin:begin + 30] = True
            mismatch = np.flatnonzero(expected != combined)
            # Source/sample rounding may move only the boundary by one frame.
            assert all(any(abs(int(i) - boundary) <= 1 for boundary in
                           (60, 90, 120, 150, 180, 210, 240, 270)) for i in mismatch), (name, mismatch)
            assert combined[65] and combined[125] and combined[185] and combined[245]
            assert not combined[10] and not combined[110] and not combined[290]
        if name == "quality50-speed2.mp4":
            assert video["width"] == 640 and video["height"] == 360
            assert abs(float(video["duration"]) - 3) < 0.01
        if name == "marked.gif":
            assert len(frames) == 90, ("GIF frame count", len(frames))
            for index in (18, 33, 48, 63):
                assert combined[index], ("GIF annotation missing", index)
        if "audio" in name:
            audio = next(stream for stream in streams if stream["codec_type"] == "audio")
            assert abs(float(audio["duration"]) - float(video["duration"])) < 0.08
            pcm = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(path),
                "-map", "0:a:0", "-f", "f32le", "-ac", "1", "-"])
            samples = np.frombuffer(pcm, dtype=np.float32)
            assert np.isfinite(samples).all() and 0.02 < np.sqrt(np.mean(samples * samples)) < 0.12
        print(json.dumps({"file": name, "frames": len(frames), "red_ranges": active_ranges(has_red),
                          "green_ranges": active_ranges(has_green), "size": [video["width"], video["height"]]}))
    original = decode(root / "source.mp4")
    output = decode(root / "marked.mp4")
    for index in (0, 10, 40, 340, 359):
        assert np.array_equal(original[index], output[index]), ("Untouched frame not bit-exact", index)
    print("PASS: independent decode, annotation time ranges, original-frame passthrough, GIF, speed, quality and audio")


if __name__ == "__main__":
    main()
