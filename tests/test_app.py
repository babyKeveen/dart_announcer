from fastapi.testclient import TestClient
from dart_announce.app import app
from dart_announce.models import Departure
from dart_announce.service import DeparturesResult, TRMNLPostError
from dart_announce.irish_rail_client import IrishRailError
from dart_announce.stations import UnknownStationError

client = TestClient(app)


def _sample_result():
    return DeparturesResult(
        station_name="Sutton",
        station_code="SUTTN",
        departures=[
            Departure(
                train_code="E948",
                origin="Greystones",
                destination="Howth",
                due_in=0,
                scheduled="22:34",
                expected="22:36",
                status="En Route",
                late=2,
                direction="Northbound",
                train_type="DART",
            )
        ],
        rain_chance=15,
    )


def test_health_endpoint():
    response = client.get("/health")
    assert response.status_code == 200
    assert response.json() == {"status": "ok"}


def test_departures_endpoint(monkeypatch):
    monkeypatch.setattr("dart_announce.app.get_departures", lambda settings: _sample_result())

    response = client.get("/departures")
    assert response.status_code == 200
    data = response.json()
    assert data["station"] == "Sutton"
    assert data["rain_chance_percent"] == 15
    assert len(data["departures"]) == 1
    assert data["departures"][0]["destination"] == "Howth"
    assert data["departures"][0]["due_in_text"] == "Due"
    assert "merge_variables" in data
    assert data["merge_variables"]["station"] == "Sutton"
    assert data["merge_variables"]["departures"][0]["due_in_text"] == "Due"


def test_trmnl_endpoint(monkeypatch):
    monkeypatch.setattr("dart_announce.app.get_departures", lambda settings: _sample_result())

    response = client.get("/trmnl")
    assert response.status_code == 200
    data = response.json()
    assert "merge_variables" in data
    assert data["merge_variables"]["station"] == "Sutton"
    assert data["merge_variables"]["weather_symbol"] == "☀"


def test_preview_endpoint(monkeypatch):
    monkeypatch.setenv("DISPLAY_STYLE", "solari")
    mock_result = DeparturesResult(
        station_name="Sutton",
        station_code="SUTTN",
        departures=[],
        rain_chance=50,
    )
    monkeypatch.setattr("dart_announce.app.get_departures", lambda settings: mock_result)

    # Default solari
    response = client.get("/preview")
    assert response.status_code == 200
    assert "text/html" in response.headers["content-type"]
    assert "DART &mdash; Sutton" in response.text
    assert "No departures found" in response.text
    assert "SOLARI SPLIT-FLAP TRANSIT DISPLAY" in response.text
    assert "background: #fff" in response.text

    # Test inverted view
    response_inv = client.get("/preview?invert=true")
    assert response_inv.status_code == 200
    assert "background: #000" in response_inv.text

    # Test dot matrix style
    response_matrix = client.get("/preview?style=matrix")
    assert response_matrix.status_code == 200
    assert "DOT MATRIX PLATFORM INDICATOR" in response_matrix.text
    assert "matrix-header" in response_matrix.text

    # Test deprecated modern alias maps to matrix
    response_modern = client.get("/preview?style=modern")
    assert response_modern.status_code == 200
    assert "DOT MATRIX PLATFORM INDICATOR" in response_modern.text

    # Test plain style
    response_plain = client.get("/preview?style=plain")
    assert response_plain.status_code == 200
    assert "plain-header" in response_plain.text
    assert "plain-footer" in response_plain.text


def test_terminal_endpoint(monkeypatch):
    monkeypatch.setattr("dart_announce.app.get_departures", lambda settings: _sample_result())

    response = client.get("/terminal")
    assert response.status_code == 200
    assert "text/plain" in response.headers["content-type"]
    assert "DART" in response.text
    assert "SUTTON" in response.text
    assert "Howth" in response.text

    # Test ASCII mode
    response_ascii = client.get("/terminal?ascii=true&plain=true")
    assert response_ascii.status_code == 200
    assert "+" in response_ascii.text


