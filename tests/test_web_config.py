import pytest
from fastapi.testclient import TestClient

from dart_announce.web_config import app
from dart_announce import config

client = TestClient(app)


def test_get_config_api(monkeypatch, tmp_path):
    env_file = tmp_path / ".env"
    env_file.write_text("STATION=Sutton\nDIRECTION=Southbound\nNUM_MINS=90\nMAX_DEPARTURES=5\nDISPLAY_STYLE=solari\n")
    monkeypatch.setattr(config, "_ENV_PATH", env_file)

    response = client.get("/api/config")
    assert response.status_code == 200
    data = response.json()
    assert data["station"] == "Sutton"
    assert data["direction"] == "Southbound"
    assert data["display_style"] == "solari"
    assert len(data["stations"]) == 33
    assert data["stations"][0]["name"] == "Malahide"


def test_update_config_api_both_directions(monkeypatch, tmp_path):
    env_file = tmp_path / ".env"
    env_file.write_text("STATION=Sutton\nDIRECTION=Southbound\n")
    monkeypatch.setattr(config, "_ENV_PATH", env_file)

    response = client.post(
        "/api/config",
        json={
            "station": "Howth",
            "direction": "Both",
            "display_style": "matrix",
            "num_mins": 60,
            "max_departures": 8,
        },
    )
    assert response.status_code == 200
    data = response.json()
    assert data["status"] == "success"
    assert data["settings"]["station"] == "Howth"
    assert data["settings"]["direction"] == "Both"
    assert data["settings"]["display_style"] == "matrix"

    # Verify underlying config loaded
    loaded = config.load_settings()
    assert loaded.station == "Howth"
    assert loaded.direction is None
    assert loaded.display_style == "matrix"
    assert loaded.num_mins == 60
    assert loaded.max_departures == 8


def test_update_config_api_single_direction(monkeypatch, tmp_path):
    env_file = tmp_path / ".env"
    env_file.write_text("STATION=Sutton\nDIRECTION=\n")
    monkeypatch.setattr(config, "_ENV_PATH", env_file)

    response = client.post(
        "/api/config",
        json={
            "station": "Tara Street",
            "direction": "Northbound",
            "num_mins": 45,
            "max_departures": 5,
        },
    )
    assert response.status_code == 200
    data = response.json()
    assert data["settings"]["direction"] == "Northbound"

    loaded = config.load_settings()
    assert loaded.station == "Tara Street"
    assert loaded.direction == "Northbound"


def test_save_form_redirect(monkeypatch, tmp_path):
    env_file = tmp_path / ".env"
    env_file.write_text("STATION=Sutton\nDIRECTION=Southbound\n")
    monkeypatch.setattr(config, "_ENV_PATH", env_file)

    response = client.post(
        "/save",
        data={
            "station": "Bray",
            "direction": "Southbound",
            "display_style": "plain",
            "num_mins": 90,
            "max_departures": 5,
        },
        follow_redirects=False,
    )
    assert response.status_code == 303
    assert response.headers["location"] == "/?saved=1"

    loaded = config.load_settings()
    assert loaded.station == "Bray"
    assert loaded.direction == "Southbound"
    assert loaded.display_style == "plain"

    # Test reverse style saving
    response_rev = client.post(
        "/save",
        data={
            "station": "Bray",
            "direction": "Southbound",
            "display_style": "reverse",
            "num_mins": 90,
            "max_departures": 5,
        },
        follow_redirects=False,
    )
    assert response_rev.status_code == 303
    loaded_rev = config.load_settings()
    assert loaded_rev.display_style == "reverse"


def test_index_page_render(monkeypatch, tmp_path):
    env_file = tmp_path / ".env"
    env_file.write_text("STATION=Sutton\nDIRECTION=Southbound\nDISPLAY_STYLE=solari\n")
    monkeypatch.setattr(config, "_ENV_PATH", env_file)

    response = client.get("/")
    assert response.status_code == 200
    assert "text/html" in response.headers["content-type"]
    assert "Departure Board Configuration" in response.text
    assert "Northbound" in response.text
    assert "Southbound" in response.text
    assert "Both Directions" in response.text
    assert "Solari di Udine" in response.text
    assert "Dot Matrix Indicator" in response.text
    assert "Plain Minimalist" in response.text
    assert "Reverse Contrast" in response.text
    assert "Ultra-Modern" not in response.text
    assert "Sutton (SUTTN)" in response.text
    assert "Live Departure Board Preview (Port 8000)" in response.text


def test_reload_stations_api(monkeypatch):
    mock_raw = [
        {"StationDesc": "Howth", "StationCode": "HOWTH", "StationLatitude": "53.388"},
        {"StationDesc": "Sutton", "StationCode": "SUTTN", "StationLatitude": "53.392"},
    ]
    from dart_announce import irish_rail_client, stations
    monkeypatch.setattr(irish_rail_client, "fetch_stations_by_type", lambda t: mock_raw)

    try:
        response = client.post("/api/stations/reload")
        assert response.status_code == 200
        data = response.json()
        assert data["status"] == "success"
        assert data["live"] is True
        assert data["count"] == 2
        assert data["stations"][0]["name"] == "Sutton"  # Higher latitude first
        assert data["stations"][1]["name"] == "Howth"
    finally:
        stations._cached_dart_stations = None


def test_reload_stations_get_redirect(monkeypatch):
    mock_raw = [
        {"StationDesc": "Sutton", "StationCode": "SUTTN", "StationLatitude": "53.392"},
    ]
    from dart_announce import irish_rail_client, stations
    monkeypatch.setattr(irish_rail_client, "fetch_stations_by_type", lambda t: mock_raw)

    try:
        response = client.get("/reload-stations", follow_redirects=False)
        assert response.status_code == 303
        assert response.headers["location"] == "/?reloaded=1"
    finally:
        stations._cached_dart_stations = None
