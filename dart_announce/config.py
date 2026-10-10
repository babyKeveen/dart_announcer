"""Environment-driven settings for which station/direction to show and how far ahead to look."""

import os
from datetime import datetime
from pathlib import Path
from dataclasses import dataclass

from dotenv import load_dotenv

_ENV_PATH = Path(__file__).resolve().parent.parent / ".env"
_last_env_mtime: float = 0.0

if _ENV_PATH.exists():
    try:
        _last_env_mtime = _ENV_PATH.stat().st_mtime
    except OSError:
        pass
    load_dotenv(_ENV_PATH)
else:
    load_dotenv()

VALID_DIRECTIONS = {"Northbound", "Southbound"}
VALID_STYLES = {"solari", "matrix", "plain", "reverse"}
STYLE_ALIASES = {
    "dotmatrix": "matrix",
    "dot_matrix": "matrix",
    "modern": "matrix",  # migration fallback for deprecated modern style
    "inverted": "reverse",
    "dark": "reverse",
}


class InvalidSettingsError(Exception):
    """Raised when an environment variable holds a value the app can't use."""


@dataclass(frozen=True)
class Settings:
    station: str
    direction: str | None
    num_mins: int
    max_departures: int
    trmnl_webhook_url: str | None = None
    display_style: str = "solari"
    schedule_enabled: bool = True
    schedule_start_hour: int = 6  # 6:00 AM
    schedule_end_hour: int = 19   # 7:00 PM (19:00)

    def is_active_hours(self, now: datetime | None = None) -> bool:
        """Return True if current time is within active commute window, False if in down period."""
        if not self.schedule_enabled:
            return True
        if now is None:
            now = datetime.now()
        hour = now.hour
        if self.schedule_start_hour < self.schedule_end_hour:
            return self.schedule_start_hour <= hour < self.schedule_end_hour
        elif self.schedule_start_hour > self.schedule_end_hour:
            return hour >= self.schedule_start_hour or hour < self.schedule_end_hour
        return True


def load_settings() -> Settings:
    global _last_env_mtime
    if _ENV_PATH.exists():
        try:
            mtime = _ENV_PATH.stat().st_mtime
            if mtime > _last_env_mtime:
                load_dotenv(_ENV_PATH, override=True)
                _last_env_mtime = mtime
        except OSError:
            pass

    station = os.environ.get("STATION", "Sutton").strip()
    if not station:
        raise InvalidSettingsError("STATION must not be empty")

    direction = os.environ.get("DIRECTION", "").strip() or None
    if direction and direction not in VALID_DIRECTIONS:
        raise InvalidSettingsError(f"DIRECTION must be one of {sorted(VALID_DIRECTIONS)}, got {direction!r}")

    try:
        num_mins = int(os.environ.get("NUM_MINS", "90"))
    except ValueError as exc:
        raise InvalidSettingsError("NUM_MINS must be an integer") from exc
    if not 5 <= num_mins <= 90:
        raise InvalidSettingsError("NUM_MINS must be between 5 and 90 (Irish Rail API limit)")

    try:
        max_departures = int(os.environ.get("MAX_DEPARTURES", "5"))
    except ValueError as exc:
        raise InvalidSettingsError("MAX_DEPARTURES must be an integer") from exc
    if max_departures < 1:
        raise InvalidSettingsError("MAX_DEPARTURES must be at least 1")

    raw_style = os.environ.get("DISPLAY_STYLE", "solari").strip().lower() or "solari"
    display_style = STYLE_ALIASES.get(raw_style, raw_style)
    if display_style not in VALID_STYLES:
        raise InvalidSettingsError(f"DISPLAY_STYLE must be one of {sorted(VALID_STYLES)}, got {raw_style!r}")

    trmnl_webhook_url = os.environ.get("TRMNL_WEBHOOK_URL", "").strip() or None

    raw_sched = os.environ.get("SCHEDULE_ENABLED", "true").strip().lower()
    schedule_enabled = raw_sched in ("true", "1", "yes", "t", "y", "on")

    try:
        schedule_start_hour = int(os.environ.get("SCHEDULE_START_HOUR", "6"))
    except ValueError as exc:
        raise InvalidSettingsError("SCHEDULE_START_HOUR must be an integer between 0 and 23") from exc
    if not 0 <= schedule_start_hour <= 23:
        raise InvalidSettingsError("SCHEDULE_START_HOUR must be between 0 and 23")

    try:
        schedule_end_hour = int(os.environ.get("SCHEDULE_END_HOUR", "19"))
    except ValueError as exc:
        raise InvalidSettingsError("SCHEDULE_END_HOUR must be an integer between 0 and 23") from exc
    if not 0 <= schedule_end_hour <= 23:
        raise InvalidSettingsError("SCHEDULE_END_HOUR must be between 0 and 23")

    return Settings(
        station=station,
        direction=direction,
        num_mins=num_mins,
        max_departures=max_departures,
        trmnl_webhook_url=trmnl_webhook_url,
        display_style=display_style,
        schedule_enabled=schedule_enabled,
        schedule_start_hour=schedule_start_hour,
        schedule_end_hour=schedule_end_hour,
    )


