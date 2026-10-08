"""Combine config, station resolution, and the Irish Rail client into a display-ready result."""

from dataclasses import dataclass

from . import irish_rail_client, stations, weather_client
from .config import Settings
from .models import Departure


@dataclass
class DeparturesResult:
    station_name: str
    station_code: str
    departures: list[Departure]
    rain_chance: int | None = None


def get_departures(settings: Settings) -> DeparturesResult:
    station = stations.resolve_station(settings.station)
    station_code = station["StationCode"].strip()

    raw_departures = irish_rail_client.fetch_station_data(station_code, num_mins=settings.num_mins)

    station_name = raw_departures[0]["Stationfullname"] if raw_departures else station.get("StationDesc", settings.station)

    departures = [Departure.from_raw(item) for item in raw_departures]
    if settings.direction:
        departures = [d for d in departures if d.direction == settings.direction]
    departures.sort(key=lambda d: d.due_in)
    departures = departures[: settings.max_departures]

    return DeparturesResult(
        station_name=station_name,
        station_code=station_code,
        departures=departures,
        rain_chance=_get_rain_chance(station),
    )


def _get_rain_chance(station: dict) -> int | None:
    """Best-effort rain forecast; weather is a bonus, so failures here degrade to None."""
    try:
        latitude = float(station["StationLatitude"])
        longitude = float(station["StationLongitude"])
        return weather_client.fetch_rain_chance(latitude, longitude)
    except (weather_client.WeatherError, KeyError, TypeError, ValueError):
        return None


class TRMNLPostError(Exception):
    """Raised when posting to a TRMNL webhook fails."""


def build_trmnl_payload(result: DeparturesResult, direction: str | None = None) -> dict:
    """Build a payload conforming to TRMNL's merge_variables specification."""
    from datetime import datetime

    rain = result.rain_chance
    weather_symbol = "☔" if (rain is not None and rain >= 40) else ("☀" if rain is not None else "")
    weather_text = f"{rain}% rain" if rain is not None else ""

    return {
        "merge_variables": {
            "station": result.station_name,
            "station_code": result.station_code,
            "direction": direction or "",
            "updated_at": datetime.now().strftime("%H:%M"),
            "rain_chance_percent": rain,
            "weather_symbol": weather_symbol,
            "weather_text": weather_text,
            "has_departures": len(result.departures) > 0,
            "departure_count": len(result.departures),
            "departures": [d.to_dict() for d in result.departures],
        }
    }


def post_to_trmnl(webhook_url: str, payload: dict, timeout: float = 10.0) -> dict:
    """Post payload to TRMNL's custom plugin webhook URL."""
    import httpx

    try:
        response = httpx.post(webhook_url, json=payload, timeout=timeout)
        response.raise_for_status()
        try:
            return response.json()
        except Exception:
            return {"status": "ok", "status_code": response.status_code}
    except Exception as exc:
        raise TRMNLPostError(f"Failed to post to TRMNL webhook: {exc}") from exc
