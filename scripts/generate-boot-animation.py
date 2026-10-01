#!/usr/bin/env python3
"""Generate the Distributed Matrix System startup animation.

Requires Pillow and either an ffmpeg executable on PATH or imageio-ffmpeg.
The generated MP4 is H.264/yuv420p without audio for RK3566 MPP playback.
"""

from __future__ import annotations

import argparse
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


def clamp(value: float, minimum: float = 0.0, maximum: float = 1.0) -> float:
    return max(minimum, min(maximum, value))


def ease_out_cubic(value: float) -> float:
    value = clamp(value)
    return 1.0 - (1.0 - value) ** 3


def ease_in_out_cubic(value: float) -> float:
    value = clamp(value)
    return 4.0 * value**3 if value < 0.5 else 1.0 - (-2.0 * value + 2.0) ** 3 / 2.0


def alpha_color(rgb: tuple[int, int, int], alpha: float) -> tuple[int, int, int, int]:
    return rgb + (round(255 * clamp(alpha)),)


def font_path(*candidates: str) -> str:
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    raise FileNotFoundError(f"No usable font found: {', '.join(candidates)}")


def find_ffmpeg() -> str:
    configured = os.environ.get("FFMPEG")
    if configured and os.path.isfile(configured):
        return configured
    executable = shutil.which("ffmpeg")
    if executable:
        return executable
    try:
        import imageio_ffmpeg

        return imageio_ffmpeg.get_ffmpeg_exe()
    except (ImportError, RuntimeError) as error:
        raise RuntimeError(
            "ffmpeg was not found. Install ffmpeg or run `python -m pip install imageio-ffmpeg`."
        ) from error


def draw_centered_text(
    draw: ImageDraw.ImageDraw,
    center_x: float,
    top: float,
    text: str,
    font: ImageFont.FreeTypeFont,
    fill: tuple[int, int, int, int],
    spacing: int = 0,
) -> None:
    if spacing <= 0:
        box = draw.textbbox((0, 0), text, font=font)
        draw.text((center_x - (box[2] - box[0]) / 2, top), text, font=font, fill=fill)
        return

    widths = [draw.textlength(character, font=font) for character in text]
    total_width = sum(widths) + spacing * max(0, len(text) - 1)
    cursor = center_x - total_width / 2
    for character, width in zip(text, widths):
        draw.text((cursor, top), character, font=font, fill=fill)
        cursor += width + spacing


