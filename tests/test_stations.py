import pytest

from dart_announce import stations

SAMPLE_STATIONS = [
    {"StationDesc": "Sutton", "StationCode": "SUTTN"},
    {"StationDesc": "Howth", "StationCode": "HOWTH"},
    {"StationDesc": "Dublin Connolly", "StationCode": "CNLLY", "StationAlias": "Connolly"},
]


def test_resolve_by_alias():
    assert stations.resolve_station_code("Connolly", SAMPLE_STATIONS) == "CNLLY"
    assert stations.resolve_station_code("connolly", SAMPLE_STATIONS) == "CNLLY"


def test_resolve_by_name_case_insensitive():
    assert stations.resolve_station_code("sutton", SAMPLE_STATIONS) == "SUTTN"
    assert stations.resolve_station_code("SUTTON", SAMPLE_STATIONS) == "SUTTN"


def test_resolve_by_code():
    assert stations.resolve_station_code("suttn", SAMPLE_STATIONS) == "SUTTN"


def test_resolve_unknown_station_raises():
    with pytest.raises(stations.UnknownStationError):
        stations.resolve_station_code("Nowhereville", SAMPLE_STATIONS)


def test_resolve_empty_station_raises():
    with pytest.raises(stations.UnknownStationError):
        stations.resolve_station_code("   ", SAMPLE_STATIONS)


def test_resolve_station_returns_full_entry():
    entry = stations.resolve_station("sutton", SAMPLE_STATIONS)
    assert entry == {"StationDesc": "Sutton", "StationCode": "SUTTN"}


def test_get_dart_stations_dynamic_and_sort(monkeypatch):
    mock_raw = [
        {"StationDesc": "Bray", "StationCode": "BRAY", "StationLatitude": "53.2043"},
        {"StationDesc": "Malahide", "StationCode": "MHIDE", "StationLatitude": "53.4509"},
        {"StationDesc": "New Future Station", "StationCode": "NEWST", "StationLatitude": "53.3000"},
    ]
    monkeypatch.setattr(stations.irish_rail_client, "fetch_stations_by_type", lambda t: mock_raw)
    try:
        reloaded, is_live = stations.reload_dart_stations()
        assert is_live is True
        assert len(reloaded) == 3
        # Verify sorted North to South by latitude descending
        assert reloaded[0]["name"] == "Malahide"
        assert reloaded[1]["name"] == "New Future Station"
        assert reloaded[2]["name"] == "Bray"
    finally:
        stations._cached_dart_stations = None


def test_reload_dart_stations_fallback_on_error(monkeypatch):
    def _fail(t):
        raise stations.irish_rail_client.IrishRailError("API timeout")

    monkeypatch.setattr(stations.irish_rail_client, "fetch_stations_by_type", _fail)
    try:
        reloaded, is_live = stations.reload_dart_stations()
        assert is_live is False
        assert len(reloaded) == 33
        assert reloaded[0]["name"] == "Malahide"
    finally:
        stations._cached_dart_stations = None
