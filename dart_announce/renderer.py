"""E-Paper display image renderer for 800x480 monochrome panels.

Generates 1-bit monochrome images tailored for the Waveshare 7.5-inch E-Paper HAT
(800x480) driven by microcontrollers such as the Adafruit ESP32-S3 Feather.

Supports 3 distinct display styles:
    1. 'solari': Iconic Solari di Udine mechanical split-flap railway station cards.
    2. 'matrix': Authentic transit Dot Matrix Indicator (DMI) with LED matrix character cells.
    3. 'plain':  Minimalist, distraction-free high-contrast transit departure grid.

Produces:
    - 1-bit PNG: /screen.png
    - 1-bit Windows BMP: /screen.bmp
    - 48,000-byte raw binary buffer: /screen.bin (ready to stream directly over SPI)
"""

import ctypes
import io
import sys
from datetime import datetime
from pathlib import Path

# Preload any shared libraries in virtual environment PIL/.libs if present (e.g. libopenjp2 on Raspbian)
_pil_libs = (
    Path(__file__).resolve().parent.parent
    / "venv"
    / "lib"
    / f"python{sys.version_info.major}.{sys.version_info.minor}"
    / "site-packages"
    / "PIL"
    / ".libs"
)
if _pil_libs.is_dir():
    for _lib in sorted(_pil_libs.glob("*.so*")):
        try:
            ctypes.CDLL(str(_lib))
        except Exception:
            pass

from PIL import Image, ImageDraw, ImageFont

from .service import DeparturesResult

WIDTH = 800
HEIGHT = 480

_FONTS_DIR = Path(__file__).resolve().parent / "fonts"


