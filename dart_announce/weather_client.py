"""Thin client for Open-Meteo's free weather API. No API key is required."""

from datetime import datetime

import httpx

BASE_URL = "https://api.open-meteo.com/v1/forecast"


class WeatherError(Exception):
    """Raised when the weather API can't be reached or returns unusable data."""


def fetch_rain_chance(latitude: float, longitude: float, timeout: float = 10.0) -> int:
    """Return the chance of rain (0-100) for the current hour at the given coordinates."""
    params = {
        "latitude": latitude,
        "longitude": longitude,
        "hourly": "precipitation_probability",
        "forecast_days": 1,
        "timezone": "auto",
    }
    try:
        response = httpx.get(BASE_URL, params=params, timeout=timeout)
        response.raise_for_status()
    except httpx.HTTPError as exc:
        raise WeatherError(f"Failed to fetch weather: {exc}") from exc

    try:
        data = response.json()
        times = data["hourly"]["time"]
        probabilities = data["hourly"]["precipitation_probability"]
    except (ValueError, KeyError) as exc:
        raise WeatherError(f"Failed to parse weather response: {exc}") from exc

    current_hour = datetime.now().strftime("%Y-%m-%dT%H:00")
    if current_hour in times:
        return probabilities[times.index(current_hour)]
    return probabilities[0] if probabilities else 0
