"""FastAPI app exposing DART departure data.

`/departures` is the JSON endpoint a TRMNL private plugin (polling strategy)
would eventually fetch. `/preview` renders an 800x480 monochrome mock of the
e-ink layout so it can be sanity-checked in a browser before real hardware
or a TRMNL Liquid template exist.
"""

from datetime import datetime, timezone
from html import escape
from pathlib import Path

from fastapi import FastAPI, HTTPException, Query, Request, Response
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, PlainTextResponse

from .cli import render_terminal_board
from .config import InvalidSettingsError, load_settings
from .irish_rail_client import IrishRailError
from .renderer import render_screen_bmp, render_screen_png, render_screen_raw
from .service import TRMNLPostError, build_trmnl_payload, get_departures, post_to_trmnl
from .stations import UnknownStationError

app = FastAPI(title="DART Announce")


@app.exception_handler(IrishRailError)
async def irish_rail_error_handler(request: Request, exc: IrishRailError) -> JSONResponse:
    return JSONResponse(status_code=502, content={"error": str(exc)})


@app.exception_handler(UnknownStationError)
async def unknown_station_error_handler(request: Request, exc: UnknownStationError) -> JSONResponse:
    return JSONResponse(status_code=400, content={"error": str(exc)})


@app.exception_handler(InvalidSettingsError)
async def invalid_settings_error_handler(request: Request, exc: InvalidSettingsError) -> JSONResponse:
    return JSONResponse(status_code=400, content={"error": str(exc)})


@app.exception_handler(TRMNLPostError)
async def trmnl_post_error_handler(request: Request, exc: TRMNLPostError) -> JSONResponse:
    return JSONResponse(status_code=502, content={"error": str(exc)})


@app.get("/health")
def health() -> dict:
    return {"status": "ok"}


@app.get("/departures")
def departures() -> dict:
    settings = load_settings()
    result = get_departures(settings)
    trmnl_data = build_trmnl_payload(result, settings.direction)
    return {
        "station": result.station_name,
        "station_code": result.station_code,
        "direction": settings.direction,
        "updated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "departures": [d.to_dict() for d in result.departures],
        "rain_chance_percent": result.rain_chance,
        "temperature_celsius": result.temperature,
        "merge_variables": trmnl_data["merge_variables"],
    }


@app.get("/trmnl")
def trmnl() -> dict:
    """Endpoint specifically formatted for TRMNL polling plugins."""
    settings = load_settings()
    result = get_departures(settings)
    return build_trmnl_payload(result, settings.direction)


@app.post("/trmnl/post")
def trmnl_post(webhook_url: str | None = Query(default=None)) -> dict:
    """Post current departures directly to a TRMNL custom plugin webhook."""
    settings = load_settings()
    target_url = webhook_url or settings.trmnl_webhook_url
    if not target_url:
        raise HTTPException(status_code=400, detail="No TRMNL webhook URL configured or provided")
    result = get_departures(settings)
    payload = build_trmnl_payload(result, settings.direction)
    response = post_to_trmnl(target_url, payload)
    return {"status": "success", "response": response, "departures_count": len(result.departures)}


@app.get("/terminal", response_class=PlainTextResponse)
def terminal(
    width: int = Query(default=68, ge=40, le=120, description="Width in characters"),
    plain: bool = Query(default=False, description="Disable ANSI colors"),
    ascii: bool = Query(default=False, description="Use plain ASCII box borders instead of Unicode"),
) -> PlainTextResponse:
    """Return an ASCII/Unicode departure board formatted for command-line display."""
    settings = load_settings()
    result = get_departures(settings)
    board = render_terminal_board(
        result,
        direction=settings.direction,
        width=width,
        use_ansi=not plain,
        ascii_only=ascii,
    )
    return PlainTextResponse(content=board + "\n")


@app.get("/screen.png")
def screen_png(
    battery: int | None = Query(default=None, ge=0, le=100, description="Battery percentage (0-100)"),
    style: str | None = Query(default=None, description="Display style: solari, matrix, plain, reverse"),
    mode: str = Query(default="auto", description="Display mode: auto, trains, clock"),
    color: bool = Query(default=False, description="Render full 24-bit RGB color for color displays"),
) -> Response:
    """Return an 800x480 PNG image for displays (1-bit monochrome by default, 24-bit RGB if color=True)."""
    settings = load_settings()
    result = get_departures(settings)
    active_style = style or settings.display_style
    png_bytes = render_screen_png(
        result,
        settings.direction,
        battery=battery,
        style=active_style,
        mode=mode,
        is_active=settings.is_active_hours(),
        resume_hour=settings.schedule_start_hour,
        end_hour=settings.schedule_end_hour,
        color=color,
    )
    return Response(content=png_bytes, media_type="image/png")


