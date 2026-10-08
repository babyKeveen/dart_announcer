"""Configuration web frontend for DART Announce.

Runs on a dedicated port (e.g., 8080) allowing users to easily change the active
station, configure direction (Northbound, Southbound, or Both), and adjust
display parameters from any phone, tablet, or browser on the local Wi-Fi.
"""

from html import escape
from pathlib import Path
from typing import Literal

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from pydantic import BaseModel, Field

from .config import InvalidSettingsError, load_settings, save_settings
from .stations import DART_STATIONS, get_dart_stations, reload_dart_stations

app = FastAPI(title="DART Announce Configuration")


class ConfigPayload(BaseModel):
    station: str = Field(..., min_length=1, description="Station name or code")
    direction: Literal["Northbound", "Southbound", "Both", ""] = Field(
        default="Both",
        description="Direction to filter: Northbound, Southbound, or Both (all trains)",
    )
    display_style: Literal["solari", "matrix", "plain"] = Field(
        default="solari",
        description="Display aesthetic: solari (Solari di Udine), matrix (Dot Matrix Indicator), or plain (Plain Minimalist)",
    )
    num_mins: int = Field(default=90, ge=5, le=90, description="Lookahead minutes (5-90)")
    max_departures: int = Field(default=5, ge=1, le=10, description="Max departures (1-10)")


@app.get("/api/config")
def get_config_api():
    """Return active settings and available stations."""
    current = load_settings()
    return {
        "station": current.station,
        "direction": current.direction or "Both",
        "display_style": current.display_style,
        "num_mins": current.num_mins,
        "max_departures": current.max_departures,
        "stations": get_dart_stations(),
    }


@app.post("/api/config")
def update_config_api(payload: ConfigPayload):
    """Update settings via JSON API."""
    dir_val = None if payload.direction in ("Both", "") else payload.direction
    try:
        updated = save_settings(
            station=payload.station,
            direction=dir_val,
            display_style=payload.display_style,
            num_mins=payload.num_mins,
            max_departures=payload.max_departures,
        )
        return {
            "status": "success",
            "message": f"Updated station to '{updated.station}' ({updated.direction or 'Both directions'}, style: {updated.display_style})",
            "settings": {
                "station": updated.station,
                "direction": updated.direction or "Both",
                "display_style": updated.display_style,
                "num_mins": updated.num_mins,
                "max_departures": updated.max_departures,
            },
        }
    except InvalidSettingsError as exc:
        raise HTTPException(status_code=400, detail=str(exc))


@app.post("/api/stations/reload")
def reload_stations_api():
    """Reload official DART stations dynamically from Irish Rail's live API."""
    stations, is_live = reload_dart_stations()
    return {
        "status": "success",
        "live": is_live,
        "count": len(stations),
        "message": f"Loaded {len(stations)} DART stations" + (" live from Irish Rail API" if is_live else " from local fallback"),
        "stations": stations,
    }


@app.get("/reload-stations")
def reload_stations_get():
    """Trigger reload via standard browser GET request."""
    _, is_live = reload_dart_stations()
    return RedirectResponse(url="/?reloaded=1" if is_live else "/?reloaded=fallback", status_code=303)


from urllib.parse import parse_qs


@app.post("/save")
async def save_form(request: Request):
    """Handle standard HTML form submission without requiring third-party multipart libraries."""
    raw_body = await request.body()
    form_data = parse_qs(raw_body.decode("utf-8", errors="replace"))

    station = form_data.get("station", [""])[0].strip()
    direction = form_data.get("direction", ["Both"])[0].strip()
    display_style = form_data.get("display_style", ["solari"])[0].strip().lower()

    try:
        num_mins = int(form_data.get("num_mins", [90])[0])
    except ValueError:
        num_mins = 90

    try:
        max_departures = int(form_data.get("max_departures", [5])[0])
    except ValueError:
        max_departures = 5

    dir_val = None if direction in ("Both", "") else direction
    try:
        save_settings(
            station=station,
            direction=dir_val,
            display_style=display_style,
            num_mins=num_mins,
            max_departures=max_departures,
        )
        return RedirectResponse(url="/?saved=1", status_code=303)
    except InvalidSettingsError as exc:
        return RedirectResponse(url=f"/?error={escape(str(exc))}", status_code=303)