def test_screen_png_endpoint(monkeypatch):
    monkeypatch.setattr("dart_announce.app.get_departures", lambda settings: _sample_result())
    response = client.get("/screen.png")
    assert response.status_code == 200
    assert response.headers["content-type"] == "image/png"
    assert len(response.content) > 100

    response_mat = client.get("/screen.png?style=matrix")
    assert response_mat.status_code == 200
    assert response_mat.headers["content-type"] == "image/png"

    response_plain = client.get("/screen.png?style=plain")
    assert response_plain.status_code == 200
    assert response_plain.headers["content-type"] == "image/png"


def test_screen_bmp_endpoint(monkeypatch):
    monkeypatch.setattr("dart_announce.app.get_departures", lambda settings: _sample_result())
    response = client.get("/screen.bmp")
    assert response.status_code == 200
    assert response.headers["content-type"] == "image/bmp"
    assert len(response.content) > 100


def test_screen_bin_endpoint(monkeypatch):
    monkeypatch.setattr("dart_announce.app.get_departures", lambda settings: _sample_result())
    response = client.get("/screen.bin")
    assert response.status_code == 200
    assert response.headers["content-type"] == "application/octet-stream"
    assert len(response.content) == 48000

    response_mat = client.get("/screen.bin?style=matrix")
    assert response_mat.status_code == 200
    assert len(response_mat.content) == 48000

    response_plain = client.get("/screen.bin?style=plain")
    assert response_plain.status_code == 200
    assert len(response_plain.content) == 48000


def test_fonts_endpoint():
    response = client.get("/fonts/BebasNeue.ttf")
    assert response.status_code == 200
    assert response.headers["content-type"] == "font/ttf"
    assert len(response.content) > 1000

    response_doto = client.get("/fonts/Doto.ttf")
    assert response_doto.status_code == 200
    assert response_doto.headers["content-type"] == "font/ttf"
    assert len(response_doto.content) > 1000

    response_missing = client.get("/fonts/nonexistent.ttf")
    assert response_missing.status_code == 404


def test_trmnl_post_success(monkeypatch):
    monkeypatch.setattr("dart_announce.app.get_departures", lambda settings: _sample_result())
    monkeypatch.setattr("dart_announce.app.post_to_trmnl", lambda url, payload: {"status": "success"})

    response = client.post("/trmnl/post?webhook_url=https://usetrmnl.com/api/custom_plugins/fake-id")
    assert response.status_code == 200
    assert response.json()["status"] == "success"


def test_trmnl_post_no_url(monkeypatch):
    monkeypatch.setattr("dart_announce.app.get_departures", lambda settings: _sample_result())
    monkeypatch.delenv("TRMNL_WEBHOOK_URL", raising=False)

    response = client.post("/trmnl/post")
    assert response.status_code == 400


def test_trmnl_post_failure(monkeypatch):
    monkeypatch.setattr("dart_announce.app.get_departures", lambda settings: _sample_result())

    def _fail(url, payload):
        raise TRMNLPostError("Webhook refused")

    monkeypatch.setattr("dart_announce.app.post_to_trmnl", _fail)

    response = client.post("/trmnl/post?webhook_url=https://usetrmnl.com/api/custom_plugins/fake-id")
    assert response.status_code == 502


def test_irish_rail_error_handler(monkeypatch):
    def _raise(settings):
        raise IrishRailError("API timeout")

    monkeypatch.setattr("dart_announce.app.get_departures", _raise)
    response = client.get("/departures")
    assert response.status_code == 502
    assert "API timeout" in response.json()["error"]


def test_unknown_station_error_handler(monkeypatch):
    def _raise(settings):
        raise UnknownStationError("No station")

    monkeypatch.setattr("dart_announce.app.get_departures", _raise)
    response = client.get("/departures")
    assert response.status_code == 400
    assert "No station" in response.json()["error"]