@app.get("/screen.bmp")
def screen_bmp(
    battery: int | None = Query(default=None, ge=0, le=100, description="Battery percentage (0-100)"),
    style: str | None = Query(default=None, description="Display style: solari, matrix, plain, reverse"),
    mode: str = Query(default="auto", description="Display mode: auto, trains, clock"),
) -> Response:
    """Return an 800x480 1-bit monochrome Windows BMP image for e-paper displays."""
    settings = load_settings()
    result = get_departures(settings)
    active_style = style or settings.display_style
    bmp_bytes = render_screen_bmp(
        result,
        settings.direction,
        battery=battery,
        style=active_style,
        mode=mode,
        is_active=settings.is_active_hours(),
        resume_hour=settings.schedule_start_hour,
        end_hour=settings.schedule_end_hour,
    )
    return Response(content=bmp_bytes, media_type="image/bmp")


@app.get("/screen.bin")
def screen_bin(
    battery: int | None = Query(default=None, ge=0, le=100, description="Battery percentage (0-100)"),
    invert: bool = Query(default=False, description="Invert bit polarity (0=white, 1=black)"),
    style: str | None = Query(default=None, description="Display style: solari, matrix, plain, reverse"),
    mode: str = Query(default="auto", description="Display mode: auto, trains, clock"),
) -> Response:
    """Return an exact 48,000-byte raw 1-bit framebuffer for Waveshare 7.5inch e-paper displays."""
    settings = load_settings()
    result = get_departures(settings)
    active_style = style or settings.display_style
    raw_bytes = render_screen_raw(
        result,
        settings.direction,
        battery=battery,
        invert=invert,
        style=active_style,
        mode=mode,
        is_active=settings.is_active_hours(),
        resume_hour=settings.schedule_start_hour,
        end_hour=settings.schedule_end_hour,
    )
    return Response(
        content=raw_bytes,
        media_type="application/octet-stream",
        headers={"Content-Length": str(len(raw_bytes))},
    )


@app.get("/fonts/{font_name}")
def get_font(font_name: str) -> FileResponse:
    """Serve bundled TrueType font files for local offline preview rendering."""
    safe_name = Path(font_name).name
    font_path = Path(__file__).resolve().parent / "fonts" / safe_name
    if font_path.is_file():
        return FileResponse(font_path, media_type="font/ttf")
    raise HTTPException(status_code=404, detail="Font not found")


# --- HTML PREVIEW TEMPLATES FOR 3 STYLES ---

def _preview_solari(result, heading: str, invert: bool = False) -> str:
    bg_color = "#000" if invert else "#fff"
    text_color = "#fff" if invert else "#000"
    border_color = "#334155" if invert else "#0f172a"

    # Authentic DART Irish Rail livery green
    plaque_bg = "#00843D" if not invert else "#006b32"
    plaque_fg = "#ffffff"
    plaque_border = "#005a28" if not invert else "#00a84d"

    # Split seam lines
    split_seam_dark = "rgba(0,0,0,0.18)"
    split_seam_light = "rgba(255,255,255,0.22)"

    # Clock & Weather Card: Charcoal Slate
    clock_bg = "#0f172a" if not invert else "#18181b"
    clock_fg = "#f8fafc"
    clock_border = "#334155" if not invert else "#3f3f46"

    # Table headers
    th_bg = "#1e293b" if not invert else "#27272a"
    th_fg = "#f8fafc"

    # Tiles
    tile_bg = "#18181b" if invert else "#ffffff"
    tile_fg = "#f8fafc" if invert else "#0f172a"
    tile_border = "#3f3f46" if invert else "#000000"

    # Weather Module in Top Line
    weather_card_html = ""
    weather_parts = []
    if result.temperature is not None:
        weather_parts.append(f'<span class="weather-temp">{result.temperature:.1f}&deg;C</span>')

    if result.rain_chance is not None:
        if result.rain_chance >= 40:
            weather_parts.append(f'<span class="weather-pill weather-rain">☔ {result.rain_chance}% RAIN</span>')
        else:
            weather_parts.append(f'<span class="weather-pill weather-dry">☀ {result.rain_chance}% RAIN</span>')

    if weather_parts:
        weather_card_html = f'<div class="weather-card">{"".join(weather_parts)}</div>'

    if result.departures:
        rows_list = []
        for d in result.departures[:5]:
            due_text = escape(d.to_dict()['due_in_text'].upper())
            if d.due_in <= 0:
                due_extra = "due-badge due-now"
            elif d.due_in <= 3:
                due_extra = "due-soon"
            else:
                due_extra = ""

            st_raw = d.status if d.status != 'No Information' else 'On Time'
            st_upper = escape(st_raw.upper())
            st_lower = st_raw.lower()
            if "late" in st_lower or "delay" in st_lower:
                st_class = "status-delayed"
            elif "cancel" in st_lower:
                st_class = "status-cancelled"
            elif "on time" in st_lower or "early" in st_lower:
                st_class = "status-ontime"
            else:
                st_class = ""

            rows_list.append(
                f"""
                <tr class="flap-row">
                  <td class="dest-col"><div class="flap-tile dest-tile">{escape(d.destination.upper())}</div></td>
                  <td class="due-col"><div class="flap-tile due-tile {due_extra}">{due_text}</div></td>
                  <td class="time-col"><div class="flap-tile time-tile">{escape(d.expected)}</div></td>
                  <td class="status-col"><div class="flap-tile status-tile {st_class}">{st_upper}</div></td>
                </tr>
                """
            )
        rows = "".join(rows_list)
    else:
        rows = '<tr class="flap-row"><td colspan="4" class="empty"><div class="flap-tile empty-tile">No departures found</div></td></tr>'

    return f"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="60">