@app.get("/", response_class=HTMLResponse)
def index_page(
    request: Request,
    saved: bool = False,
    reloaded: str | None = None,
    error: str | None = None,
) -> HTMLResponse:
    """Render the configuration web dashboard."""
    settings = load_settings()
    current_dir = settings.direction or "Both"
    current_style = settings.display_style or "solari"
    host_no_port = request.url.hostname or "raspi2modelb2014.local"

    # Build station options HTML from dynamically loaded DART stations
    active_stations = get_dart_stations()
    station_options = []
    matched_known = False
    for st in active_stations:
        st_name = st["name"]
        st_code = st["code"]
        selected = "selected" if st_name.lower() == settings.station.lower() or st_code.lower() == settings.station.lower() else ""
        if selected:
            matched_known = True
        station_options.append(f'<option value="{escape(st_name)}" {selected}>{escape(st_name)} ({st_code})</option>')

    alert_html = ""
    if saved:
        alert_html = """
        <div class="alert alert-success">
          <span class="alert-icon">✓</span>
          <div>
            <strong>Settings saved!</strong> The departure board and e-ink feed updated immediately.
          </div>
        </div>
        """
    elif reloaded == "1":
        alert_html = f"""
        <div class="alert alert-success">
          <span class="alert-icon">✓</span>
          <div>
            <strong>Stations reloaded!</strong> Successfully refreshed {len(active_stations)} coastal DART stations live from Irish Rail API.
          </div>
        </div>
        """
    elif reloaded:
        alert_html = f"""
        <div class="alert alert-success">
          <span class="alert-icon">ℹ</span>
          <div>
            <strong>Stations refreshed!</strong> {len(active_stations)} stations loaded from local cache.
          </div>
        </div>
        """
    elif error:
        alert_html = f"""
        <div class="alert alert-error">
          <span class="alert-icon">⚠</span>
          <div><strong>Error saving settings:</strong> {escape(error)}</div>
        </div>
        """

    html = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DART Announce — Display Configuration</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Bebas+Neue&family=Inter:wght@400;500;600;700&family=Oswald:wght@500;700&display=swap" rel="stylesheet">
