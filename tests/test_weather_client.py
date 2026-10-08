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