<title>DART - {escape(heading)}</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Bebas+Neue&family=Oswald:wght@400;600;700&display=swap" rel="stylesheet">
<style>
  @font-face {{
    font-family: 'Bebas Neue';
    src: url('/fonts/BebasNeue.ttf') format('truetype');
  }}
  @font-face {{
    font-family: 'Oswald';
    src: url('/fonts/Oswald.ttf') format('truetype');
  }}
  * {{ box-sizing: border-box; }}
  html, body {{
    width: 100%; height: 100%; min-width: 800px; min-height: 480px; margin: 0; padding: 0;
    background: {bg_color}; color: {text_color};
    font-family: 'Bebas Neue', 'Oswald', sans-serif;
    overflow: hidden;
  }}
  body {{
    padding: 10px 14px;
    border: 3px solid {border_color};
    display: flex; flex-direction: column; justify-content: space-between;
  }}
  .header-bar {{ display: flex; gap: 8px; align-items: stretch; margin-bottom: 8px; }}
  .station-plaque {{
    flex: 1; background: {plaque_bg}; color: {plaque_fg};
    border: 2px solid {plaque_border}; border-radius: 4px;
    padding: 6px 14px; position: relative; overflow: hidden; display: flex; align-items: center;
    box-shadow: 0 2px 4px rgba(0,0,0,0.15);
  }}
  .station-plaque::after, .weather-card::after, .clock-card::after {{
    content: ""; position: absolute; left: 0; right: 0; top: 50%; height: 1px;
    background: {split_seam_light}; pointer-events: none;
  }}
  h1 {{ font-size: 36px; margin: 0; font-weight: normal; letter-spacing: 1.5px; text-transform: uppercase; line-height: 1; }}
  .weather-card {{
    background: {clock_bg}; color: {clock_fg};
    border: 2px solid {clock_border}; border-radius: 4px; padding: 4px 16px;
    position: relative; overflow: hidden; display: flex; align-items: center; justify-content: center;
    gap: 10px; font-family: 'Oswald', 'Bebas Neue', sans-serif;
    box-shadow: 0 2px 4px rgba(0,0,0,0.2);
  }}
  .weather-temp {{
    font-family: 'Bebas Neue', sans-serif; font-size: 38px; letter-spacing: 1.5px;
    color: #f8fafc; line-height: 1;
  }}
  .weather-pill {{
    padding: 3px 9px; border-radius: 4px; font-weight: 700; font-size: 14px;
    letter-spacing: 0.5px; display: inline-flex; align-items: center; gap: 4px;
    font-family: 'Oswald', sans-serif; text-transform: uppercase;
  }}
  .weather-pill.weather-rain {{
    background: #0284c7; color: #ffffff;
  }}
  .weather-pill.weather-dry {{
    background: #f59e0b; color: #1c1917;
  }}
  .clock-card {{
    min-width: 136px; background: {clock_bg}; color: {clock_fg};
    border: 2px solid {clock_border}; border-radius: 4px; padding: 4px 14px;
    font-size: 44px; position: relative; overflow: hidden; display: flex; align-items: center; justify-content: center; letter-spacing: 2px;
    box-shadow: 0 2px 4px rgba(0,0,0,0.2);
  }}
  table {{ width: 100%; border-collapse: separate; border-spacing: 0 6px; margin: 0; }}
  thead th {{ background: {th_bg}; color: {th_fg}; padding: 6px 12px; font-size: 19px; font-weight: normal; letter-spacing: 1px; text-transform: uppercase; border-radius: 3px; }}
  th.dest {{ text-align: left; }}
  th.due, th.time, th.status {{ text-align: right; }}
  .flap-row td {{ padding: 0; }}
  .flap-tile {{
    background: {tile_bg}; color: {tile_fg}; border: 2px solid {tile_border}; border-radius: 4px;
    padding: 4px 12px; position: relative; overflow: hidden; font-size: 32px; letter-spacing: 1px; display: flex; align-items: center; height: 52px;
  }}
  .flap-tile::after {{
    content: ""; position: absolute; left: 0; right: 0; top: 50%; height: 1px;
    background: {'rgba(255,255,255,0.15)' if invert else 'rgba(0,0,0,0.18)'}; pointer-events: none;
  }}
  .dest-col {{ width: 50%; }}
  .due-col {{ width: 16%; padding-left: 6px !important; }}
  .time-col {{ width: 15%; padding-left: 6px !important; }}
  .status-col {{ width: 19%; padding-left: 6px !important; }}
  .due-tile, .time-tile, .status-tile {{ justify-content: flex-end; }}
  .due-badge, .due-now {{
    background: #f59e0b !important; color: #1c1917 !important;
    font-weight: bold !important; border-color: #d97706 !important;
    box-shadow: 0 0 10px rgba(245, 158, 11, 0.35);
  }}
  .due-now::after {{
    background: rgba(0,0,0,0.2) !important;
  }}
  .due-soon {{
    background: {'#78350f' if invert else '#fef3c7'} !important;
    color: {'#fef3c7' if invert else '#92400e'} !important;
    font-weight: bold !important;
    border-color: {'#b45309' if invert else '#fde68a'} !important;
  }}
  .status-ontime {{
    color: {'#4ade80' if invert else '#15803d'} !important;
    font-weight: 600 !important;
  }}
  .status-delayed {{
    background: {'#7f1d1d' if invert else '#fee2e2'} !important;
    color: {'#fecaca' if invert else '#dc2626'} !important;
    font-weight: bold !important;
    border-color: {'#991b1b' if invert else '#fca5a5'} !important;
  }}
  .status-delayed::after {{
    background: {'rgba(255,255,255,0.2)' if invert else 'rgba(220,38,38,0.25)'} !important;
  }}
  .status-cancelled {{
    background: #ef4444 !important; color: #ffffff !important;
    font-weight: bold !important; border-color: #dc2626 !important;
  }}
  .empty-tile {{ justify-content: center; font-size: 24px; font-style: italic; height: 140px; }}
  .footer-strip {{
    display: flex; justify-content: space-between; padding-top: 4px; border-top: 1px solid {border_color};
    font-family: 'Oswald', sans-serif; font-size: 11px; letter-spacing: 1px; text-transform: uppercase; opacity: 0.85;
  }}