<style>
  :root {{
    --bg-page: #0f1117;
    --card-bg: #181b24;
    --card-border: #282d3c;
    --accent: #22c55e;
    --accent-hover: #16a34a;
    --accent-glow: rgba(34, 197, 94, 0.2);
    --text-primary: #f8fafc;
    --text-secondary: #94a3b8;
    --input-bg: #0d0f15;
    --input-border: #333a4d;
    --radio-active-bg: #1e293b;
    --radio-active-border: #38bdf8;
  }}

  * {{ box-sizing: border-box; margin: 0; padding: 0; }}
  body {{
    font-family: 'Inter', -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    background: var(--bg-page);
    color: var(--text-primary);
    min-height: 100vh;
    padding: 24px 16px;
    display: flex;
    justify-content: center;
  }}

  .container {{
    width: 100%;
    max-width: 840px;
  }}

  /* Header */
  .brand-header {{
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: 24px;
    padding-bottom: 16px;
    border-bottom: 1px solid var(--card-border);
    flex-wrap: wrap;
    gap: 12px;
  }}
  .brand-title {{
    display: flex;
    align-items: center;
    gap: 12px;
  }}
  .logo-badge {{
    background: #0284c7;
    color: #fff;
    font-family: 'Bebas Neue', sans-serif;
    font-size: 26px;
    letter-spacing: 1px;
    padding: 4px 10px;
    border-radius: 6px;
    line-height: 1;
  }}
  h1 {{
    font-size: 20px;
    font-weight: 700;
    letter-spacing: -0.5px;
  }}
  .subtitle {{
    font-size: 13px;
    color: var(--text-secondary);
  }}
  .status-pill {{
    display: inline-flex;
    align-items: center;
    gap: 6px;
    background: #064e3b;
    color: #34d399;
    font-size: 12px;
    font-weight: 600;
    padding: 4px 10px;
    border-radius: 999px;
  }}
  .status-dot {{
    width: 8px;
    height: 8px;
    background: #10b981;
    border-radius: 50%;
    box-shadow: 0 0 8px #10b981;
  }}

  /* Alerts */
  .alert {{
    display: flex;
    align-items: center;
    gap: 12px;
    padding: 12px 16px;
    border-radius: 8px;
    font-size: 14px;
    margin-bottom: 20px;
    animation: fadeIn 0.3s ease;
  }}
  .alert-success {{
    background: rgba(34, 197, 94, 0.12);
    border: 1px solid rgba(34, 197, 94, 0.3);
    color: #4ade80;
  }}
  .alert-error {{
    background: rgba(239, 68, 68, 0.12);
    border: 1px solid rgba(239, 68, 68, 0.3);
    color: #f87171;
  }}
  .alert-icon {{
    font-size: 18px;
    font-weight: bold;
  }}

  /* Main Card */
  .config-card {{
    background: var(--card-bg);
    border: 1px solid var(--card-border);
    border-radius: 12px;
    padding: 24px;
    box-shadow: 0 8px 24px rgba(0, 0, 0, 0.4);
    margin-bottom: 24px;
  }}

  .section-title {{
    font-size: 14px;
    font-weight: 700;
    text-transform: uppercase;
    letter-spacing: 0.8px;
    color: var(--text-secondary);
    margin-bottom: 14px;
  }}

  .form-group {{
    margin-bottom: 20px;
  }}
  label {{
    display: block;
    font-size: 14px;
    font-weight: 600;
    margin-bottom: 8px;
  }}
  .help-text {{
    font-size: 12px;
    color: var(--text-secondary);
    margin-top: 5px;
  }}

  select, input[type="text"], input[type="number"] {{
    width: 100%;
    padding: 10px 14px;
    background: var(--input-bg);
    border: 1px solid var(--input-border);
    border-radius: 8px;
    color: var(--text-primary);
    font-size: 15px;
    outline: none;
    transition: border-color 0.15s, box-shadow 0.15s;
  }}
  select:focus, input:focus {{
    border-color: #38bdf8;
    box-shadow: 0 0 0 3px rgba(56, 189, 248, 0.2);
  }}

  /* Direction Button Group */
  .direction-grid {{
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 10px;
  }}
  .direction-option {{
    position: relative;
  }}
  .direction-option input[type="radio"] {{
    position: absolute;
    opacity: 0;
    cursor: pointer;
  }}
  .direction-card {{
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 14px 10px;
    background: var(--input-bg);
    border: 2px solid var(--input-border);
    border-radius: 8px;
    cursor: pointer;
    transition: all 0.2s ease;
    text-align: center;
    user-select: none;
  }}
  .direction-card:hover {{
    border-color: #64748b;
  }}
  .direction-option input[type="radio"]:checked + .direction-card {{
    background: var(--radio-active-bg);
    border-color: var(--radio-active-border);
    box-shadow: 0 0 12px rgba(56, 189, 248, 0.25);
  }}
  .dir-icon {{
    font-size: 22px;
    margin-bottom: 4px;
  }}
  .dir-title {{
    font-size: 14px;
    font-weight: 700;
  }}
  .dir-sub {{
    font-size: 11px;
    color: var(--text-secondary);
    margin-top: 2px;
  }}

  /* Style Button Group */
  .style-grid {{
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 10px;
  }}
  .style-option {{
    position: relative;
  }}
  .style-option input[type="radio"] {{
    position: absolute;
    opacity: 0;
    cursor: pointer;
  }}
  .style-card {{
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    padding: 14px 10px;
    background: var(--input-bg);
    border: 2px solid var(--input-border);
    border-radius: 8px;
    cursor: pointer;
    transition: all 0.2s ease;
    text-align: center;
    user-select: none;
  }}
  .style-card:hover {{
    border-color: #64748b;
  }}
  .style-option input[type="radio"]:checked + .style-card {{
    background: var(--radio-active-bg);
    border-color: var(--radio-active-border);
    box-shadow: 0 0 12px rgba(56, 189, 248, 0.25);
  }}
  .style-icon {{
    font-size: 22px;
    margin-bottom: 4px;
  }}
  .style-title {{
    font-size: 14px;
    font-weight: 700;
  }}
  .style-sub {{
    font-size: 11px;
    color: var(--text-secondary);
    margin-top: 2px;
  }}

  /* 2-column layout for numbers */
  .grid-2 {{
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 16px;
  }}

  /* Submit Button */
  .btn-submit {{
    width: 100%;
    padding: 12px;
    background: var(--accent);
    color: #052e16;
    border: none;
    border-radius: 8px;
    font-size: 16px;
    font-weight: 700;
    cursor: pointer;
    transition: background 0.15s, transform 0.05s;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    margin-top: 10px;
  }}
  .btn-submit:hover {{
    background: var(--accent-hover);
  }}
  .btn-submit:active {{
    transform: scale(0.99);
  }}

  /* Links & Preview Box */
  .links-strip {{
    display: flex;
    gap: 12px;
    flex-wrap: wrap;
    margin-top: 16px;
  }}
  .quick-link {{
    flex: 1;
    min-width: 150px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    background: #1e2230;
    color: #38bdf8;
    text-decoration: none;
    font-size: 13px;
    font-weight: 600;
    padding: 10px;
    border-radius: 6px;
    border: 1px solid #2d3345;
    transition: background 0.15s;
  }}
  .quick-link:hover {{
    background: #262c3e;
  }}

  /* Embedded preview */
  .preview-wrapper {{
    background: #000;
    border: 2px solid var(--card-border);
    border-radius: 10px;
    padding: 12px;
    margin-top: 24px;
  }}
  .preview-header {{
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 10px;
  }}
  .preview-title {{
    font-size: 13px;
    font-weight: 700;
    text-transform: uppercase;
    color: var(--text-secondary);
    display: flex;
    align-items: center;
    gap: 8px;
  }}
  .iframe-container {{
    position: relative;
    width: 100%;
    aspect-ratio: 800 / 480;
    max-height: 480px;
    overflow: hidden;
    border-radius: 6px;
    background: #fff;
  }}
  iframe {{
    width: 800px;
    height: 480px;
    border: none;
    transform-origin: 0 0;
  }}

  @keyframes fadeIn {{
    from {{ opacity: 0; transform: translateY(-4px); }}
    to {{ opacity: 1; transform: translateY(0); }}
  }}

  @media (max-width: 640px) {{
    .direction-grid {{ grid-template-columns: 1fr; }}
    .style-grid {{ grid-template-columns: 1fr; }}
    .grid-2 {{ grid-template-columns: 1fr; }}
  }}
