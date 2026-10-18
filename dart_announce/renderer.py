"""E-Paper display image renderer for 800x480 monochrome panels.

Generates 1-bit monochrome images tailored for the Waveshare 7.5-inch E-Paper HAT
(800x480) driven by microcontrollers such as the Adafruit ESP32-S3 Feather.

Supports 4 distinct display styles:
    1. 'solari':  Iconic Solari di Udine mechanical split-flap railway station cards.
    2. 'matrix':  Authentic transit Dot Matrix Indicator (DMI) with LED matrix character cells.
    3. 'plain':   Minimalist, distraction-free high-contrast transit departure grid.
    4. 'reverse': High-contrast inverted dark mode (white text on black canvas).

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
# STYLE 4: REVERSE CONTRAST (White on Black Inverted Dark Mode)
# =====================================================================
def _render_reverse(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
) -> Image.Image:
    img = Image.new("1", (WIDTH, HEIGHT), color=0)
    draw = ImageDraw.Draw(img)

    f_title = _load_font("Oswald.ttf", 32)
    f_clock = _load_font("Oswald.ttf", 44)
    f_sub = _load_font("Oswald.ttf", 16)
    f_th = _load_font("Oswald.ttf", 16)
    f_dest = _load_font("Oswald.ttf", 26)
    f_time = _load_font("Oswald.ttf", 24)
    f_status = _load_font("Oswald.ttf", 18)
    f_meta = _load_font("Oswald.ttf", 13)

    # Outer crisp bezel border
    draw.rectangle([(4, 4), (WIDTH - 5, HEIGHT - 5)], outline=1, width=2)

    # Header
    heading = f"DART — {result.station_name}"
    if direction:
        heading += f" ({direction})"
    draw.text((20, 16), heading, fill=1, font=f_title)

    now_str = datetime.now().strftime("%H:%M")
    draw.text((WIDTH - 120, 12), now_str, fill=1, font=f_clock)

    right_offset = WIDTH - 120
    if battery is not None:
        draw.text((WIDTH - 210, 24), f"BAT: {battery}%", fill=1, font=f_sub)
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
            draw.text((rain_x, 24), rain_str, fill=1, font=f_rain)
        else:
            draw.text((right_offset - r_w - 10, 24), rain_str, fill=1, font=f_rain)

    draw.line([(20, 68), (WIDTH - 20, 68)], fill=1, width=2)

    # Column headers
    y_th = 80
    draw.text((20, y_th), "DESTINATION", fill=1, font=f_th)
    draw.text((450, y_th), "DUE", fill=1, font=f_th)
    draw.text((580, y_th), "EXPECTED", fill=1, font=f_th)
    draw.text((700, y_th), "STATUS", fill=1, font=f_th)
    draw.line([(20, y_th + 24), (WIDTH - 20, y_th + 24)], fill=1, width=2)

    # Rows
    y_start = 114
    row_h = 60

    if result.departures:
        for i, d in enumerate(result.departures[:5]):
            y = y_start + i * row_h
            draw.text((20, y + 12), d.destination, fill=1, font=f_dest)
            due_text = "DUE" if d.due_in <= 0 else f"{d.due_in} min"
            draw.text((450, y + 12), due_text, fill=1, font=f_time)
            draw.text((580, y + 12), d.expected, fill=1, font=f_time)
            st = d.status if d.status != "No Information" else "On Time"
            draw.text((700, y + 14), st, fill=1, font=f_status)
            draw.line([(20, y + 54), (WIDTH - 20, y + 54)], fill=1, width=1)
    else:
        draw.text((260, 240), "No upcoming departures scheduled", fill=1, font=f_title)

    # Footer
    draw.line([(20, HEIGHT - 26), (WIDTH - 20, HEIGHT - 26)], fill=1, width=1)
    draw.text((20, HEIGHT - 20), "LIVE IRISH RAIL REAL-TIME DATA  •  REVERSE MONOCHROME", fill=1, font=f_meta)
    updated_str = f"UPDATED: {datetime.now().strftime('%H:%M:%S')}"
    up_bbox = f_meta.getbbox(updated_str)
    up_w = up_bbox[2] - up_bbox[0]
    draw.text(((WIDTH - up_w) // 2, HEIGHT - 20), updated_str, fill=1, font=f_meta)
    draw.text((WIDTH - 140, HEIGHT - 20), "ADAFRUIT ESP32-S3", fill=1, font=f_meta)

    return img


# =====================================================================
# STYLE 5: COMMUTE (3-Section Multimodal Board: DART + Bus + Dashboard)
# =====================================================================
def _render_commute(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
) -> Image.Image:
    img = Image.new("1", (WIDTH, HEIGHT), color=1)
    draw = ImageDraw.Draw(img)

    f_title = _load_font("BebasNeue.ttf", 26)
    f_badge = _load_font("BebasNeue.ttf", 24)
    f_dest = _load_font("BebasNeue.ttf", 32)
    f_sub = _load_font("Oswald.ttf", 20)
    f_status = _load_font("Oswald.ttf", 18)
    f_meta = _load_font("Oswald.ttf", 14)
    f_large_clock = _load_font("BebasNeue.ttf", 52)
    f_large_temp = _load_font("BebasNeue.ttf", 46)

    # ================= SECTION 1: DART =================
    # Title Bar (Y: 0 to 36)
    draw.rectangle([(0, 0), (WIDTH, 36)], fill=0)
    st_name = result.station_name.upper() if result.station_name else "SUTTON"
    dir_str = direction.upper() if direction else "SOUTHBOUND"
    dart_title = f"DART  •  {st_name}  •  {dir_str}"
    draw.text((24, 6), dart_title, fill=1, font=f_title)

    # Next 2 Trains (Row 1 at Y: 42, Row 2 at Y: 96)
    y_trains = [42, 96]
    if not result.departures:
        draw.text((180, 80), "CONNECTING TO IRISH RAIL...", fill=0, font=f_dest)
    else:
        for i, d in enumerate(result.departures[:2]):
            ry = y_trains[i]
            if i == 0:
                draw.line([(24, 94), (WIDTH - 24, 94)], fill=0, width=1)

            # Due in badge
            draw.rectangle([(24, ry + 2), (120, ry + 46)], fill=0)
            due_txt = "DUE" if d.due_in <= 0 else f"{d.due_in} MIN"
            due_bbox = f_badge.getbbox(due_txt)
            due_w = due_bbox[2] - due_bbox[0]
            draw.text((24 + (96 - due_w) // 2, ry + 12), due_txt, fill=1, font=f_badge)

            # Destination
            dest = d.destination.upper()
            if len(dest) > 16:
                dest = dest[:15] + "."
            draw.text((140, ry + 8), dest, fill=0, font=f_dest)

            # Expected
            exp_txt = f"EXP: {d.expected if d.expected else '--:--'}"
            draw.text((520, ry + 14), exp_txt, fill=0, font=f_sub)

            # Status
            st = d.status.upper() if d.status and d.status != "No Information" else "ON TIME"
            draw.text((675, ry + 14), st, fill=0, font=f_status)

    # ================= SECTION 2: DUBLIN BUS =================
    # Title Bar (Y: 154 to 190)
    draw.rectangle([(0, 154), (WIDTH, 190)], fill=0)
    bus_rt = (result.bus_route or "H1").upper()
    bus_st = (result.bus_stop_name or "STOP 917 (ADMIRAL PARK)").upper()
    bus_title = f"DUBLIN BUS  •  ROUTE {bus_rt}  •  {bus_st}"
    draw.text((24, 160), bus_title, fill=1, font=f_title)

    # Next 2 Buses (Row 1 at Y: 196, Row 2 at Y: 250)
    y_buses = [196, 250]
    if not result.bus_departures:
        draw.text((180, 234), "CONNECTING TO DUBLIN BUS...", fill=0, font=f_dest)
    else:
        for i, b in enumerate(result.bus_departures[:2]):
            ry = y_buses[i]
            if i == 0:
                draw.line([(24, 248), (WIDTH - 24, 248)], fill=0, width=1)

            # Due in badge
            draw.rectangle([(24, ry + 2), (120, ry + 46)], fill=0)
            due_txt = "DUE" if b.due_in <= 0 else f"{b.due_in} MIN"
            due_bbox = f_badge.getbbox(due_txt)
            due_w = due_bbox[2] - due_bbox[0]
            draw.text((24 + (96 - due_w) // 2, ry + 12), due_txt, fill=1, font=f_badge)

            # Destination
            b_dest = b.destination.upper()
            if len(b_dest) > 16:
                b_dest = b_dest[:15] + "."
            draw.text((140, ry + 8), b_dest, fill=0, font=f_dest)

            # Expected
            exp_txt = f"EXP: {b.expected if b.expected else '--:--'}"
            draw.text((520, ry + 14), exp_txt, fill=0, font=f_sub)

            # Status
            st = b.status.upper()
            draw.text((675, ry + 14), st, fill=0, font=f_status)

    # ================= SECTION 3: TIME, WEATHER & BATTERY =================
    # Thick Divider (Y: 308 to 312)
    draw.rectangle([(0, 308), (WIDTH, 312)], fill=0)

    # Vertical separators at X: 275 and X: 545
    draw.line([(275, 318), (275, 472)], fill=0, width=1)
    draw.line([(545, 318), (545, 472)], fill=0, width=1)

    # Column 1: Current Time & Date (X: 24 to 260)
    draw.text((24, 322), "CURRENT TIME", fill=0, font=f_meta)
    clock_str = datetime.now().strftime("%H:%M")
    draw.text((24, 344), clock_str, fill=0, font=f_large_clock)
    date_str = datetime.now().strftime("%a %d %b %Y").upper()
    draw.text((24, 414), date_str, fill=0, font=f_meta)
    draw.text((24, 442), "HTTP://DART-DISPLAY.LOCAL/", fill=0, font=f_meta)

    # Column 2: Expected Weather (X: 295 to 530)
    draw.text((295, 322), "EXPECTED WEATHER", fill=0, font=f_meta)
    temp_str = f"{result.temperature:.1f}°C" if result.temperature is not None else "--°C"
    draw.text((295, 344), temp_str, fill=0, font=f_large_temp)

    rain_val = result.rain_chance if result.rain_chance is not None else -1
    rain_str = f"RAIN CHANCE: {rain_val}%" if rain_val >= 0 else "RAIN CHANCE: --%"
    if rain_val >= 40:
        r_bbox = f_meta.getbbox(rain_str)
        rw = r_bbox[2] - r_bbox[0]
        draw.rectangle([(295, 410), (295 + rw + 14, 434)], fill=0)
        draw.text((302, 414), rain_str, fill=1, font=f_meta)
        draw.text((295, 442), "UMBRELLA ADVISED", fill=0, font=f_meta)
    else:
        draw.text((295, 414), rain_str, fill=0, font=f_meta)
        draw.text((295, 442), "BALDOYLE & SUTTON", fill=0, font=f_meta)

    # Column 3: Battery & System (X: 565 to 780)
    draw.text((565, 322), "BATTERY & SYSTEM", fill=0, font=f_meta)
    bat_pct = battery if battery is not None else 100
    draw.text((565, 344), f"{bat_pct}%", fill=0, font=f_large_temp)

    # Visual battery gauge
    bx, by, bw, bh = 680, 352, 56, 26
    draw.rectangle([(bx, by), (bx + bw, by + bh)], outline=0, width=1)
    draw.rectangle([(bx + bw, by + 7), (bx + bw + 4, by + 19)], fill=0)
    fill_w = max(2, min(48, int(48 * (bat_pct / 100.0))))
    draw.rectangle([(bx + 4, by + 4), (bx + 4 + fill_w, by + bh - 4)], fill=0)

    pwr_str = "POWER: USB (DESK)" if bat_pct == 100 else "POWER: LIPO BATTERY"
    draw.text((565, 414), pwr_str, fill=0, font=f_meta)
    draw.text((565, 442), "AUTO-REFRESH: 60 SEC", fill=0, font=f_meta)

    return img


# =====================================================================
# SCHEDULED DOWN PERIOD: CLOCK & TOMORROW'S WEATHER
# =====================================================================
def _render_clock_weather(
    result: DeparturesResult,
    battery: int | None = None,
    invert: bool = False,
    resume_hour: int = 6,
) -> Image.Image:
    bg_color = 0 if invert else 1
    fg_color = 1 if invert else 0
    img = Image.new("1", (WIDTH, HEIGHT), color=bg_color)
    draw = ImageDraw.Draw(img)

    f_title = _load_font("Oswald.ttf", 26)
    f_sub = _load_font("Oswald.ttf", 15)
    f_clock_huge = _load_font("BebasNeue.ttf", 116)
    f_date = _load_font("Oswald.ttf", 22)
    f_card_head = _load_font("BebasNeue.ttf", 24)
    f_cond = _load_font("Oswald.ttf", 26)
    f_temp = _load_font("Oswald.ttf", 22)
    f_badge = _load_font("BebasNeue.ttf", 24)
    f_body = _load_font("Oswald.ttf", 15)
    f_meta = _load_font("Oswald.ttf", 13)

    # Double chassis outline
    draw.rectangle([(6, 6), (WIDTH - 7, HEIGHT - 7)], outline=fg_color, width=2)
    draw.rectangle([(10, 10), (WIDTH - 11, HEIGHT - 11)], outline=fg_color, width=1)

    now = datetime.now()
    now_str = now.strftime("%H:%M")
    date_str = now.strftime("%A, %d %B %Y").upper()

    # Header Bar
    heading = f"DART  •  {result.station_name.upper()}  •  NIGHT STANDBY"
    draw.text((22, 18), heading, fill=fg_color, font=f_title)

    right_label = f"STANDBY  •  RESUMES {resume_hour:02d}:00"
    if battery is not None:
        right_label = f"BAT: {battery}%  |  " + right_label
    rl_bbox = f_sub.getbbox(right_label)
    rl_w = rl_bbox[2] - rl_bbox[0]
    draw.text((WIDTH - 24 - rl_w, 24), right_label, fill=fg_color, font=f_sub)

    draw.line([(12, 56), (WIDTH - 13, 56)], fill=fg_color, width=2)

    # Left Column: Huge Clock & Schedule Card (X: 16 to 376)
    draw.text((28, 70), now_str, fill=fg_color, font=f_clock_huge)
    draw.text((32, 195), date_str, fill=fg_color, font=f_date)

    # Schedule status card
    box_sched = [(28, 245), (375, 420)]
    draw.rectangle(box_sched, outline=fg_color, width=1)
    # Mini header tab
    draw.rectangle([(28, 245), (375, 275)], fill=fg_color)
    draw.text((38, 252), "ACTIVE SCHEDULE STATUS", fill=bg_color, font=f_sub)
    draw.text((38, 288), f"• Commute window: {resume_hour:02d}:00 – 19:00", fill=fg_color, font=f_body)
    draw.text((38, 318), "• Status: Down Period (Night Mode)", fill=fg_color, font=f_body)
    draw.text((38, 348), "• Refresh: Low-power battery conservation", fill=fg_color, font=f_body)
    draw.text((38, 378), f"• Monitored Station: {result.station_name} ({result.station_code})", fill=fg_color, font=f_body)

    # Center vertical divider
    draw.line([(390, 68), (390, 425)], fill=fg_color, width=1)

    # Right Column: Tomorrow's Forecast (X: 405 to WIDTH - 20)
    card_w_right = WIDTH - 20
    draw.rectangle([(405, 75), (card_w_right, 420)], outline=fg_color, width=2)
    # Header bar
    draw.rectangle([(405, 75), (card_w_right, 115)], fill=fg_color)
    draw.text((420, 84), "TOMORROW'S COMMUTE FORECAST", fill=bg_color, font=f_card_head)

    tw = result.tomorrow_weather
    if tw is not None:
        cond_text = f"{tw.symbol} {tw.condition.upper()}"
        draw.text((420, 130), cond_text, fill=fg_color, font=f_cond)

        temp_text = f"HIGH: {tw.temp_max:.1f}°C    |    LOW: {tw.temp_min:.1f}°C"
        draw.text((420, 178), temp_text, fill=fg_color, font=f_temp)

        # Precipitation gauge badge
        badge_box = [(420, 225), (card_w_right - 15, 285)]
        draw.rectangle(badge_box, fill=fg_color)
        rain_label = f"PRECIPITATION PROBABILITY: {tw.rain_chance}%"
        draw.text((435, 242), rain_label, fill=bg_color, font=f_badge)

        # Commuter Advice tip
        draw.text((420, 310), "COMMUTE PREPARATION:", fill=fg_color, font=f_sub)
        if tw.rain_chance >= 40:
            tip = "Heavy rain expected — pack an umbrella"
        elif tw.rain_chance >= 20:
            tip = "Light rain showers possible during commute"
        else:
            tip = "Dry & clear conditions expected for morning commute"
        draw.text((420, 338), tip, fill=fg_color, font=f_body)

        date_formatted = f"Forecast Date: {tw.date_str}"
        draw.text((420, 385), date_formatted, fill=fg_color, font=f_meta)
    else:
        draw.text((425, 140), "Tomorrow's weather forecast unavailable", fill=fg_color, font=f_body)
        draw.text((425, 175), "Open-Meteo offline or station coordinates missing", fill=fg_color, font=f_meta)

    # Footer
    draw.line([(12, 440), (WIDTH - 13, 440)], fill=fg_color, width=1)
    draw.text((22, 448), "LIVE IRISH RAIL REAL-TIME DATA  •  OPEN-METEO WEATHER ENGINE", fill=fg_color, font=f_meta)
    draw.text((WIDTH - 210, 448), f"UPDATED: {now.strftime('%H:%M:%S')}", fill=fg_color, font=f_meta)

    return img


# =====================================================================
# RGB COLOR RENDERING FOR COLOR DISPLAYS (WAVESHARE 10-INCH COLOR PANELS)
# =====================================================================
def _render_solari_rgb(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
) -> Image.Image:
    """Render authentic 24-bit RGB Irish Rail Solari platform board for color displays."""
    img = Image.new("RGB", (WIDTH, HEIGHT), color=(255, 255, 255))
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
    draw.rectangle([(6, 6), (WIDTH - 6, HEIGHT - 6)], outline=(30, 41, 59), width=2)

    now_str = datetime.now().strftime("%H:%M")

    # Clock card (top-right)
    clock_x1 = WIDTH - 146
    clock_x2 = WIDTH - 14
    draw.rectangle([(clock_x1, 14), (clock_x2, 68)], fill=(15, 23, 42))
    draw.line([(clock_x1, 41), (clock_x2, 41)], fill=(51, 65, 85), width=1)
    draw.text((clock_x1 + 18, 16), now_str, fill=(248, 250, 252), font=font_clock)

    right_cursor = clock_x1

    # Battery module
    if battery is not None:
        bat_level = max(0, min(100, battery))
        bat_x1 = clock_x1 - 88
        bat_x2 = clock_x1 - 8
        draw.rectangle([(bat_x1, 14), (bat_x2, 68)], fill=(15, 23, 42))
        draw.line([(bat_x1, 41), (bat_x2, 41)], fill=(51, 65, 85), width=1)
        draw.text((bat_x1 + 10, 18), f"{bat_level}%", fill=(248, 250, 252), font=font_status)
        ix, iy, iw, ih = bat_x1 + 14, 46, 32, 14
        draw.rectangle([(ix, iy), (ix + iw, iy + ih)], outline=(248, 250, 252), width=1)
        draw.rectangle([(ix + iw, iy + 3), (ix + iw + 2, iy + ih - 3)], fill=(248, 250, 252))
        fill_w = int((iw - 4) * (bat_level / 100.0))
        bat_col = (34, 197, 94) if bat_level > 20 else (239, 68, 68)
        if fill_w > 0:
            draw.rectangle([(ix + 2, iy + 2), (ix + 2 + fill_w, iy + ih - 2)], fill=bat_col)
        right_cursor = bat_x1

    # Weather card on top line (forecasted temperature & rain probability)
    has_temp = result.temperature is not None
    has_rain = result.rain_chance is not None
    if has_temp or has_rain:
        weather_w = 190 if (has_temp and has_rain) else 100
        weather_x1 = right_cursor - weather_w - 8
        weather_x2 = right_cursor - 8
        draw.rectangle([(weather_x1, 14), (weather_x2, 68)], fill=(15, 23, 42))
        draw.line([(weather_x1, 41), (weather_x2, 41)], fill=(51, 65, 85), width=1)
        cur_x = weather_x1 + 10
        if has_temp:
            temp_str = f"{result.temperature:.1f}°C"
            draw.text((cur_x, 16), temp_str, fill=(248, 250, 252), font=font_station)
            cur_x += 80
        if has_rain:
            rain_bg = (2, 132, 199) if result.rain_chance >= 40 else (245, 158, 11)
            rain_fg = (255, 255, 255) if result.rain_chance >= 40 else (28, 25, 23)
            draw.rectangle([(cur_x, 24), (weather_x2 - 8, 58)], fill=rain_bg)
            rain_str = f"{result.rain_chance}% RAIN"
            draw.text((cur_x + 6, 28), rain_str, fill=rain_fg, font=font_sub)
        plaque_right = weather_x1 - 8
    else:
        plaque_right = right_cursor - 8

    # Station Header Plaque (DART Irish Rail Green #00843D)
    plaque_left = 14
    draw.rectangle([(plaque_left, 14), (plaque_right, 68)], fill=(0, 132, 61))
    draw.line([(plaque_left, 41), (plaque_right, 41)], fill=(0, 100, 46), width=1)

    heading = f"DART  •  {result.station_name.upper()}"
    if direction:
        heading += f" ({direction.upper()})"
    draw.text((plaque_left + 16, 18), heading, fill=(255, 255, 255), font=font_station)

    # Column Headers (Y: 76 to 104)
    draw.rectangle([(14, 76), (WIDTH - 14, 104)], fill=(30, 41, 59))
    draw.text((24, 78), "DESTINATION", fill=(248, 250, 252), font=font_th)
    draw.text((432, 78), "DUE", fill=(248, 250, 252), font=font_th)
    draw.text((544, 78), "EXPECTED", fill=(248, 250, 252), font=font_th)
    draw.text((652, 78), "STATUS", fill=(248, 250, 252), font=font_th)

    # Rows (expanded card height row_h=62, gap=6)
    row_y = 110
    row_h = 62
    gap = 6

    if result.departures:
        for idx, d in enumerate(result.departures[:5]):
            y = row_y + idx * (row_h + gap)
            mid_y = y + row_h // 2

            # 1. Destination Flap Card
            dest_box = [(14, y), (420, y + row_h)]
            draw.rectangle(dest_box, fill=(255, 255, 255), outline=(203, 213, 225), width=2)
            draw.line([(14, mid_y), (420, mid_y)], fill=(226, 232, 240), width=1)
            draw.text((24, y + 14), d.destination.upper(), fill=(15, 23, 42), font=font_dest)

            # 2. Due Flap Card (Transit Amber highlight when DUE)
            due_box = [(428, y), (532, y + row_h)]
            if d.due_in <= 0:
                draw.rectangle(due_box, fill=(245, 158, 11))
                draw.line([(428, mid_y), (532, mid_y)], fill=(217, 119, 6), width=1)
                draw.text((456, y + 16), "DUE", fill=(28, 25, 23), font=font_num)
            elif d.due_in <= 3:
                draw.rectangle(due_box, fill=(254, 243, 199), outline=(253, 230, 138), width=2)
                draw.line([(428, mid_y), (532, mid_y)], fill=(253, 230, 138), width=1)
                due_text = f"{d.due_in} MIN"
                draw.text((440, y + 16), due_text, fill=(146, 64, 14), font=font_num)
            else:
                draw.rectangle(due_box, fill=(255, 255, 255), outline=(203, 213, 225), width=2)
                draw.line([(428, mid_y), (532, mid_y)], fill=(226, 232, 240), width=1)
                due_text = f"{d.due_in} MIN"
                draw.text((440, y + 16), due_text, fill=(15, 23, 42), font=font_num)

            # 3. Expected Time Flap Card
            exp_box = [(540, y), (642, y + row_h)]
            draw.rectangle(exp_box, fill=(255, 255, 255), outline=(203, 213, 225), width=2)
            draw.line([(540, mid_y), (642, mid_y)], fill=(226, 232, 240), width=1)
            draw.text((554, y + 16), d.expected, fill=(15, 23, 42), font=font_num)

            # 4. Status Flap Card (Emerald green for On Time, Crimson for Delayed)
            status_box = [(650, y), (WIDTH - 14, y + row_h)]
            status_raw = d.status if d.status != "No Information" else "On Time"
            st_lower = status_raw.lower()
            if "late" in st_lower or "delay" in st_lower or "cancel" in st_lower:
                draw.rectangle(status_box, fill=(254, 226, 226), outline=(252, 165, 165), width=2)
                draw.line([(650, mid_y), (WIDTH - 14, mid_y)], fill=(254, 202, 202), width=1)
                draw.text((664, y + 19), status_raw.upper(), fill=(220, 38, 38), font=font_status)
            elif "on time" in st_lower or "early" in st_lower:
                draw.rectangle(status_box, fill=(236, 253, 245), outline=(167, 243, 208), width=2)
                draw.line([(650, mid_y), (WIDTH - 14, mid_y)], fill=(209, 250, 229), width=1)
                draw.text((664, y + 19), status_raw.upper(), fill=(21, 128, 61), font=font_status)
            else:
                draw.rectangle(status_box, fill=(255, 255, 255), outline=(203, 213, 225), width=2)
                draw.line([(650, mid_y), (WIDTH - 14, mid_y)], fill=(226, 232, 240), width=1)
                draw.text((664, y + 19), status_raw.upper(), fill=(15, 23, 42), font=font_status)
    else:
        draw.rectangle([(14, 160), (WIDTH - 14, 320)], fill=(248, 250, 252), outline=(203, 213, 225), width=2)
        draw.line([(14, 240), (WIDTH - 14, 240)], fill=(226, 232, 240), width=1)
        draw.text((180, 220), "NO UPCOMING DEPARTURES SCHEDULED", fill=(100, 116, 139), font=font_station)

    # Footer
    y_footer = HEIGHT - 22
    draw.line([(14, y_footer - 4), (WIDTH - 14, y_footer - 4)], fill=(203, 213, 225), width=1)
    draw.text((16, y_footer), "LIVE IRISH RAIL REAL-TIME DATA  •  SOLARI SPLIT-FLAP TRANSIT DISPLAY", fill=(100, 116, 139), font=font_meta)
    draw.text((WIDTH - 142, y_footer), "ADAFRUIT ESP32-S3", fill=(100, 116, 139), font=font_meta)

    return img


# =====================================================================
# MAIN PUBLIC API
# =====================================================================
def render_screen_image(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
    style: str = "solari",
    mode: str = "auto",
    is_active: bool = True,
    resume_hour: int = 6,
    color: bool = False,
) -> Image.Image:
    """Render the departure board or down-period clock with tomorrow's weather onto 800x480 image (monochrome or RGB if color=True)."""
    if color:
        return _render_solari_rgb(result, direction=direction, battery=battery)

    style_key = (style or "solari").strip().lower()
    is_reverse = style_key in ("reverse", "inverted", "dark")

    # If clock mode forced or in down period under auto mode
    if mode == "clock" or (mode == "auto" and not is_active):
        return _render_clock_weather(result, battery=battery, invert=is_reverse, resume_hour=resume_hour)

    if is_reverse:
        return _render_reverse(result, direction=direction, battery=battery)
    elif style_key in ("commute", "three_section", "3section"):
        return _render_commute(result, direction=direction, battery=battery)
    elif style_key in ("matrix", "dotmatrix", "modern"):
        return _render_matrix(result, direction=direction, battery=battery)
    elif style_key == "plain":
        return _render_plain(result, direction=direction, battery=battery)
    return _render_solari(result, direction=direction, battery=battery)