</style>
</head>
<body>
  <div>
    <div class="header-bar">
      <div class="station-plaque"><h1>DART &mdash; {escape(heading)}</h1></div>
      {weather_card_html}
      <div class="clock-card">{datetime.now().strftime('%H:%M')}</div>
    </div>
    <table>
      <thead>
        <tr>
          <th class="dest">Destination</th>
          <th class="due">Due</th>
          <th class="time">Expected</th>
          <th class="status">Status</th>
        </tr>
      </thead>
      <tbody>{rows}</tbody>
    </table>
  </div>
  <div class="footer-strip">
    <span>LIVE IRISH RAIL REAL-TIME DATA &bull; SOLARI SPLIT-FLAP TRANSIT DISPLAY</span>
    <span>ADAFRUIT ESP32-S3</span>
  </div>
</body>
</html>"""


def _preview_matrix(result, heading: str, invert: bool = False) -> str:
    bg_color = "#f4f4f5" if invert else "#0a0c10"
    panel_bg = "#ffffff" if invert else "#050608"
    text_color = "#111827" if invert else "#ffb000"  # Classic transit amber LED
    dim_color = "#6b7280" if invert else "#b47800"
    border_color = "#d1d5db" if invert else "#374151"
    frame_border = "#9ca3af" if invert else "#1f2937"
    led_glow = "none" if invert else "0 0 8px rgba(255, 176, 0, 0.45)"
    badge_bg = "#111827" if invert else "#ffb000"
    badge_fg = "#ffffff" if invert else "#000000"

    # Multi-color transit LED matrix colors
    led_green = "#16a34a" if invert else "#22c55e"
    led_red = "#dc2626" if invert else "#ef4444"
    led_cyan = "#0284c7" if invert else "#38bdf8"
    glow_green = "none" if invert else "0 0 8px rgba(34, 197, 94, 0.45)"
    glow_red = "none" if invert else "0 0 8px rgba(239, 68, 68, 0.45)"
    glow_cyan = "none" if invert else "0 0 8px rgba(56, 189, 248, 0.45)"

    weather_parts = []
    if result.temperature is not None:
        weather_parts.append(f'<span style="color: #f8fafc; font-size: 26px; font-weight: 800; letter-spacing: 1px;">{result.temperature:.1f}&deg;C</span>')
    if result.rain_chance is not None:
        symbol = "☔" if result.rain_chance >= 40 else "☀"
        weather_parts.append(f'<span style="color: {led_cyan}; text-shadow: {glow_cyan}; font-size: 20px;">{symbol} {result.rain_chance}% RAIN</span>')
    matrix_weather_html = f'<div class="matrix-weather" style="display: flex; gap: 12px; align-items: center;">{"".join(weather_parts)}</div>' if weather_parts else ""

    if result.departures:
        rows_list = []
        for i, d in enumerate(result.departures[:5]):
            st_raw = d.status if d.status != 'No Information' else 'On Time'
            st_upper = escape(st_raw.upper())
            st_lower = st_raw.lower()
            if "late" in st_lower or "delay" in st_lower or "cancel" in st_lower:
                st_style = f"color: {led_red}; text-shadow: {glow_red};"
            elif "on time" in st_lower or "early" in st_lower:
                st_style = f"color: {led_green}; text-shadow: {glow_green};"
            else:
                st_style = ""

            rows_list.append(
                f"""
                <tr class="matrix-row">
                  <td class="dest-col"><span class="matrix-dest">{i + 1}  {escape(d.destination.upper())}</span></td>
                  <td class="due-col"><span class="matrix-due {'due-flash' if d.due_in <= 0 else ''}">{escape(d.to_dict()['due_in_text'].upper())}</span></td>
                  <td class="time-col"><span class="matrix-time">{escape(d.expected)}</span></td>
                  <td class="status-col"><span class="matrix-status" style="{st_style}">{st_upper}</span></td>
                </tr>
                """
            )
        rows = "".join(rows_list)
    else:
        rows = '<tr><td colspan="4" class="empty-matrix">NO UPCOMING DEPARTURES</td></tr>'

    return f"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="60">
<title>DART - {escape(heading)}</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Doto:wght@700;800&display=swap" rel="stylesheet">
<style>
  @font-face {{
    font-family: 'Doto';
    src: url('/fonts/Doto.ttf') format('truetype');
  }}
  * {{ box-sizing: border-box; }}
  html, body {{
    width: 100%; height: 100%; min-width: 800px; min-height: 480px; margin: 0; padding: 0;
    background: {bg_color}; color: {text_color};
    font-family: 'Doto', monospace, sans-serif;
    font-weight: 700;
    overflow: hidden;
  }}
  body {{
    padding: 12px 16px;
    display: flex; flex-direction: column; justify-content: space-between;
  }}
  .chassis {{
    background: {panel_bg};
    border: 3px solid {frame_border};
    border-radius: 6px;
    padding: 10px 14px;
    box-shadow: inset 0 0 16px rgba(0,0,0,0.8);
    height: 100%;
    display: flex; flex-direction: column; justify-content: space-between;
  }}
  .matrix-header {{
    display: flex; justify-content: space-between; align-items: center;
    padding-bottom: 8px; border-bottom: 2px solid {border_color};
    text-shadow: {led_glow};
  }}
  h1 {{
    font-size: 26px; font-weight: 800; margin: 0;
    letter-spacing: 1px; text-transform: uppercase;
  }}
  .matrix-clock {{
    font-size: 30px; font-weight: 800; letter-spacing: 1.5px;
  }}
  table {{
    width: 100%; border-collapse: collapse; margin-top: 8px;
  }}
  thead th {{
    font-size: 15px; color: {dim_color}; font-weight: 700;
    padding: 6px 6px 8px 6px; border-bottom: 1px solid {border_color};
    letter-spacing: 1px; text-transform: uppercase;
  }}
  th.dest {{ text-align: left; width: 48%; }}
  th.due {{ text-align: right; width: 18%; }}
  th.time {{ text-align: right; width: 16%; }}
  th.status {{ text-align: right; width: 18%; }}
  .matrix-row td {{
    padding: 10px 6px;
    border-bottom: 1px dotted {border_color};
    text-shadow: {led_glow};
    font-size: 23px;
    letter-spacing: 1px;
  }}
  .dest-col {{ text-align: left; }}
  .due-col, .time-col, .status-col {{ text-align: right; }}
  .due-flash {{
    background: {badge_bg}; color: {badge_fg} !important;
    padding: 2px 8px; border-radius: 4px; text-shadow: none !important;
  }}
  .empty-matrix {{
    text-align: center; padding: 50px 0; font-size: 24px; color: {dim_color};
  }}
  .matrix-footer {{
    display: flex; justify-content: space-between; align-items: center;
    padding-top: 6px; border-top: 2px solid {border_color};
    font-size: 13px; color: {dim_color}; letter-spacing: 0.5px;
  }}
</style>
</head>
<body>
  <div class="chassis">
    <div>
      <div class="matrix-header">
        <h1 style="color: {led_green}; text-shadow: {glow_green};">DART :: {escape(heading.upper())}</h1>
        {matrix_weather_html}
        <div class="matrix-clock">{datetime.now().strftime('%H:%M')}</div>
      </div>
      <table>
        <thead>
          <tr>
            <th class="dest">Destination</th>
            <th class="due">Due</th>
            <th class="time">Expected</th>
            <th class="status">Status</th>
          </tr>
        </thead>
        <tbody>{rows}</tbody>
      </table>
    </div>
    <div class="matrix-footer">
      <span>● DOT MATRIX PLATFORM INDICATOR</span>
      <span>ADAFRUIT ESP32-S3</span>
    </div>
  </div>
</body>
</html>"""


