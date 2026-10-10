"""Thin client for Open-Meteo's free weather API. No API key is required."""

from dataclasses import dataclass
from datetime import datetime

import httpx

BASE_URL = "https://api.open-meteo.com/v1/forecast"


class WeatherError(Exception):
    """Raised when the weather API can't be reached or returns unusable data."""


WMO_WEATHER_MAP: dict[int, tuple[str, str]] = {
    0: ("Clear Sky", "☀️"),
    1: ("Mainly Clear", "🌤️"),
    2: ("Partly Cloudy", "⛅"),
    3: ("Overcast", "☁️"),
    45: ("Foggy", "🌫️"),
    48: ("Depositing Rime Fog", "🌫️"),
    51: ("Light Drizzle", "🌦️"),
    53: ("Moderate Drizzle", "🌦️"),
    55: ("Dense Drizzle", "🌧️"),
    56: ("Light Freezing Drizzle", "🌨️"),
    57: ("Dense Freezing Drizzle", "🌨️"),
    61: ("Slight Rain", "🌧️"),
    63: ("Moderate Rain", "🌧️"),
    65: ("Heavy Rain", "🌧️"),
    66: ("Light Freezing Rain", "🌨️"),
    67: ("Heavy Freezing Rain", "🌨️"),
    71: ("Slight Snow", "❄️"),
    73: ("Moderate Snow", "❄️"),
    75: ("Heavy Snow", "❄️"),
    77: ("Snow Grains", "❄️"),
    80: ("Slight Rain Showers", "🌦️"),
    81: ("Moderate Rain Showers", "🌧️"),
    82: ("Violent Rain Showers", "⛈️"),
    85: ("Slight Snow Showers", "🌨️"),
    86: ("Heavy Snow Showers", "❄️"),
    95: ("Thunderstorm", "⛈️"),
    96: ("Thunderstorm with Hail", "⛈️"),
    99: ("Heavy Thunderstorm with Hail", "⛈️"),
}


@dataclass(frozen=True)
class DailyForecast:
    date_str: str                 # e.g. "2026-10-11"
    temp_max: float               # e.g. 15.3 (°C)
    temp_min: float               # e.g. 8.7 (°C)
    rain_chance: int              # e.g. 50 (%)
    weather_code: int             # e.g. 63
    condition: str                # e.g. "Moderate Rain"
    symbol: str                   # e.g. "🌧️"


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


def fetch_tomorrow_weather(latitude: float, longitude: float, timeout: float = 10.0) -> DailyForecast:
    """Fetch tomorrow's daily weather forecast from Open-Meteo."""
    params = {
        "latitude": latitude,
        "longitude": longitude,
        "daily": [
            "weather_code",
            "temperature_2m_max",
            "temperature_2m_min",
            "precipitation_probability_max",
        ],
        "forecast_days": 2,
        "timezone": "auto",
    }
    try:
        response = httpx.get(BASE_URL, params=params, timeout=timeout)
        response.raise_for_status()
    except httpx.HTTPError as exc:
        raise WeatherError(f"Failed to fetch tomorrow's weather: {exc}") from exc

    try:
        data = response.json()
        daily = data["daily"]
        idx = 1 if len(daily["time"]) > 1 else 0
        date_str = str(daily["time"][idx])
        code = int(daily["weather_code"][idx])
        t_max = float(daily["temperature_2m_max"][idx])
        t_min = float(daily["temperature_2m_min"][idx])
        rain = int(daily["precipitation_probability_max"][idx])
    except (ValueError, KeyError, IndexError) as exc:
        raise WeatherError(f"Failed to parse daily weather response: {exc}") from exc

    condition, symbol = WMO_WEATHER_MAP.get(code, ("Fair", "⛅"))
    return DailyForecast(
        date_str=date_str,
        temp_max=t_max,
        temp_min=t_min,
        rain_chance=rain,
        weather_code=code,
        condition=condition,
        symbol=symbol,
    )
