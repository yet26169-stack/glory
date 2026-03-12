#!/usr/bin/env python3
"""
mp4_to_atlas.py  –  Convert an MP4 VFX video to a sprite-sheet texture atlas.

Pipeline:
  1. Extract N frames from the video (evenly spaced within a time window)
  2. Center-crop each frame to a square
  3. Generate an alpha channel from luminance (bright VFX → opaque, dark bg → transparent)
  4. Scale each frame to the target cell size
  5. Pack all frames into a grid atlas PNG

Usage:
  python3 tools/mp4_to_atlas.py \
    --input  models/abiliities_models/explosion_e.mp4 \
    --output assets/textures/explosion_e_atlas.png \
    --frames 48 --grid 8 --cell-size 256 \
    --start-time 0.5 --end-time 4.0

Requirements: ffmpeg (in PATH), Pillow
"""

import argparse
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image


def extract_frames(input_path: str, out_dir: str, n_frames: int,
                   start: float, end: float) -> list[str]:
    """Use ffmpeg to extract *n_frames* evenly spaced in [start, end]."""
    duration = end - start
    fps = n_frames / duration  # output framerate to get exactly n_frames

    cmd = [
        "ffmpeg", "-y",
        "-ss", f"{start:.4f}",
        "-t", f"{duration:.4f}",
        "-i", input_path,
        "-vf", f"fps={fps:.6f}",
        "-pix_fmt", "rgb24",
        os.path.join(out_dir, "frame-%06d.png"),
    ]
    subprocess.run(cmd, check=True, capture_output=True)

    frames = sorted(Path(out_dir).glob("frame-*.png"))
    # ffmpeg may produce ±1 frame; trim or warn
    if len(frames) > n_frames:
        for f in frames[n_frames:]:
            f.unlink()
        frames = frames[:n_frames]
    return [str(f) for f in frames]


def center_crop_square(img: Image.Image) -> Image.Image:
    """Crop the largest centered square from a rectangular image."""
    w, h = img.size
    side = min(w, h)
    left = (w - side) // 2
    top = (h - side) // 2
    return img.crop((left, top, left + side, top + side))


def luminance_alpha(img: Image.Image, gain: float = 2.5) -> Image.Image:
    """Create RGBA image where alpha = clamp(luminance * gain, 0, 255).

    For VFX on dark backgrounds this makes bright effects opaque and the
    black background transparent — perfect for additive or alpha blending.
    """
    rgb = img.convert("RGB")
    r, g, b = rgb.split()

    # ITU-R BT.709 luminance weights
    lum = Image.eval(r, lambda v: int(v * 0.2126))
    lum_g = Image.eval(g, lambda v: int(v * 0.7152))
    lum_b = Image.eval(b, lambda v: int(v * 0.0722))

    # Merge luminance channel by channel (Pillow doesn't have direct add)
    import numpy as np
    arr_r = np.array(r, dtype=np.float32)
    arr_g = np.array(g, dtype=np.float32)
    arr_b = np.array(b, dtype=np.float32)

    lum_arr = 0.2126 * arr_r + 0.7152 * arr_g + 0.0722 * arr_b
    alpha_arr = np.clip(lum_arr * gain, 0, 255).astype(np.uint8)

    alpha = Image.fromarray(alpha_arr, "L")
    return Image.merge("RGBA", (r, g, b, alpha))


def build_atlas(frames: list[Image.Image], grid: int, cell_size: int) -> Image.Image:
    """Pack frames into a grid×grid atlas image."""
    atlas_size = grid * cell_size
    atlas = Image.new("RGBA", (atlas_size, atlas_size), (0, 0, 0, 0))

    for idx, frame in enumerate(frames):
        if idx >= grid * grid:
            break
        row = idx // grid
        col = idx % grid
        x = col * cell_size
        y = row * cell_size
        atlas.paste(frame, (x, y))

    return atlas