def _preview_plain(result, heading: str, invert: bool = False) -> str:
    bg_color = "#000" if invert else "#fff"
    text_color = "#fff" if invert else "#000"
    border_color = "#334155" if invert else "#0f172a"
    accent_green = "#4ade80" if invert else "#00843D"
    status_ontime_color = "#4ade80" if invert else "#16a34a"
    status_delayed_color = "#f87171" if invert else "#dc2626"

    weather_parts = []
    if result.temperature is not None:
        weather_parts.append(f'<span style="font-size: 26px; font-weight: 700; margin-right: 8px;">{result.temperature:.1f}&deg;C</span>')
    if result.rain_chance is not None:
        symbol = "☔" if result.rain_chance >= 40 else "☀"
        weather_color = "#38bdf8" if invert else "#0284c7"
        weather_parts.append(f'<span style="color: {weather_color}; font-weight: 600;">{symbol} {result.rain_chance}% Rain</span>')
    weather_html = " ".join(weather_parts)

    if result.departures:
        rows_list = []
        for d in result.departures[:5]:
            due_text = escape(d.to_dict()['due_in_text'])
            if d.due_in <= 0:
                due_cell = f'<span style="background: #f59e0b; color: #1c1917; padding: 2px 8px; border-radius: 4px; font-weight: 700;">{due_text}</span>'
            else:
                due_cell = due_text

            st_raw = d.status if d.status != 'No Information' else 'On Time'
            st_lower = st_raw.lower()
            if "late" in st_lower or "delay" in st_lower or "cancel" in st_lower:
                st_cell = f'<span style="color: {status_delayed_color}; font-weight: 700;">{escape(st_raw)}</span>'
            elif "on time" in st_lower or "early" in st_lower:
                st_cell = f'<span style="color: {status_ontime_color}; font-weight: 600;">{escape(st_raw)}</span>'
            else:
                st_cell = escape(st_raw)

            rows_list.append(
                f"""
                <tr class="plain-row">
                  <td class="dest">{escape(d.destination)}</td>
                  <td class="due">{due_cell}</td>
                  <td class="time">{escape(d.expected)}</td>
                  <td class="status">{st_cell}</td>
                </tr>
                """
            )
        rows = "".join(rows_list)
    else:
        rows = '<tr><td colspan="4" class="empty-plain">No departures found</td></tr>'

    return f"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="60">
