# DART Announce

Fetches live Dublin Area Rapid Transit (DART) departure times from
[Irish Rail's public real-time API](http://api.irishrail.ie/realtime/) for a
configured station, and serves them as JSON. Built as the software backbone
for a future TRMNL (OG, 800x480 e-ink) kitchen display — this repo doesn't
talk to TRMNL yet, it just produces the data and a visual preview.

## Setup

```
pip install -r requirements.txt
copy .env.example .env
```

Edit `.env`:

- `STATION` — station name (`Sutton`) or code (`SUTTN`)
- `DIRECTION` — `Northbound`, `Southbound`, or blank for both
- `NUM_MINS` — how far ahead to look, 5-90 (Irish Rail API limit)
- `MAX_DEPARTURES` — max number of departures to return
- `DISPLAY_STYLE` — `solari` (mechanical split-flap), `matrix` (Dot Matrix Indicator), or `plain` (minimalist table)

## Run

```
uvicorn dart_announce.app:app --reload
```

- `GET /departures` — Combined JSON feed containing root fields and `merge_variables`.
- `GET /trmnl` — Dedicated JSON feed matching TRMNL's `{"merge_variables": ...}` schema for Polling Strategy plugins.
- `POST /trmnl/post` — Pushes current departures directly to your TRMNL Custom Plugin Webhook URL (reads `TRMNL_WEBHOOK_URL` from `.env` or `?webhook_url=...` query param).
- `GET /terminal` — ASCII/Unicode departure board formatted directly for terminal / curl display (e.g. `curl http://localhost:8000/terminal`).
- `GET /preview` — 800x480 high-contrast layout rendered in the selected display style (`?style=solari|matrix|plain`, `?invert=true` for light/dark mode).
- `GET /screen.png` — 800x480 1-bit monochrome PNG image for e-paper displays (`?style=...`, `?battery=...`).
- `GET /screen.bmp` — 800x480 1-bit monochrome Windows BMP image (`?style=...`, `?battery=...`).
- `GET /screen.bin` — Exact 48,000-byte 1-bit monochrome raw framebuffer for Waveshare 7.5" e-paper displays.
- `GET /health` — Liveness check.

## Web Configuration Dashboard (Port 8080)

A dedicated, mobile-friendly web frontend runs on port **8080** allowing anyone on your local Wi-Fi to easily adjust the display settings:

- **URL**: [http://raspi2modelb2014.local:8080](http://raspi2modelb2014.local:8080) (or `http://<ip>:8080`)
- **Run locally**:
  ```bash
  uvicorn dart_announce.web_config:app --host 0.0.0.0 --port 8080
  ```
- **Features**:
  - **Station Selector**: Choose any of the 33 coastal DART stations (Malahide to Greystones) or dynamically reload official stations from Irish Rail.
  - **Direction Filter**: Select **Northbound**, **Southbound**, or **Both Directions** (all upcoming trains interleaved).
  - **Display Style Selector**:
    - 🔲 **Solari di Udine**: Classic mechanical split-flap cards, flip seam lines, Bebas Neue & Oswald typography.
    - 🟡 **Dot Matrix Indicator**: Authentic railway LED platform screen, industrial chassis, Doto dot matrix typography.
    - 📄 **Plain Minimalist**: Clean, distraction-free high-contrast tabular departure grid.
  - **Live Apply**: Updates `.env` and propagates immediately to the e-ink buffer (`/screen.bin`), PNG (`/screen.png`), and web preview (`/preview`) on port 8000 with zero downtime.
  - **Embedded Live Preview**: Displays a live preview of the board that reloads as soon as settings are saved.

## Command-Line Display (CLI)

You can view the live departure board directly in your terminal without needing hardware:

```bash
# One-time view
python -m dart_announce.cli

# Auto-refreshing watch mode (e.g. refresh every 30 seconds)
python -m dart_announce.cli --watch 30

# Override station or direction on the fly
python -m dart_announce.cli --station Connolly --direction Southbound

# Plain ASCII box borders (for simple terminals)
python -m dart_announce.cli --ascii
```

Or curl the running server from any machine on your network:
```bash
curl http://<server-ip>:8000/terminal
```

### Using with TRMNL E-Ink Display

You can post data to your TRMNL panel using either strategy:

1. **Webhook Strategy (Push)**:
   - Create a **Custom Plugin** in the TRMNL dashboard and choose **Webhook**.
   - Copy the webhook URL into `.env`:
     ```
     TRMNL_WEBHOOK_URL=https://usetrmnl.com/api/custom_plugins/<YOUR_UUID>
     ```
   - Copy the contents of [`trmnl/template.liquid`](trmnl/template.liquid) into your TRMNL custom plugin markup editor.
   - Trigger a push anytime by making an HTTP POST request to `/trmnl/post`, or schedule it with a cron job on your Pi:
     ```bash
     curl -X POST http://localhost:8000/trmnl/post
     ```

2. **Polling Strategy (Pull)**:
   - Choose **Polling** in your TRMNL custom plugin settings.
   - Point the polling URL to `http://<your-pi-ip>:8000/trmnl` (or via Cloudflare Tunnel / ngrok).
   - Paste [`trmnl/template.liquid`](trmnl/template.liquid) into your plugin markup editor.

## Test

```
pytest
# or on Windows:
python -m pytest
```

Tests run against saved XML fixtures in `fixtures/` (captured from the real
API) — no network access needed.

## Exposing it publicly (before the TRMNL device arrives)

TRMNL private plugins poll a URL you host. To test that end-to-end today,
run the server locally and put a tunnel in front of it:

```
cloudflared tunnel --url http://localhost:8000
# or
ngrok http 8000
```

Then `https://<your-tunnel>.trycloudflare.com/departures` (or the ngrok URL)
is reachable from anywhere — useful for testing, even before a TRMNL plugin
is configured to poll it.

## Running in Docker

```
docker compose build
docker compose up -d
```

This builds a standalone image (`Dockerfile`) and runs it via
`docker-compose.yml` as its own independent stack — no dependency on any
other containers or networks. It reads config from `.env` (same variables as
above) via `env_file`, publishes port 8000, and has a healthcheck against
`/health`.

### Deploying to Raspberry Pi (systemd service)

For resource-constrained devices like the Raspberry Pi 2 Model B (ARMv7, 1GB RAM), running via a native Python 3.13 virtual environment with a systemd user service avoids Docker overhead and uses ~45MB RAM:

1. Copy the repo to the Pi:
   ```bash
   scp -r . kevin@raspi2ModelB2014:~/dart_announce
   ```
2. On the Pi, create the venv and install dependencies:
   ```bash
   cd ~/dart_announce
   python3 -m venv venv
   ./venv/bin/pip install -r requirements.txt
   ```
3. Enable lingering so the service runs in the background without needing an active SSH session:
   ```bash
   loginctl enable-linger kevin
   ```
4. Create the systemd user service (`~/.config/systemd/user/dart-announce.service`):
   ```ini
   [Unit]
   Description=DART Announce FastAPI Service
   After=network-online.target
   Wants=network-online.target

   [Service]
   Type=simple
   WorkingDirectory=/home/kevin/dart_announce
   Environment=TZ=Europe/Dublin
   EnvironmentFile=/home/kevin/dart_announce/.env
   ExecStart=/home/kevin/dart_announce/venv/bin/uvicorn dart_announce.app:app --host 0.0.0.0 --port 8000
   Restart=always
   RestartSec=5

   [Install]
   WantedBy=default.target
   ```
5. Enable and start:
   ```bash
   systemctl --user daemon-reload
   systemctl --user enable --now dart-announce.service
   ```
6. Check status and logs:
   ```bash
   systemctl --user status dart-announce.service
   journalctl --user -u dart-announce.service -f
   ```

## Notes

- Irish Rail offers this API "as is" with no support or SLA — the client
  wraps failures in `IrishRailError` rather than crashing the service.
- Station name -> code resolution uses Irish Rail's own station list
  (`getAllStationsXML`), cached in memory for the life of the process.
- A tunnel (Cloudflare Tunnel/ngrok) is only needed for access from outside
  the LAN — e.g. TRMNL's cloud service polling this, or checking it from a
  phone off the home WiFi. Devices on the same network can reach the
  container directly via the host's LAN IP/hostname and published port.
- Not yet done: the actual TRMNL private plugin (Liquid template) and
  permanent hosting — deferred until the device arrives.
