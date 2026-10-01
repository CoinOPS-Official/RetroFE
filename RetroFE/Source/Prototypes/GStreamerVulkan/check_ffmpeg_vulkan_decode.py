"""Compare software and Vulkan decoded frame hashes for 8-bit H.264/HEVC videos.

Run serially so a collection scan does not compete with RetroFE for the GPU.
This is an offline diagnostic; it does not change the videos.
"""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, errors="replace")


def frame_hashes(ffmpeg: str, path: Path, hardware: bool,
                 max_frames: int | None = None) -> list[str]:
    command = [ffmpeg, "-hide_banner", "-loglevel", "error"]
    if hardware:
        command += ["-init_hw_device", "vulkan=vk", "-hwaccel", "vulkan",
                    "-hwaccel_device", "vk", "-hwaccel_output_format", "vulkan"]
    command += ["-i", str(path), "-map", "0:v:0", "-an", "-vf",
                "hwdownload,format=nv12" if hardware else "format=nv12"]
    if max_frames is not None:
        command += ["-frames:v", str(max_frames)]
    command += ["-f", "framemd5", "-"]
    result = run(command)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"FFmpeg exited {result.returncode}")
    return [line.rsplit(",", 1)[-1].strip() for line in result.stdout.splitlines()
            if line and not line.startswith("#")]


def video_format(ffprobe: str, path: Path) -> tuple[str, str]:
    result = run([ffprobe, "-v", "error", "-select_streams", "v:0",
                  "-show_entries", "stream=codec_name,pix_fmt", "-of", "json",
                  str(path)])
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or "ffprobe failed")
    stream = json.loads(result.stdout)["streams"][0]
    return stream.get("codec_name", ""), stream.get("pix_fmt", "")


def inputs(paths: list[Path], mp4_only: bool):
    seen = set()
    extensions = {".mp4"} if mp4_only else {".mp4", ".mkv", ".mov"}
    for path in paths:
        files = (sorted(p for p in path.rglob("*") if p.suffix.lower() in
                        extensions) if path.is_dir() else
                 [path] if path.suffix.lower() in extensions else [])
        for file in files:
            resolved = file.resolve()
            if resolved not in seen:
                seen.add(resolved)
                yield resolved


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path,
                        help="video files or folders to scan recursively")
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg"),
                        help="Vulkan-enabled ffmpeg.exe path")
    parser.add_argument("--max-frames", type=int,
                        help="check only the first N frames of each video")
    parser.add_argument("--mp4-only", action="store_true",
                        help="include only .mp4 files")
    args = parser.parse_args()
    if args.max_frames is not None and args.max_frames < 1:
        parser.error("--max-frames must be positive")
    if not args.ffmpeg:
        parser.error("pass --ffmpeg with a Vulkan-enabled FFmpeg build")
    ffprobe = str(Path(args.ffmpeg).with_name("ffprobe.exe" if sys.platform == "win32"
                                                else "ffprobe"))
    failures = 0
    for path in inputs(args.paths, args.mp4_only):
        try:
            codec, pixel_format = video_format(ffprobe, path)
            if codec not in {"h264", "hevc"} or pixel_format != "yuv420p":
                print(f"SKIP {path}: {codec}/{pixel_format}", flush=True)
                continue
            software = frame_hashes(args.ffmpeg, path, False, args.max_frames)
            vulkan = frame_hashes(args.ffmpeg, path, True, args.max_frames)
            mismatches = [i for i, (sw, vk) in enumerate(zip(software, vulkan))
                          if sw != vk]
            if len(software) != len(vulkan) or mismatches:
                failures += 1
                print(f"FAIL {path}: software={len(software)} Vulkan={len(vulkan)} "
                      f"different={len(mismatches)} first={mismatches[:8]}", flush=True)
            else:
                scope = "checked" if args.max_frames is not None else "total"
                print(f"PASS {path}: {len(software)} {scope} frames", flush=True)
        except (OSError, ValueError, IndexError, RuntimeError) as error:
            failures += 1
            print(f"ERROR {path}: {error}", file=sys.stderr, flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