<title>DART - {escape(heading)}</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Oswald:wght@400;600&display=swap" rel="stylesheet">
<style>
  @font-face {{
    font-family: 'Oswald';
    src: url('/fonts/Oswald.ttf') format('truetype');
  }}
  * {{ box-sizing: border-box; }}
  html, body {{
    width: 100%; height: 100%; min-width: 800px; min-height: 480px; margin: 0; padding: 0;
    background: {bg_color}; color: {text_color};
    font-family: 'Oswald', -apple-system, sans-serif;
    overflow: hidden;
  }}
  body {{
    padding: 14px 20px;
    display: flex; flex-direction: column; justify-content: space-between;
  }}
  .plain-header {{
    display: flex; justify-content: space-between; align-items: baseline;
    border-bottom: 2px solid {border_color}; padding-bottom: 4px; margin-bottom: 8px;
  }}
  h1 {{
    font-size: 32px; font-weight: 600; margin: 0;
    border-left: 4px solid {accent_green}; padding-left: 10px;
  }}
  .plain-weather {{ font-size: 22px; font-weight: 600; letter-spacing: 0.5px; }}
  .plain-clock {{ font-size: 38px; font-weight: 600; }}
  table {{ width: 100%; border-collapse: collapse; }}
  th {{
    text-align: left; font-size: 15px; font-weight: 600; letter-spacing: 1px;
    border-bottom: 2px solid {border_color}; padding: 4px 6px;
  }}
  th.due, th.time, th.status {{ text-align: right; }}
  td {{
    padding: 10px 6px; border-bottom: 1px solid {border_color};
  }}
  .dest {{ font-size: 24px; font-weight: 600; }}
  .due {{ text-align: right; font-size: 22px; font-weight: 600; }}
  .time {{ text-align: right; font-size: 22px; }}
  .status {{ text-align: right; font-size: 18px; }}
  .empty-plain {{ text-align: center; padding: 40px; font-size: 22px; font-style: italic; }}
  .plain-footer {{
    display: flex; justify-content: space-between; font-size: 12px;
    border-top: 1px solid {border_color}; padding-top: 6px;
  }}
