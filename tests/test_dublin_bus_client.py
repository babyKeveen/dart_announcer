from pathlib import Path
try:
    import pytest
except ImportError:
    pytest = None

from dart_announce import dublin_bus_client

FIXTURES = Path(__file__).resolve().parent.parent / "fixtures"


class FakeResponse:
    def __init__(self, text: str):
        self.text = text

    def raise_for_status(self) -> None:
        pass


def test_parse_bus_html_fixture():
    html = (FIXTURES / "stop_917_sample.html").read_text(encoding="utf-8")
    departures = dublin_bus_client.parse_bus_html(html, route_filter="H1", ref_time="06:30")

    assert len(departures) == 12
    first = departures[0]
    assert first.route == "H1"
    assert "Abbey" in first.destination
    assert first.scheduled == "06:37"
    assert first.expected == "06:37"
    assert first.due_in == 7
    assert first.status == "Sched"


def test_parse_bus_html_filters_route():
    html = (FIXTURES / "stop_917_sample.html").read_text(encoding="utf-8")
    departures = dublin_bus_client.parse_bus_html(html, route_filter="NONEXISTENT")
    assert len(departures) == 0


def test_fetch_bus_departures_with_mock(monkeypatch):
    html = (FIXTURES / "stop_917_sample.html").read_text(encoding="utf-8")

    def fake_get(url, headers=None, timeout=None, follow_redirects=True):
        return FakeResponse(html)

    if dublin_bus_client.httpx is not None:
        monkeypatch.setattr(dublin_bus_client.httpx, "get", fake_get)

    departures = dublin_bus_client.parse_bus_html(html, route_filter="H1", ref_time="06:30")
    assert len(departures) > 0
    assert departures[0].route == "H1"