</style>
<script>
  function scaleIframe() {{
    const container = document.querySelector('.iframe-container');
    const iframe = document.querySelector('#preview-frame');
    if (container && iframe) {{
      const scale = container.clientWidth / 800;
      iframe.style.transform = 'scale(' + scale + ')';
    }}
  }}
  window.addEventListener('resize', scaleIframe);
  window.addEventListener('load', scaleIframe);

  function reloadPreview() {{
    const iframe = document.querySelector('#preview-frame');
    if (iframe) {{
      iframe.src = iframe.src.split('?')[0] + '?t=' + Date.now();
    }}
  }}

  async function reloadStationsLive() {{
    const btn = document.getElementById('reload-stations-btn');
    const sel = document.getElementById('station-select');
    const currentVal = sel ? sel.value : '';
    if (btn) {{
      btn.innerText = '⏳ Querying Irish Rail...';
      btn.disabled = true;
    }}
    try {{
      const res = await fetch('/api/stations/reload', {{ method: 'POST' }});
      const data = await res.json();
      if (data.stations && sel) {{
        sel.innerHTML = data.stations.map(s => {{
          const isSel = s.name.toLowerCase() === currentVal.toLowerCase() || s.code.toLowerCase() === currentVal.toLowerCase();
          return '<option value="' + s.name + '" ' + (isSel ? 'selected' : '') + '>' + s.name + ' (' + s.code + ')</option>';
        }}).join('');
        if (btn) {{
          btn.innerText = '✓ ' + (data.count || '') + ' Stations Live';
          setTimeout(() => {{ btn.innerText = '🔄 Reload Stations (Irish Rail)'; btn.disabled = false; }}, 3500);
        }}
      }}
    }} catch (err) {{
      window.location.href = '/reload-stations';
    }}
  }}
