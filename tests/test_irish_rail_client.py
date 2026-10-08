from pathlib import Path

import httpx
import pytest

from dart_announce import irish_rail_client

FIXTURES = Path(__file__).resolve().parent.parent / "fixtures"


class FakeResponse:
    def __init__(self, text: str):
        self.text = text

    def raise_for_status(self) -> None:
        pass


def test_fetch_station_data_parses_fixture(monkeypatch):
    xml = (FIXTURES / "sutton_sample.xml").read_text(encoding="utf-8")

    def fake_get(url, params=None, timeout=None):
        return FakeResponse(xml)

    monkeypatch.setattr(irish_rail_client.httpx, "get", fake_get)

    result = irish_rail_client.fetch_station_data("SUTTN", num_mins=90)

    assert len(result) == 6
    assert all(entry["Stationcode"] == "SUTTN" for entry in result)
    assert {entry["Direction"] for entry in result} == {"Northbound", "Southbound"}


def test_fetch_station_data_rejects_out_of_range_num_mins():
    with pytest.raises(ValueError):
        irish_rail_client.fetch_station_data("SUTTN", num_mins=200)
    with pytest.raises(ValueError):
        irish_rail_client.fetch_station_data("SUTTN", num_mins=1)


def test_fetch_station_data_wraps_network_errors(monkeypatch):
    def fake_get(url, params=None, timeout=None):
        raise httpx.ConnectError("boom", request=httpx.Request("GET", url))

    monkeypatch.setattr(irish_rail_client.httpx, "get", fake_get)

    with pytest.raises(irish_rail_client.IrishRailError):
        irish_rail_client.fetch_station_data("SUTTN")


def test_fetch_all_stations_parses_fixture(monkeypatch):
    xml = (FIXTURES / "all_stations_sample.xml").read_text(encoding="utf-8")

    def fake_get(url, timeout=None):
        return FakeResponse(xml)

    monkeypatch.setattr(irish_rail_client.httpx, "get", fake_get)

    result = irish_rail_client.fetch_all_stations()

    assert any(s["StationCode"] == "SUTTN" and s["StationDesc"] == "Sutton" for s in result)