def render_screen_png(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
    style: str = "solari",
    mode: str = "auto",
    is_active: bool = True,
    resume_hour: int = 6,
    color: bool = False,
) -> bytes:
    """Return PNG bytes of the 800x480 departure board or clock in chosen style."""
    img = render_screen_image(
        result,
        direction=direction,
        battery=battery,
        style=style,
        mode=mode,
        is_active=is_active,
        resume_hour=resume_hour,
        color=color,
    )
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def render_screen_bmp(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
    style: str = "solari",
    mode: str = "auto",
    is_active: bool = True,
    resume_hour: int = 6,
) -> bytes:
    """Return 1-bit Windows BMP bytes of the 800x480 departure board or clock in chosen style."""
    img = render_screen_image(
        result,
        direction=direction,
        battery=battery,
        style=style,
        mode=mode,
        is_active=is_active,
        resume_hour=resume_hour,
    )
    buf = io.BytesIO()
    img.save(buf, format="BMP")
    return buf.getvalue()


def render_screen_raw(
    result: DeparturesResult,
    direction: str | None = None,
    battery: int | None = None,
    invert: bool = False,
    style: str = "solari",
    mode: str = "auto",
    is_active: bool = True,
    resume_hour: int = 6,
) -> bytes:
    """Return exact 48,000 bytes (800x480 / 8) raw 1-bit buffer in chosen style."""
    img = render_screen_image(
        result,
        direction=direction,
        battery=battery,
        style=style,
        mode=mode,
        is_active=is_active,
        resume_hour=resume_hour,
    )
    raw = img.tobytes()
    if invert:
        return bytes(~b & 0xFF for b in raw)
    return raw