</script>
</head>
<body>
<div class="container">

  <!-- Header -->
  <header class="brand-header">
    <div class="brand-title">
      <span class="logo-badge">DART</span>
      <div>
        <h1>Departure Board Configuration</h1>
        <p class="subtitle">Raspberry Pi Real-time Station & Direction Manager</p>
      </div>
    </div>
    <div class="status-pill">
      <span class="status-dot"></span>
      Board Port :8000 Active
    </div>
  </header>

  {alert_html}

  <!-- Configuration Form -->
  <main class="config-card">
    <form action="/save" method="post" onsubmit="setTimeout(reloadPreview, 800)">
      
      <!-- Station Selection -->
      <div class="form-group">
        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
          <label for="station-select" style="margin-bottom: 0;">DART Station</label>
          <button type="button" id="reload-stations-btn" onclick="reloadStationsLive()" style="background: none; border: 1px solid var(--card-border); color: #38bdf8; font-size: 12px; font-weight: 600; padding: 4px 10px; border-radius: 6px; cursor: pointer; display: inline-flex; align-items: center; gap: 4px;">
            🔄 Reload Stations (Irish Rail)
          </button>
        </div>
        <select id="station-select" name="station" required>
          {"" .join(station_options)}
        </select>
        <p class="help-text">Live official DART stations ordered North to South. Click 'Reload Stations' to dynamically fetch any newly opened stations from Irish Rail.</p>
      </div>

      <!-- Direction Selection -->
      <div class="form-group">
        <label>Direction Filter</label>
        <div class="direction-grid">
          
          <label class="direction-option">
            <input type="radio" name="direction" value="Northbound" {'checked' if current_dir == 'Northbound' else ''}>
            <div class="direction-card">
              <span class="dir-icon">⬆️</span>
              <span class="dir-title">Northbound</span>
              <span class="dir-sub">To Howth / Malahide</span>
            </div>
          </label>

          <label class="direction-option">
            <input type="radio" name="direction" value="Southbound" {'checked' if current_dir == 'Southbound' else ''}>
            <div class="direction-card">
              <span class="dir-icon">⬇️</span>
              <span class="dir-title">Southbound</span>
              <span class="dir-sub">To Bray / Greystones</span>
            </div>
          </label>

          <label class="direction-option">
            <input type="radio" name="direction" value="Both" {'checked' if current_dir in ('Both', '', None) else ''}>
            <div class="direction-card">
              <span class="dir-icon">🔄</span>
              <span class="dir-title">Both Directions</span>
              <span class="dir-sub">All upcoming trains</span>
            </div>
          </label>

        </div>
        <p class="help-text">Choose whether to display single-direction trains or both platforms.</p>
      </div>

      <!-- Display Style Selection -->
      <div class="form-group">
        <label>Display Aesthetic Style</label>
        <div class="style-grid">
          
          <label class="style-option">
            <input type="radio" name="display_style" value="solari" {'checked' if current_style == 'solari' else ''}>
            <div class="style-card">
              <span class="style-icon">🔲</span>
              <span class="style-title">Solari di Udine</span>
              <span class="style-sub">Mechanical split-flap cards</span>
            </div>
          </label>

          <label class="style-option">
            <input type="radio" name="display_style" value="matrix" {'checked' if current_style in ('matrix', 'modern') else ''}>
            <div class="style-card">
              <span class="style-icon">🟡</span>
              <span class="style-title">Dot Matrix Indicator</span>
              <span class="style-sub">Transit LED platform screen</span>
            </div>
          </label>

          <label class="style-option">
            <input type="radio" name="display_style" value="plain" {'checked' if current_style == 'plain' else ''}>
            <div class="style-card">
              <span class="style-icon">📄</span>
              <span class="style-title">Plain Minimalist</span>
              <span class="style-sub">High-contrast tabular grid</span>
            </div>
          </label>

        </div>
        <p class="help-text">Choose the display layout rendered on both the Waveshare e-paper screen and the live web preview.</p>
      </div>

      <!-- Advanced Display Tuning -->
      <div class="grid-2">
        <div class="form-group">
          <label for="num_mins">Lookahead Window (Minutes)</label>
          <input type="number" id="num_mins" name="num_mins" min="5" max="90" value="{settings.num_mins}">
          <p class="help-text">Irish Rail limit: 5 to 90 min.</p>
        </div>

        <div class="form-group">
          <label for="max_departures">Max Trains to Display</label>
          <input type="number" id="max_departures" name="max_departures" min="1" max="10" value="{settings.max_departures}">
          <p class="help-text">E-Ink displays up to 5 rows.</p>
        </div>
      </div>

      <!-- Save Button -->
      <button type="submit" class="btn-submit">
        <span>💾 Save & Apply to Departure Board</span>
      </button>
    </form>
  </main>

  <!-- Live Board Preview Container -->
  <section class="preview-wrapper">
    <div class="preview-header">
      <div class="preview-title">
        <span>Live Departure Board Preview (Port 8000)</span>
      </div>
      <button type="button" onclick="reloadPreview()" class="quick-link" style="min-width:auto; padding: 4px 10px; font-size: 11px;">
        🔄 Refresh View
      </button>
    </div>
    <div class="iframe-container">
      <iframe id="preview-frame" src="http://{host_no_port}:8000/preview" title="Live DART Preview"></iframe>
    </div>
  </section>

  <!-- Quick Access Links -->
  <div class="links-strip">
    <a href="http://{host_no_port}:8000/preview" target="_blank" class="quick-link">
      <span>🖥️ Full Web Preview</span>
    </a>
    <a href="http://{host_no_port}:8000/terminal" target="_blank" class="quick-link">
      <span>📟 Terminal Board</span>
    </a>
    <a href="http://{host_no_port}:8000/screen.png" target="_blank" class="quick-link">
      <span>🖼️ 1-Bit PNG</span>
    </a>
    <a href="http://{host_no_port}:8000/departures" target="_blank" class="quick-link">
      <span>📊 JSON API</span>
    </a>
  </div>

</div>
</body>
</html>
"""
    return HTMLResponse(content=html)
