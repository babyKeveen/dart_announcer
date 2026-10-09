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
) -> Response:
    """Return an 800x480 1-bit monochrome PNG image for e-paper displays."""
    settings = load_settings()
    result = get_departures(settings)
    active_style = style or settings.display_style
    png_bytes = render_screen_png(result, settings.direction, battery=battery, style=active_style)
    return Response(content=png_bytes, media_type="image/png")


@app.get("/screen.bmp")
def screen_bmp(
    battery: int | None = Query(default=None, ge=0, le=100, description="Battery percentage (0-100)"),
    style: str | None = Query(default=None, description="Display style: solari, matrix, plain, reverse"),
) -> Response:
    """Return an 800x480 1-bit monochrome Windows BMP image for e-paper displays."""
    settings = load_settings()
    result = get_departures(settings)
    active_style = style or settings.display_style
    bmp_bytes = render_screen_bmp(result, settings.direction, battery=battery, style=active_style)
    return Response(content=bmp_bytes, media_type="image/bmp")


@app.get("/screen.bin")
def screen_bin(
    battery: int | None = Query(default=None, ge=0, le=100, description="Battery percentage (0-100)"),
    invert: bool = Query(default=False, description="Invert bit polarity (0=white, 1=black)"),
    style: str | None = Query(default=None, description="Display style: solari, matrix, plain, reverse"),
) -> Response:
    """Return an exact 48,000-byte raw 1-bit framebuffer for Waveshare 7.5inch e-paper displays."""
    settings = load_settings()
    result = get_departures(settings)
    active_style = style or settings.display_style
    raw_bytes = render_screen_raw(result, settings.direction, battery=battery, invert=invert, style=active_style)
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
    border_color = "#fff" if invert else "#000"
    card_bg = "#000" if not invert else "#fff"
    card_fg = "#fff" if not invert else "#000"
    tile_bg = "#111" if invert else "#fff"
    tile_fg = "#fff" if invert else "#000"
    tile_border = "#444" if invert else "#000"
    split_seam = "rgba(255,255,255,0.25)" if invert else "rgba(0,0,0,0.3)"

    weather_html = ""
    if result.rain_chance is not None:
        symbol = "☔" if result.rain_chance >= 40 else "☀"
        weather_html = f'<span class="weather-item">{symbol} {result.rain_chance}% RAIN</span>'

    if result.departures:
        rows = "".join(
            f"""
            <tr class="flap-row">
              <td class="dest-col"><div class="flap-tile dest-tile">{escape(d.destination.upper())}</div></td>
              <td class="due-col"><div class="flap-tile due-tile {'due-badge' if d.due_in <= 0 else ''}">{escape(d.to_dict()['due_in_text'].upper())}</div></td>
              <td class="time-col"><div class="flap-tile time-tile">{escape(d.expected)}</div></td>
              <td class="status-col"><div class="flap-tile status-tile">{escape((d.status if d.status != 'No Information' else 'On Time').upper())}</div></td>
            </tr>
            """
            for d in result.departures[:5]
        )
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
    width: 800px; height: 480px; margin: 0; padding: 0;
    background: {bg_color}; color: {text_color};
    font-family: 'Bebas Neue', 'Oswald', sans-serif;
    overflow: hidden;
  }}
  body {{
    padding: 10px 14px;
    border: 3px solid {border_color};
    display: flex; flex-direction: column; justify-content: space-between;
  }}
  .header-bar {{ display: flex; gap: 8px; align-items: stretch; margin-bottom: 6px; }}
  .station-plaque {{
    flex: 1; background: {card_bg}; color: {card_fg};
    border: 2px solid {border_color}; border-radius: 3px;
    padding: 6px 14px; position: relative; overflow: hidden; display: flex; align-items: center;
  }}
  .station-plaque::after, .clock-card::after, .flap-tile::after {{
    content: ""; position: absolute; left: 0; right: 0; top: 50%; height: 1px;
    background: {split_seam}; pointer-events: none;
  }}
  h1 {{ font-size: 36px; margin: 0; font-weight: normal; letter-spacing: 1.5px; text-transform: uppercase; line-height: 1; }}
  .clock-card {{
    min-width: 136px; background: {card_bg}; color: {card_fg};
    border: 2px solid {border_color}; border-radius: 3px; padding: 4px 14px;
    font-size: 44px; position: relative; overflow: hidden; display: flex; align-items: center; justify-content: center; letter-spacing: 2px;
  }}
  .info-strip {{
    display: flex; justify-content: space-between; align-items: center;
    background: {bg_color}; color: {text_color}; border: 1px solid {border_color};
    border-radius: 2px; padding: 3px 10px; font-family: 'Oswald', sans-serif; font-size: 13px; letter-spacing: 1px; text-transform: uppercase; margin-bottom: 6px;
  }}
  table {{ width: 100%; border-collapse: separate; border-spacing: 0 4px; margin: 0; }}
  thead th {{ background: {card_bg}; color: {card_fg}; padding: 4px 10px; font-size: 18px; font-weight: normal; letter-spacing: 1px; text-transform: uppercase; }}
  th.dest {{ text-align: left; }}
  th.due, th.time, th.status {{ text-align: right; }}
  .flap-row td {{ padding: 0; }}
  .flap-tile {{
    background: {tile_bg}; color: {tile_fg}; border: 2px solid {tile_border}; border-radius: 3px;
    padding: 4px 10px; position: relative; overflow: hidden; font-size: 30px; letter-spacing: 1px; display: flex; align-items: center; height: 48px;
  }}
  .dest-col {{ width: 50%; }}
  .due-col {{ width: 16%; padding-left: 6px !important; }}
  .time-col {{ width: 15%; padding-left: 6px !important; }}
  .status-col {{ width: 19%; padding-left: 6px !important; }}
  .due-tile, .time-tile, .status-tile {{ justify-content: flex-end; }}
  .due-badge {{ background: {card_bg} !important; color: {card_fg} !important; font-weight: bold; }}
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
      <div class="clock-card">{datetime.now().strftime('%H:%M')}</div>
    </div>
    <div class="info-strip">
      <span>STATION: {result.station_code}</span>
      {weather_html}
      <span>FEED: IRISH RAIL REAL-TIME</span>
      <span>UPDATED: {datetime.now().strftime('%H:%M:%S')}</span>
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

    weather_html = ""
    if result.rain_chance is not None:
        symbol = "☔" if result.rain_chance >= 40 else "☀"
        weather_html = f'<span class="weather-item">{symbol} {result.rain_chance}% RAIN</span>'

    if result.departures:
        rows = "".join(
            f"""
            <tr class="matrix-row">
              <td class="dest-col"><span class="matrix-dest">{i + 1}  {escape(d.destination.upper())}</span></td>
              <td class="due-col"><span class="matrix-due {'due-flash' if d.due_in <= 0 else ''}">{escape(d.to_dict()['due_in_text'].upper())}</span></td>
              <td class="time-col"><span class="matrix-time">{escape(d.expected)}</span></td>
              <td class="status-col"><span class="matrix-status">{escape((d.status if d.status != 'No Information' else 'On Time').upper())}</span></td>
            </tr>
            """
            for i, d in enumerate(result.departures[:5])
        )
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
    width: 800px; height: 480px; margin: 0; padding: 0;
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
  .matrix-sub {{
    display: flex; justify-content: space-between; align-items: center;
    font-size: 13px; color: {dim_color}; padding: 4px 2px;
    border-bottom: 1px solid {border_color}; letter-spacing: 0.5px;
  }}
  table {{
    width: 100%; border-collapse: collapse; margin-top: 4px;
  }}
  thead th {{
    font-size: 14px; color: {dim_color}; font-weight: 700;
    padding: 4px 6px 6px 6px; border-bottom: 1px solid {border_color};
    letter-spacing: 1px; text-transform: uppercase;
  }}
  th.dest {{ text-align: left; width: 48%; }}
  th.due {{ text-align: right; width: 18%; }}
  th.time {{ text-align: right; width: 16%; }}
  th.status {{ text-align: right; width: 18%; }}
  .matrix-row td {{
    padding: 8px 6px;
    border-bottom: 1px dotted {border_color};
    text-shadow: {led_glow};
    font-size: 22px;
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
        <h1>DART :: {escape(heading.upper())}</h1>
        <div class="matrix-clock">{datetime.now().strftime('%H:%M')}</div>
      </div>
      <div class="matrix-sub">
        <span>● LIVE RAIL FEED</span>
        <span>STATION: {result.station_code}</span>
        {weather_html}
        <span>UPDATED: {datetime.now().strftime('%H:%M:%S')}</span>
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
    border_color = "#fff" if invert else "#000"

    weather_html = ""
    if result.rain_chance is not None:
        symbol = "☔" if result.rain_chance >= 40 else "☀"
        weather_html = f'<span>{symbol} {result.rain_chance}% Rain</span>'

    if result.departures:
        rows = "".join(
            f"""
            <tr class="plain-row">
              <td class="dest">{escape(d.destination)}</td>
              <td class="due">{escape(d.to_dict()['due_in_text'])}</td>
              <td class="time">{escape(d.expected)}</td>
              <td class="status">{escape(d.status if d.status != 'No Information' else 'On Time')}</td>
            </tr>
            """
            for d in result.departures[:5]
        )
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
    width: 800px; height: 480px; margin: 0; padding: 0;
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
  h1 {{ font-size: 32px; font-weight: 600; margin: 0; }}
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
    <span>LIVE IRISH RAIL REAL-TIME DATA</span>
    <span>UPDATED: {datetime.now().strftime('%H:%M:%S')}</span>
    <span>ADAFRUIT ESP32-S3</span>
  </div>
</body>
</html>"""


@app.get("/preview", response_class=HTMLResponse)
def preview(
    invert: bool = Query(default=False, description="Invert colors for dark display"),
    style: str | None = Query(default=None, description="Display style: solari, matrix, plain, reverse"),
) -> HTMLResponse:
    settings = load_settings()
    result = get_departures(settings)

    heading = result.station_name
    if settings.direction:
        heading += f" ({settings.direction})"

    active_style = (style or settings.display_style).strip().lower()
    if active_style in ("reverse", "inverted", "dark"):
        html = _preview_plain(result, heading, invert=True)
    elif active_style in ("matrix", "dotmatrix", "modern"):
        html = _preview_matrix(result, heading, invert=invert)
    elif active_style == "plain":
        html = _preview_plain(result, heading, invert=invert)
    else:
        html = _preview_solari(result, heading, invert=invert)

    return HTMLResponse(content=html)
