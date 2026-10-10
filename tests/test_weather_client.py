from datetime import datetime

import httpx
import pytest

from dart_announce import weather_client


class _FakeResponse:
    def __init__(self, json_data):
        self._json_data = json_data

    def raise_for_status(self):
        pass

    def json(self):
        return self._json_data


def test_fetch_rain_chance_returns_current_hour_probability(monkeypatch):
    current_hour = datetime.now().strftime("%Y-%m-%dT%H:00")
    sample = {"hourly": {"time": [current_hour], "precipitation_probability": [72]}}
    monkeypatch.setattr(weather_client.httpx, "get", lambda *a, **k: _FakeResponse(sample))

    assert weather_client.fetch_rain_chance(53.39, -6.11) == 72


def test_fetch_rain_chance_falls_back_to_first_entry_if_hour_missing(monkeypatch):
    sample = {"hourly": {"time": ["2020-01-01T00:00"], "precipitation_probability": [15]}}
    monkeypatch.setattr(weather_client.httpx, "get", lambda *a, **k: _FakeResponse(sample))

    assert weather_client.fetch_rain_chance(53.39, -6.11) == 15


def test_fetch_rain_chance_raises_on_network_error(monkeypatch):
    def _raise(*a, **k):
        raise httpx.ConnectError("boom")

    monkeypatch.setattr(weather_client.httpx, "get", _raise)

    with pytest.raises(weather_client.WeatherError):
        weather_client.fetch_rain_chance(53.39, -6.11)


def test_fetch_rain_chance_raises_on_unexpected_shape(monkeypatch):
    monkeypatch.setattr(weather_client.httpx, "get", lambda *a, **k: _FakeResponse({"unexpected": {}}))

    with pytest.raises(weather_client.WeatherError):
        weather_client.fetch_rain_chance(53.39, -6.11)


def test_fetch_tomorrow_weather_success(monkeypatch):
    sample = {
        "daily": {
            "time": ["2026-10-10", "2026-10-11"],
            "weather_code": [3, 61],
            "temperature_2m_max": [14.0, 15.5],
            "temperature_2m_min": [8.0, 9.2],
            "precipitation_probability_max": [20, 65],
        }
    }
    monkeypatch.setattr(weather_client.httpx, "get", lambda *a, **k: _FakeResponse(sample))

    forecast = weather_client.fetch_tomorrow_weather(53.39, -6.11)
    assert forecast.date_str == "2026-10-11"
    assert forecast.temp_max == 15.5
    assert forecast.temp_min == 9.2
    assert forecast.rain_chance == 65
    assert forecast.weather_code == 61
    assert forecast.condition == "Slight Rain"
    assert forecast.symbol == "🌧️"


def test_fetch_tomorrow_weather_single_day_fallback(monkeypatch):
    sample = {
        "daily": {
            "time": ["2026-10-10"],
            "weather_code": [0],
            "temperature_2m_max": [18.0],
            "temperature_2m_min": [10.0],
            "precipitation_probability_max": [5],
        }
    }
    monkeypatch.setattr(weather_client.httpx, "get", lambda *a, **k: _FakeResponse(sample))

    forecast = weather_client.fetch_tomorrow_weather(53.39, -6.11)
    assert forecast.date_str == "2026-10-10"
    assert forecast.temp_max == 18.0
    assert forecast.temp_min == 10.0
    assert forecast.rain_chance == 5
    assert forecast.weather_code == 0
    assert forecast.condition == "Clear Sky"
    assert forecast.symbol == "☀️"


def test_fetch_tomorrow_weather_raises_on_network_error(monkeypatch):
    def _raise(*a, **k):
        raise httpx.ConnectError("network timeout")

    monkeypatch.setattr(weather_client.httpx, "get", _raise)

    with pytest.raises(weather_client.WeatherError):
        weather_client.fetch_tomorrow_weather(53.39, -6.11)


def test_fetch_tomorrow_weather_raises_on_unexpected_shape(monkeypatch):
    monkeypatch.setattr(weather_client.httpx, "get", lambda *a, **k: _FakeResponse({"daily": {}}))

    with pytest.raises(weather_client.WeatherError):
        weather_client.fetch_tomorrow_weather(53.39, -6.11)