def _load_font(filename: str, size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    """Load bundled TrueType font with graceful fallback to default PIL font."""
    font_path = _FONTS_DIR / filename
    if font_path.is_file():
        try:
            return ImageFont.truetype(str(font_path), size=size)
        except Exception:
            pass
    return ImageFont.load_default(size=size)


# =====================================================================
# STYLE 1: SOLARI DI UDINE (Mechanical Split-Flap Railway Display)
# =====================================================================
def _render_solari(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
) -> Image.Image:
    img = Image.new("1", (WIDTH, HEIGHT), color=1)
    draw = ImageDraw.Draw(img)

    font_station = _load_font("BebasNeue.ttf", 36)
    font_clock = _load_font("BebasNeue.ttf", 44)
    font_sub = _load_font("Oswald.ttf", 16)
    font_th = _load_font("BebasNeue.ttf", 20)
    font_dest = _load_font("BebasNeue.ttf", 32)
    font_num = _load_font("BebasNeue.ttf", 28)
    font_status = _load_font("BebasNeue.ttf", 22)
    font_meta = _load_font("Oswald.ttf", 13)

    # Outer enclosure bezel
    draw.rectangle([(6, 6), (WIDTH - 6, HEIGHT - 6)], outline=0, width=2)

    now_str = datetime.now().strftime("%H:%M")

    # Clock card (top-right)
    clock_x1 = WIDTH - 146
    clock_x2 = WIDTH - 14
    draw.rectangle([(clock_x1, 14), (clock_x2, 68)], fill=0)
    draw.line([(clock_x1, 41), (clock_x2, 41)], fill=1, width=1)
    draw.text((clock_x1 + 18, 16), now_str, fill=1, font=font_clock)

    # Battery module
    if battery is not None:
        bat_level = max(0, min(100, battery))
        bat_x1 = clock_x1 - 88
        bat_x2 = clock_x1 - 8
        draw.rectangle([(bat_x1, 14), (bat_x2, 68)], fill=0)
        draw.line([(bat_x1, 41), (bat_x2, 41)], fill=1, width=1)
        draw.text((bat_x1 + 10, 18), f"{bat_level}%", fill=1, font=font_status)
        ix, iy, iw, ih = bat_x1 + 14, 46, 32, 14
        draw.rectangle([(ix, iy), (ix + iw, iy + ih)], outline=1, width=1)
        draw.rectangle([(ix + iw, iy + 3), (ix + iw + 2, iy + ih - 3)], fill=1)
        fill_w = int((iw - 4) * (bat_level / 100.0))
        if fill_w > 0:
            draw.rectangle([(ix + 2, iy + 2), (ix + 2 + fill_w, iy + ih - 2)], fill=1)
        plaque_right = bat_x1 - 8
    else:
        plaque_right = clock_x1 - 8

    # Station Header Plaque
    plaque_left = 14
    draw.rectangle([(plaque_left, 14), (plaque_right, 68)], fill=0)
    draw.line([(plaque_left, 41), (plaque_right, 41)], fill=1, width=1)

    heading = f"DART  •  {result.station_name.upper()}"
    if direction:
        heading += f" ({direction.upper()})"
    draw.text((plaque_left + 16, 18), heading, fill=1, font=font_station)

    # Subtitle Info Strip
    draw.rectangle([(14, 74), (WIDTH - 14, 100)], fill=1, outline=0, width=1)
    sub_parts = [f"STATION: {result.station_code}"]
    if result.rain_chance is not None:
        sub_parts.append(f"RAIN CHANCE: {result.rain_chance}%")
    sub_parts.append(f"FEED: IRISH RAIL REAL-TIME")
    sub_parts.append(f"UPDATED: {datetime.now().strftime('%H:%M:%S')}")
    subtitle_text = "   |   ".join(sub_parts)
    draw.text((22, 77), subtitle_text, fill=0, font=font_sub)

    # Column Headers
    draw.rectangle([(14, 106), (WIDTH - 14, 132)], fill=0)
    draw.text((24, 108), "DESTINATION", fill=1, font=font_th)
    draw.text((432, 108), "DUE", fill=1, font=font_th)
    draw.text((544, 108), "EXPECTED", fill=1, font=font_th)
    draw.text((652, 108), "STATUS", fill=1, font=font_th)

    # Rows
    row_y = 138
    row_h = 58
    gap = 6

    if result.departures:
        for idx, d in enumerate(result.departures[:5]):
            y = row_y + idx * (row_h + gap)
            mid_y = y + row_h // 2

            # 1. Destination Flap Card
            dest_box = [(14, y), (420, y + row_h)]
            draw.rectangle(dest_box, fill=1, outline=0, width=2)
            draw.line([(14, mid_y), (420, mid_y)], fill=0, width=1)
            draw.text((24, y + 12), d.destination.upper(), fill=0, font=font_dest)

            # 2. Due Flap Card
            due_box = [(428, y), (532, y + row_h)]
            if d.due_in <= 0:
                draw.rectangle(due_box, fill=0)
                draw.line([(428, mid_y), (532, mid_y)], fill=1, width=1)
                draw.text((456, y + 14), "DUE", fill=1, font=font_num)
            else:
                draw.rectangle(due_box, fill=1, outline=0, width=2)
                draw.line([(428, mid_y), (532, mid_y)], fill=0, width=1)
                due_text = f"{d.due_in} MIN"
                draw.text((440, y + 14), due_text, fill=0, font=font_num)

            # 3. Expected Time Flap Card
            exp_box = [(540, y), (642, y + row_h)]
            draw.rectangle(exp_box, fill=1, outline=0, width=2)
            draw.line([(540, mid_y), (642, mid_y)], fill=0, width=1)
            draw.text((554, y + 14), d.expected, fill=0, font=font_num)

            # 4. Status Flap Card
            status_box = [(650, y), (WIDTH - 14, y + row_h)]
            draw.rectangle(status_box, fill=1, outline=0, width=2)
            draw.line([(650, mid_y), (WIDTH - 14, mid_y)], fill=0, width=1)
            status_raw = d.status if d.status != "No Information" else "On Time"
            draw.text((664, y + 17), status_raw.upper(), fill=0, font=font_status)
    else:
        draw.rectangle([(14, 180), (WIDTH - 14, 340)], fill=1, outline=0, width=2)
        draw.line([(14, 260), (WIDTH - 14, 260)], fill=0, width=1)
        draw.text((180, 240), "NO UPCOMING DEPARTURES SCHEDULED", fill=0, font=font_station)

    # Footer
    y_footer = HEIGHT - 22
    draw.line([(14, y_footer - 4), (WIDTH - 14, y_footer - 4)], fill=0, width=1)
    draw.text((16, y_footer), "LIVE IRISH RAIL REAL-TIME DATA  •  SOLARI SPLIT-FLAP TRANSIT DISPLAY", fill=0, font=font_meta)
    draw.text((WIDTH - 142, y_footer), "ADAFRUIT ESP32-S3", fill=0, font=font_meta)

    return img


# =====================================================================
# STYLE 2: DOT MATRIX INDICATOR (Transit LED / Flip-Dot Matrix Display)
# =====================================================================
def _render_matrix(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
) -> Image.Image:
    img = Image.new("1", (WIDTH, HEIGHT), color=0)
    draw = ImageDraw.Draw(img)

    f_title = _load_font("Doto.ttf", 30)
    f_head = _load_font("Doto.ttf", 16)
    f_row = _load_font("Doto.ttf", 26)
    f_ticker = _load_font("Doto.ttf", 15)

    # Double industrial chassis outline
    draw.rectangle([(6, 6), (WIDTH - 7, HEIGHT - 7)], outline=1, width=2)
    draw.rectangle([(10, 10), (WIDTH - 11, HEIGHT - 11)], outline=1, width=1)

    # Header: DART :: [STATION] ([DIRECTION])
    heading = f"DART :: {result.station_name.upper()}"
    if direction:
        heading += f" ({direction.upper()})"
    draw.text((22, 18), heading, fill=1, font=f_title)

    # Top-right clock
    now_str = datetime.now().strftime("%H:%M")
    clock_bbox = f_title.getbbox(now_str)
    clock_w = clock_bbox[2] - clock_bbox[0]
    draw.text((WIDTH - 24 - clock_w, 18), now_str, fill=1, font=f_title)

    # Header divider line
    draw.line([(12, 58), (WIDTH - 13, 58)], fill=1, width=2)

    # Column headers
    draw.text((22, 68), "DESTINATION", fill=1, font=f_head)

    due_lbl = "DUE"
    due_w = f_head.getbbox(due_lbl)[2] - f_head.getbbox(due_lbl)[0]
    draw.text((490 - due_w, 68), due_lbl, fill=1, font=f_head)

    exp_lbl = "EXPECTED"
    exp_w = f_head.getbbox(exp_lbl)[2] - f_head.getbbox(exp_lbl)[0]
    draw.text((615 - exp_w, 68), exp_lbl, fill=1, font=f_head)

    stat_lbl = "STATUS"
    stat_w = f_head.getbbox(stat_lbl)[2] - f_head.getbbox(stat_lbl)[0]
    draw.text((WIDTH - 26 - stat_w, 68), stat_lbl, fill=1, font=f_head)

    draw.line([(12, 94), (WIDTH - 13, 94)], fill=1, width=1)

    # Departures
    if result.departures:
        y = 106
        for i, d in enumerate(result.departures[:5]):
            dest_text = f"{i + 1}  {d.destination.upper()}"
            while len(dest_text) > 4 and f_row.getbbox(dest_text)[2] > 400:
                dest_text = dest_text[:-2] + ".."
            draw.text((22, y), dest_text, fill=1, font=f_row)

            # Due text (right-aligned at 490)
            due_text = d.to_dict()["due_in_text"].upper()
            w_due = f_row.getbbox(due_text)[2] - f_row.getbbox(due_text)[0]
            draw.text((490 - w_due, y), due_text, fill=1, font=f_row)

            # Expected time (right-aligned at 615)
            w_exp = f_row.getbbox(d.expected)[2] - f_row.getbbox(d.expected)[0]
            draw.text((615 - w_exp, y), d.expected, fill=1, font=f_row)

            # Status (right-aligned at WIDTH - 26)
            st = d.status if d.status != "No Information" else "On Time"
            st_upper = st.upper()
            w_st = f_row.getbbox(st_upper)[2] - f_row.getbbox(st_upper)[0]
            draw.text((WIDTH - 26 - w_st, y), st_upper, fill=1, font=f_row)

            y += 65
            if i < len(result.departures[:5]) - 1:
                for dot_x in range(16, WIDTH - 16, 8):
                    draw.point((dot_x, y - 8), fill=1)
    else:
        empty_msg = "NO UPCOMING DEPARTURES"
        w_empty = f_title.getbbox(empty_msg)[2] - f_title.getbbox(empty_msg)[0]
        draw.text(((WIDTH - w_empty) // 2, 230), empty_msg, fill=1, font=f_title)

    # Bottom ticker / telemetry
    draw.line([(12, 436), (WIDTH - 13, 436)], fill=1, width=2)
    rain_str = f"{result.rain_chance}%" if result.rain_chance is not None else "0%"
    left_ticker = f"● IRISH RAIL LIVE  |  STN: {result.station_code}  |  RAIN: {rain_str}"
    draw.text((22, 446), left_ticker, fill=1, font=f_ticker)

    bat_str = f"  |  BAT: {battery}%" if battery is not None else ""
    right_ticker = f"UPDATED {datetime.now().strftime('%H:%M:%S')}{bat_str}"
    rt_w = f_ticker.getbbox(right_ticker)[2] - f_ticker.getbbox(right_ticker)[0]
    draw.text((WIDTH - 26 - rt_w, 446), right_ticker, fill=1, font=f_ticker)

    return img


# =====================================================================
# STYLE 3: PLAIN (Minimalist High-Contrast Tabular Transit Board)
# =====================================================================
def _render_plain(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
) -> Image.Image:
    img = Image.new("1", (WIDTH, HEIGHT), color=1)
    draw = ImageDraw.Draw(img)

    f_title = _load_font("Oswald.ttf", 32)
    f_clock = _load_font("Oswald.ttf", 44)
    f_sub = _load_font("Oswald.ttf", 16)
    f_th = _load_font("Oswald.ttf", 16)
    f_dest = _load_font("Oswald.ttf", 26)
    f_time = _load_font("Oswald.ttf", 24)
    f_status = _load_font("Oswald.ttf", 18)
    f_meta = _load_font("Oswald.ttf", 13)

    # Header
    heading = f"DART — {result.station_name}"
    if direction:
        heading += f" ({direction})"
    draw.text((20, 16), heading, fill=0, font=f_title)

    now_str = datetime.now().strftime("%H:%M")
    draw.text((WIDTH - 120, 12), now_str, fill=0, font=f_clock)

    right_offset = WIDTH - 120
    if battery is not None:
        draw.text((WIDTH - 210, 24), f"BAT: {battery}%", fill=0, font=f_sub)
        right_offset = WIDTH - 210

    if result.rain_chance is not None:
        rain_str = f"Rain: {result.rain_chance}%"
        f_rain = _load_font("Oswald.ttf", 22)
        r_bbox = f_rain.getbbox(rain_str)
        r_w = r_bbox[2] - r_bbox[0]
        h_bbox = f_title.getbbox(heading)
        h_w = h_bbox[2] - h_bbox[0]
        left_bound = 20 + h_w + 16
        if right_offset - left_bound > r_w:
            rain_x = left_bound + (right_offset - left_bound - r_w) // 2
            draw.text((rain_x, 24), rain_str, fill=0, font=f_rain)
        else:
            draw.text((right_offset - r_w - 10, 24), rain_str, fill=0, font=f_rain)

    draw.line([(20, 68), (WIDTH - 20, 68)], fill=0, width=2)

    # Column headers
    y_th = 80
    draw.text((20, y_th), "DESTINATION", fill=0, font=f_th)
    draw.text((450, y_th), "DUE", fill=0, font=f_th)
    draw.text((580, y_th), "EXPECTED", fill=0, font=f_th)
    draw.text((700, y_th), "STATUS", fill=0, font=f_th)
    draw.line([(20, y_th + 24), (WIDTH - 20, y_th + 24)], fill=0, width=2)

    # Rows
    y_start = 114
    row_h = 60

    if result.departures:
        for i, d in enumerate(result.departures[:5]):
            y = y_start + i * row_h
            draw.text((20, y + 12), d.destination, fill=0, font=f_dest)
            due_text = "DUE" if d.due_in <= 0 else f"{d.due_in} min"
            draw.text((450, y + 12), due_text, fill=0, font=f_time)
            draw.text((580, y + 12), d.expected, fill=0, font=f_time)
            st = d.status if d.status != "No Information" else "On Time"
            draw.text((700, y + 14), st, fill=0, font=f_status)
            draw.line([(20, y + 54), (WIDTH - 20, y + 54)], fill=0, width=1)
    else:
        draw.text((260, 240), "No upcoming departures scheduled", fill=0, font=f_title)

    # Footer
    draw.line([(20, HEIGHT - 26), (WIDTH - 20, HEIGHT - 26)], fill=0, width=1)
    draw.text((20, HEIGHT - 20), "LIVE IRISH RAIL REAL-TIME DATA", fill=0, font=f_meta)
    updated_str = f"UPDATED: {datetime.now().strftime('%H:%M:%S')}"
    up_bbox = f_meta.getbbox(updated_str)
    up_w = up_bbox[2] - up_bbox[0]
    draw.text(((WIDTH - up_w) // 2, HEIGHT - 20), updated_str, fill=0, font=f_meta)
    draw.text((WIDTH - 140, HEIGHT - 20), "ADAFRUIT ESP32-S3", fill=0, font=f_meta)

    return img


# =====================================================================
# MAIN PUBLIC API
# =====================================================================
def render_screen_image(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
    style: str = "solari",
) -> Image.Image:
    """Render the departure board onto an 800x480 1-bit monochrome image (mode '1').

    Supports: 'solari' (split-flap cards), 'matrix' (Dot Matrix Indicator), 'plain' (minimal tabular).
    """
    style_key = (style or "solari").strip().lower()
    if style_key in ("matrix", "dotmatrix", "modern"):
        return _render_matrix(result, direction=direction, battery=battery)
    elif style_key == "plain":
        return _render_plain(result, direction=direction, battery=battery)
    return _render_solari(result, direction=direction, battery=battery)


def render_screen_png(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
    style: str = "solari",
) -> bytes:
    """Return PNG bytes of the 800x480 departure board in chosen style."""
    img = render_screen_image(result, direction=direction, battery=battery, style=style)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def render_screen_bmp(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
    style: str = "solari",
) -> bytes:
    """Return 1-bit Windows BMP bytes of the 800x480 departure board in chosen style."""
    img = render_screen_image(result, direction=direction, battery=battery, style=style)
    buf = io.BytesIO()
    img.save(buf, format="BMP")
    return buf.getvalue()


def render_screen_raw(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
    invert: bool = False,
    style: str = "solari",
) -> bytes:
    """Return exact 48,000 bytes (800x480 / 8) raw 1-bit buffer in chosen style."""
    img = render_screen_image(result, direction=direction, battery=battery, style=style)
    raw = img.tobytes()
    if invert:
        return bytes(~b & 0xFF for b in raw)
    return raw