def main():
    parser = argparse.ArgumentParser(
        description="Convert MP4 VFX video to sprite-sheet texture atlas"
    )
    parser.add_argument("--input", required=True, help="Input MP4 file")
    parser.add_argument("--output", required=True, help="Output atlas PNG path")
    parser.add_argument("--frames", type=int, default=48,
                        help="Number of frames to extract (default: 48)")
    parser.add_argument("--grid", type=int, default=8,
                        help="Grid dimension (default: 8 → 8×8 = 64 cells)")
    parser.add_argument("--cell-size", type=int, default=256,
                        help="Per-cell resolution in pixels (default: 256)")
    parser.add_argument("--start-time", type=float, default=0.0,
                        help="Start time in seconds (default: 0.0)")
    parser.add_argument("--end-time", type=float, default=None,
                        help="End time in seconds (default: video duration)")
    parser.add_argument("--alpha-mode", choices=["luminance", "keep"], default="luminance",
                        help="Alpha generation mode (default: luminance)")
    parser.add_argument("--alpha-gain", type=float, default=2.5,
                        help="Luminance-to-alpha gain multiplier (default: 2.5)")
    parser.add_argument("--save-frames", action="store_true",
                        help="Also save individual processed frames alongside atlas")
    args = parser.parse_args()

    if not os.path.isfile(args.input):
        print(f"Error: input file not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    if args.frames > args.grid * args.grid:
        print(f"Warning: {args.frames} frames > {args.grid}×{args.grid} grid capacity "
              f"({args.grid**2}). Clamping to {args.grid**2}.", file=sys.stderr)
        args.frames = args.grid * args.grid

    # Probe video duration if end-time not specified
    if args.end_time is None:
        result = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "csv=p=0", args.input],
            capture_output=True, text=True, check=True,
        )
        args.end_time = float(result.stdout.strip())

    print(f"Input:      {args.input}")
    print(f"Output:     {args.output}")
    print(f"Frames:     {args.frames}")
    print(f"Grid:       {args.grid}×{args.grid}")
    print(f"Cell size:  {args.cell_size}px")
    print(f"Time range: {args.start_time:.2f}s – {args.end_time:.2f}s")
    print(f"Alpha mode: {args.alpha_mode} (gain={args.alpha_gain})")
    print()

    with tempfile.TemporaryDirectory(prefix="mp4_to_atlas_") as tmpdir:
        # Step 1: Extract frames
        print("Extracting frames from video...")
        frame_paths = extract_frames(
            args.input, tmpdir, args.frames,
            args.start_time, args.end_time,
        )
        print(f"  Extracted {len(frame_paths)} frames")

        # Steps 2–4: Process each frame
        print("Processing frames (crop → alpha → scale)...")
        processed = []
        for i, fp in enumerate(frame_paths):
            img = Image.open(fp)

            # Center-crop to square
            img = center_crop_square(img)

            # Alpha generation
            if args.alpha_mode == "luminance":
                img = luminance_alpha(img, gain=args.alpha_gain)
            else:
                img = img.convert("RGBA")

            # Scale to cell size
            img = img.resize((args.cell_size, args.cell_size), Image.LANCZOS)
            processed.append(img)

        print(f"  Processed {len(processed)} frames → {args.cell_size}×{args.cell_size} RGBA")

        # Optional: save individual frames
        if args.save_frames:
            frames_dir = Path(args.output).parent / (Path(args.output).stem + "_frames")
            frames_dir.mkdir(parents=True, exist_ok=True)
            for i, img in enumerate(processed):
                img.save(frames_dir / f"frame-{i:06d}.png")
            print(f"  Saved individual frames to {frames_dir}/")

        # Step 5: Build atlas
        print("Building atlas...")
        atlas = build_atlas(processed, args.grid, args.cell_size)

        # Ensure output directory exists
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        atlas.save(args.output, "PNG")

        atlas_w, atlas_h = atlas.size
        file_size = os.path.getsize(args.output)
        print(f"  Atlas: {atlas_w}×{atlas_h} → {args.output} ({file_size / 1024:.0f} KB)")
        print()
        print("Done! Atlas metadata for SpriteEffectRenderer:")
        print(f"  gridCount  = {args.grid}")
        print(f"  frameCount = {len(processed)}")
        print(f"  duration   = {args.end_time - args.start_time:.2f}s")


if __name__ == "__main__":
    main()