def save_settings(
    station: str,
    direction: str | None,
    num_mins: int = 90,
    max_departures: int = 5,
    trmnl_webhook_url: str | None = None,
    display_style: str = "solari",
    schedule_enabled: bool = True,
    schedule_start_hour: int = 6,
    schedule_end_hour: int = 19,
) -> Settings:
    """Validate and persist new configuration to .env and update active environment."""
    global _last_env_mtime

    station_clean = station.strip()
    if not station_clean:
        raise InvalidSettingsError("STATION must not be empty")

    direction_clean = direction.strip() if direction else ""
    if direction_clean and direction_clean not in VALID_DIRECTIONS:
        raise InvalidSettingsError(f"DIRECTION must be one of {sorted(VALID_DIRECTIONS)} or empty/None for Both, got {direction_clean!r}")

    if not 5 <= num_mins <= 90:
        raise InvalidSettingsError("NUM_MINS must be between 5 and 90")

    if max_departures < 1:
        raise InvalidSettingsError("MAX_DEPARTURES must be at least 1")

    raw_style = (display_style or "solari").strip().lower()
    style_clean = STYLE_ALIASES.get(raw_style, raw_style)
    if style_clean not in VALID_STYLES:
        raise InvalidSettingsError(f"DISPLAY_STYLE must be one of {sorted(VALID_STYLES)}, got {raw_style!r}")

    if not 0 <= schedule_start_hour <= 23:
        raise InvalidSettingsError("SCHEDULE_START_HOUR must be between 0 and 23")
    if not 0 <= schedule_end_hour <= 23:
        raise InvalidSettingsError("SCHEDULE_END_HOUR must be between 0 and 23")

    # Format .env lines
    lines = [
        f"STATION={station_clean}",
        f"DIRECTION={direction_clean}",
        f"NUM_MINS={num_mins}",
        f"MAX_DEPARTURES={max_departures}",
        f"DISPLAY_STYLE={style_clean}",
        f"SCHEDULE_ENABLED={'true' if schedule_enabled else 'false'}",
        f"SCHEDULE_START_HOUR={schedule_start_hour}",
        f"SCHEDULE_END_HOUR={schedule_end_hour}",
    ]
    if trmnl_webhook_url:
        lines.append(f"TRMNL_WEBHOOK_URL={trmnl_webhook_url.strip()}")
    elif os.environ.get("TRMNL_WEBHOOK_URL"):
        lines.append(f"TRMNL_WEBHOOK_URL={os.environ['TRMNL_WEBHOOK_URL'].strip()}")

    content = "\n".join(lines) + "\n"
    _ENV_PATH.write_text(content, encoding="utf-8")

    # Update in-memory environment variables
    os.environ["STATION"] = station_clean
    os.environ["DIRECTION"] = direction_clean
    os.environ["NUM_MINS"] = str(num_mins)
    os.environ["MAX_DEPARTURES"] = str(max_departures)
    os.environ["DISPLAY_STYLE"] = style_clean
    os.environ["SCHEDULE_ENABLED"] = "true" if schedule_enabled else "false"
    os.environ["SCHEDULE_START_HOUR"] = str(schedule_start_hour)
    os.environ["SCHEDULE_END_HOUR"] = str(schedule_end_hour)
    if trmnl_webhook_url:
        os.environ["TRMNL_WEBHOOK_URL"] = trmnl_webhook_url.strip()

    try:
        _last_env_mtime = _ENV_PATH.stat().st_mtime
    except OSError:
        pass

    return Settings(
        station=station_clean,
        direction=direction_clean or None,
        num_mins=num_mins,
        max_departures=max_departures,
        trmnl_webhook_url=trmnl_webhook_url or os.environ.get("TRMNL_WEBHOOK_URL"),
        display_style=style_clean,
        schedule_enabled=schedule_enabled,
        schedule_start_hour=schedule_start_hour,
        schedule_end_hour=schedule_end_hour,
    )