def render_frame(
    time_seconds: float,
    width: int,
    height: int,
    title_font: ImageFont.FreeTypeFont,
    label_font: ImageFont.FreeTypeFont,
    small_font: ImageFont.FreeTypeFont,
) -> Image.Image:
    navy = (5, 17, 31)
    cyan = (39, 205, 224)
    blue = (53, 135, 226)
    white = (238, 248, 255)
    muted = (126, 157, 178)

    frame = Image.new("RGBA", (width, height), navy + (255,))
    draw = ImageDraw.Draw(frame, "RGBA")

    # Subtle right-side ambient light and center vignette.
    ambient = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    ambient_draw = ImageDraw.Draw(ambient, "RGBA")
    ambient_draw.ellipse(
        (width * 0.56, -height * 0.25, width * 1.18, height * 1.15),
        fill=(19, 123, 155, 54),
    )
    ambient_draw.ellipse(
        (width * 0.18, height * 0.06, width * 0.82, height * 0.96),
        fill=(5, 47, 79, 40),
    )
    ambient = ambient.filter(ImageFilter.GaussianBlur(radius=max(50, width // 24)))
    frame = Image.alpha_composite(frame, ambient)
    draw = ImageDraw.Draw(frame, "RGBA")

    # The engineering grid is deliberately static; only the center tiles and progress bar animate.
    grid_alpha = 0.09
    grid_step = max(80, width // 16)
    for x in range(0, width + grid_step, grid_step):
        draw.line((x, 0, x, height), fill=alpha_color((48, 151, 181), grid_alpha), width=2)
    for y in range(0, height + grid_step, grid_step):
        draw.line((0, y, width, y), fill=alpha_color((48, 151, 181), grid_alpha), width=2)

    # Logo: fifteen display tiles assemble from the center in a distributed wave.
    tile_size = round(width * 0.029)
    tile_gap = round(width * 0.0075)
    columns, rows = 5, 3
    logo_width = columns * tile_size + (columns - 1) * tile_gap
    logo_height = rows * tile_size + (rows - 1) * tile_gap
    logo_left = (width - logo_width) / 2
    logo_top = height * 0.245
    logo_center = (width / 2, logo_top + logo_height / 2)

    glow = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    glow_draw = ImageDraw.Draw(glow, "RGBA")
    tile_states: list[tuple[float, float, float, float, float]] = []
    for row in range(rows):
        for column in range(columns):
            index = row * columns + column
            delay = 0.28 + index * 0.045
            reveal = ease_out_cubic((time_seconds - delay) / 0.48)
            pulse = 0.5 + 0.5 * math.sin(time_seconds * 3.2 - index * 0.34)
            target_x = logo_left + column * (tile_size + tile_gap)
            target_y = logo_top + row * (tile_size + tile_gap)
            start_x = logo_center[0] + (column - 2) * tile_size * 0.18
            start_y = logo_center[1] + (row - 1) * tile_size * 0.18
            x = start_x + (target_x - start_x) * reveal
            y = start_y + (target_y - start_y) * reveal
            scale = 0.2 + 0.8 * reveal
            size = tile_size * scale
            tile_states.append((x + tile_size / 2, y + tile_size / 2, reveal, pulse, size))

            if reveal > 0:
                glow_alpha = (0.10 + pulse * 0.07) * reveal
                glow_draw.rounded_rectangle(
                    (x - 8, y - 8, x + size + 8, y + size + 8),
                    radius=8,
                    fill=alpha_color(cyan, glow_alpha),
                )
    glow = glow.filter(ImageFilter.GaussianBlur(radius=15))
    frame = Image.alpha_composite(frame, glow)
    draw = ImageDraw.Draw(frame, "RGBA")

    for index, (center_x, center_y, reveal, pulse, size) in enumerate(tile_states):
        if reveal <= 0:
            continue
        column = index % columns
        blend = column / max(1, columns - 1)
        color = (
            round(cyan[0] * (1 - blend) + blue[0] * blend),
            round(cyan[1] * (1 - blend) + blue[1] * blend),
            round(cyan[2] * (1 - blend) + blue[2] * blend),
        )
        half = size / 2
        radius = max(2, round(size * 0.08))
        draw.rounded_rectangle(
            (center_x - half, center_y - half, center_x + half, center_y + half),
            radius=radius,
            fill=alpha_color(color, reveal * (0.76 + pulse * 0.24)),
        )
        inset = min(4.0, max(1.0, size * 0.12))
        if size > inset * 2.0 + 2.0:
            highlight = (
                center_x - half + inset, center_y - half + inset,
                center_x + half - inset, center_y - half + inset + max(1.0, size * 0.06),
            )
            draw.rounded_rectangle(highlight, radius=2, fill=alpha_color(white, 0.26 * reveal))

    # All copy remains static so the only motion is the tile assembly and progress fill.
    draw_centered_text(draw, width / 2, height * 0.49, "DISTRIBUTED MATRIX", title_font, alpha_color(white, 1.0), 4)
    draw_centered_text(
        draw,
        width / 2,
        height * 0.575,
        "S Y S T E M   /   D I S P L A Y   F A B R I C",
        small_font,
        alpha_color(muted, 0.82),
    )
    draw_centered_text(
        draw, width / 2, height * 0.68, "SYSTEM STARTUP",
        label_font, alpha_color(cyan, 0.9), 3,
    )

    # The progress fill is the only moving element outside the center tile mark.
    rail_width = width * 0.36
    rail_height = max(4, round(height * 0.006))
    rail_left = (width - rail_width) / 2
    rail_top = height * 0.744
    progress = ease_in_out_cubic((time_seconds - 1.25) / 2.65)
    rail_alpha = 1.0
    draw.rounded_rectangle(
        (rail_left, rail_top, rail_left + rail_width, rail_top + rail_height),
        radius=rail_height / 2,
        fill=alpha_color((31, 70, 94), 0.8 * rail_alpha),
    )
    if progress > 0:
        fill_right = rail_left + rail_width * progress
        draw.rounded_rectangle(
            (rail_left, rail_top, fill_right, rail_top + rail_height),
            radius=rail_height / 2,
            fill=alpha_color(cyan, rail_alpha),
        )
        beacon_radius = rail_height * (1.2 + 0.45 * math.sin(time_seconds * 7.0) ** 2)
        draw.ellipse(
            (fill_right - beacon_radius, rail_top + rail_height / 2 - beacon_radius,
             fill_right + beacon_radius, rail_top + rail_height / 2 + beacon_radius),
            fill=alpha_color(white, rail_alpha * 0.86),
        )

    # Telemetry is fixed; changing percentages would introduce another animated element.
    left_label = "NODE 01  /  DRM 113  /  PLANE 78"
    right_label = "BOOT SEQUENCE"
    draw.text((width * 0.07, height * 0.91), left_label, font=small_font, fill=alpha_color(muted, 0.72))
    right_box = draw.textbbox((0, 0), right_label, font=small_font)
    draw.text((width * 0.93 - (right_box[2] - right_box[0]), height * 0.91), right_label,
              font=small_font, fill=alpha_color(muted, 0.72))

    return frame.convert("RGB")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("assets/boot-animation.mp4"))
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--duration", type=float, default=5.2)
    args = parser.parse_args()

    if args.width <= 0 or args.height <= 0 or args.fps <= 0 or args.duration <= 0:
        parser.error("width, height, fps and duration must be positive")
    if args.width % 2 or args.height % 2:
        parser.error("width and height must be even for yuv420p output")

    regular = font_path(
        r"C:\Windows\Fonts\segoeui.ttf",
        r"C:\Windows\Fonts\arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    )
    semibold = font_path(
        r"C:\Windows\Fonts\seguisb.ttf",
        r"C:\Windows\Fonts\arialbd.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    )
    monospace = font_path(
        r"C:\Windows\Fonts\consola.ttf",
        r"C:\Windows\Fonts\cour.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    )
    title_font = ImageFont.truetype(semibold, round(args.height * 0.050))
    label_font = ImageFont.truetype(regular, round(args.height * 0.022))
    small_font = ImageFont.truetype(monospace, round(args.height * 0.014))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    ffmpeg = find_ffmpeg()
    command = [
        ffmpeg,
        "-hide_banner",
        "-loglevel", "error",
        "-y",
        "-f", "rawvideo",
        "-pix_fmt", "rgb24",
        "-s", f"{args.width}x{args.height}",
        "-r", str(args.fps),
        "-i", "-",
        "-an",
        "-c:v", "libx264",
        "-preset", "slow",
        "-crf", "18",
        "-profile:v", "high",
        "-level:v", "4.1",
        "-pix_fmt", "yuv420p",
        "-movflags", "+faststart",
        str(args.output),
    ]

    total_frames = round(args.duration * args.fps)
    process = subprocess.Popen(command, stdin=subprocess.PIPE)
    assert process.stdin is not None
    try:
        for frame_index in range(total_frames):
            frame_time = frame_index / args.fps
            frame = render_frame(
                frame_time, args.width, args.height,
                title_font, label_font, small_font,
            )
            process.stdin.write(frame.tobytes())
            if frame_index % args.fps == 0:
                print(f"rendered {frame_index:03d}/{total_frames} frames", file=sys.stderr)
    except BrokenPipeError:
        pass
    finally:
        process.stdin.close()

    return_code = process.wait()
    if return_code != 0:
        raise RuntimeError(f"ffmpeg failed with exit code {return_code}")
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