</style>
</head>
<body>
  <div>
    <div class="plain-header">
      <h1>DART &mdash; {escape(heading)}</h1>
      <div class="plain-weather">{weather_html}</div>
      <div class="plain-clock">{datetime.now().strftime('%H:%M')}</div>
    </div>
    <table>
      <thead>
        <tr>
          <th>Destination</th>
          <th class="due">Due</th>
          <th class="time">Expected</th>
          <th class="status">Status</th>
        </tr>
      </thead>
      <tbody>{rows}</tbody>
    </table>
  </div>
  <div class="plain-footer">
    <span><span style="color: {status_ontime_color};">●</span> LIVE IRISH RAIL REAL-TIME DATA</span>
    <span>UPDATED: {datetime.now().strftime('%H:%M:%S')}</span>
    <span>ADAFRUIT ESP32-S3</span>
  </div>
</body>
</html>"""


def _preview_clock_weather(result, heading: str, invert: bool = False, resume_hour: int = 6, end_hour: int = 21) -> str:
    bg_color = "#000" if invert else "#fff"
    text_color = "#fff" if invert else "#000"
    border_color = "#334155" if invert else "#0f172a"
    card_bg = "#18181b" if invert else "#f8fafc"
    card_border = "#3f3f46" if invert else "#cbd5e1"
    badge_bg = "#312e81" if invert else "#1e293b"
    badge_text = "#c7d2fe" if invert else "#f8fafc"
    accent_green = "#4ade80" if invert else "#00843D"

    now = datetime.now()
    now_str = now.strftime("%H:%M")
    date_str = now.strftime("%A, %d %B %Y").upper()

    tw = result.tomorrow_weather
    if tw is not None:
        cond_html = f"{tw.symbol} {escape(tw.condition.upper())}"
        temp_html = f'High: <strong style="color: #ea580c;">{tw.temp_max:.1f}&deg;C</strong> &bull; Low: <strong style="color: #0284c7;">{tw.temp_min:.1f}&deg;C</strong>'
        rain_html = f"Precipitation: {tw.rain_chance}%"
        if tw.rain_chance >= 40:
            tip_html = "Heavy rain expected &mdash; pack an umbrella"
        elif tw.rain_chance >= 20:
            tip_html = "Light rain showers possible during commute"
        else:
            tip_html = "Dry & clear conditions expected for morning commute"
        date_sub = f"Forecast Date: {escape(tw.date_str)}"
    else:
        cond_html = "Forecast Unavailable"
        temp_html = "--&deg;C / --&deg;C"
        rain_html = "Rain: --%"
        tip_html = "Open-Meteo weather service offline"
        date_sub = "Tomorrow"

    return f"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="60">
<title>DART - Standby Clock & Tomorrow's Weather</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Bebas+Neue&family=Oswald:wght@400;600;700&display=swap" rel="stylesheet">
<style>
  @font-face {{
    font-family: 'Bebas Neue';
    src: url('/fonts/BebasNeue.ttf') format('truetype');
  }}
  @font-face {{
    font-family: 'Oswald';
    src: url('/fonts/Oswald.ttf') format('truetype');
  }}
  * {{ box-sizing: border-box; }}
  html, body {{
    width: 100%; height: 100%; min-width: 800px; min-height: 480px; margin: 0; padding: 0;
    background: {bg_color}; color: {text_color};
    font-family: 'Oswald', -apple-system, sans-serif;
    overflow: hidden;
  }}
  body {{
    padding: 16px 20px;
    display: flex; flex-direction: column; justify-content: space-between;
  }}
  .clock-header {{
    display: flex; justify-content: space-between; align-items: baseline;
    border-bottom: 2px solid {border_color}; padding-bottom: 6px;
  }}
  .clock-title {{ font-size: 24px; font-weight: 700; letter-spacing: 1px; color: {accent_green}; }}
  .clock-badge {{
    font-size: 14px; font-weight: 600; letter-spacing: 0.5px;
    background: {badge_bg}; color: {badge_text};
    padding: 4px 10px; border-radius: 4px;
    border: 1px solid {'#4338ca' if invert else '#334155'};
  }}
  .main-split {{
    display: grid; grid-template-columns: 1fr 1fr; gap: 24px;
    align-items: center; margin: 10px 0;
  }}
  .clock-section {{
    display: flex; flex-direction: column;
  }}
  .huge-clock {{
    font-family: 'Bebas Neue', sans-serif;
    font-size: 116px; line-height: 0.9; margin: 0;
    letter-spacing: 2px;
  }}
  .huge-date {{
    font-size: 19px; font-weight: 600; margin-top: 8px;
    letter-spacing: 0.5px; opacity: 0.9;
  }}
  .sched-box {{
    margin-top: 14px; padding: 10px 14px;
    border: 1px solid {card_border}; border-radius: 6px;
    background: {card_bg};
    font-size: 13px; line-height: 1.5;
  }}
  .sched-title {{ font-weight: 700; text-transform: uppercase; margin-bottom: 4px; }}
  .weather-card {{
    border: 2px solid {card_border}; border-radius: 8px;
    padding: 18px 20px; background: {card_bg};
  }}
  .weather-head {{
    font-family: 'Bebas Neue', sans-serif; font-size: 26px;
    letter-spacing: 1px; margin-bottom: 10px; border-bottom: 1px solid {card_border};
    padding-bottom: 4px; color: {accent_green};
  }}
  .weather-cond {{
    font-size: 26px; font-weight: 700; margin-bottom: 6px; color: {'#38bdf8' if invert else '#0284c7'};
  }}
  .weather-temp {{
    font-size: 20px; font-weight: 600; margin-bottom: 12px;
  }}
  .rain-badge {{
    display: inline-block; background: {'#0369a1' if invert else '#0284c7'}; color: #ffffff;
    padding: 6px 14px; border-radius: 6px; font-weight: 700; font-size: 15px;
    margin-bottom: 12px;
  }}
  .weather-tip {{
    font-size: 14px; font-weight: 600; margin-top: 6px;
    color: {'#93c5fd' if invert else '#0369a1'};
    background: {'#1e293b' if invert else '#e0f2fe'};
    padding: 6px 10px; border-radius: 4px;
  }}
  .clock-footer {{
    display: flex; justify-content: space-between; font-size: 12px;
    border-top: 1px solid {border_color}; padding-top: 8px;
  }}
</style>
</head>
<body>
  <div class="clock-header">
    <div class="clock-title">DART &bull; {escape(heading.upper())}</div>
    <div class="clock-badge">🌙 NIGHT STANDBY &bull; RESUMES {resume_hour:02d}:00</div>
  </div>

  <div class="main-split">
    <div class="clock-section">
      <div class="huge-clock">{now_str}</div>
      <div class="huge-date">{date_str}</div>
      <div class="sched-box">
        <div class="sched-title">Display Schedule Status</div>
        <div><span style="color: {accent_green};">&bull;</span> Active Commute Window: {resume_hour:02d}:00 &ndash; {end_hour:02d}:00</div>
        <div><span style="color: {accent_green};">&bull;</span> Current Mode: Down Period (Night Standby)</div>
        <div><span style="color: {accent_green};">&bull;</span> Station: {escape(result.station_name)} ({result.station_code})</div>
      </div>
    </div>

    <div class="weather-card">
      <div class="weather-head">TOMORROW'S COMMUTE FORECAST</div>
      <div class="weather-cond">{cond_html}</div>
      <div class="weather-temp">{temp_html}</div>
      <div class="rain-badge">{rain_html}</div>
      <div class="weather-tip">{tip_html}</div>
      <div style="font-size: 11px; opacity: 0.7; margin-top: 10px;">{date_sub}</div>
    </div>
  </div>

  <div class="clock-footer">
    <span>LIVE IRISH RAIL REAL-TIME DATA &bull; WEATHER DATA BY <a href="https://open-meteo.com/" target="_blank" style="color:inherit; text-decoration:underline;">OPEN-METEO</a></span>
    <span>UPDATED: {now.strftime('%H:%M:%S')}</span>
    <span>ADAFRUIT ESP32-S3</span>
  </div>
</body>
</html>"""


@app.get("/preview", response_class=HTMLResponse)
def preview(
    invert: bool = Query(default=False, description="Invert colors for dark display"),
    style: str | None = Query(default=None, description="Display style: solari, matrix, plain, reverse"),
    mode: str = Query(default="auto", description="Display mode: auto, trains, clock"),
) -> HTMLResponse:
    settings = load_settings()
    result = get_departures(settings)

    heading = result.station_name
    if settings.direction:
        heading += f" ({settings.direction})"

    active_style = (style or settings.display_style).strip().lower()
    is_invert = invert or (active_style in ("reverse", "inverted", "dark"))
    is_active = settings.is_active_hours()

    req_mode = (mode or "auto").strip().lower()
    if req_mode == "clock" or (req_mode == "auto" and not is_active):
        html = _preview_clock_weather(
            result,
            heading,
            invert=is_invert,
            resume_hour=settings.schedule_start_hour,
            end_hour=settings.schedule_end_hour,
        )
        return HTMLResponse(content=html)

    if active_style in ("reverse", "inverted", "dark"):
        html = _preview_plain(result, heading, invert=True)
    elif active_style in ("matrix", "dotmatrix", "modern"):
        html = _preview_matrix(result, heading, invert=invert)
    elif active_style == "plain":
        html = _preview_plain(result, heading, invert=invert)
    else:
        html = _preview_solari(result, heading, invert=invert)

    return HTMLResponse(content=html)
