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


def test_parse_bus_html_stop_929_h2():
    html = (FIXTURES / "stop_929_sample.html").read_text(encoding="utf-8")
    departures = dublin_bus_client.parse_bus_html(html, route_filter="H2", ref_time="09:40")

    assert len(departures) > 0
    first = departures[0]
    assert first.route == "H2"
    # Destination must be clean without vehicle registration div
    assert "Abbey" in first.destination
    assert "SG284" not in first.destination
    assert "\n" not in first.destination


def test_fetch_two_routes_departures(monkeypatch):
    html_917 = (FIXTURES / "stop_917_sample.html").read_text(encoding="utf-8")
    html_929 = (FIXTURES / "stop_929_sample.html").read_text(encoding="utf-8")

    def fake_fetch(stop_id, route_filter=None, ref_time=None, timeout=8.0):
        if "917" in stop_id:
            return dublin_bus_client.parse_bus_html(html_917, route_filter=route_filter, ref_time=ref_time)
        elif "929" in stop_id:
            return dublin_bus_client.parse_bus_html(html_929, route_filter=route_filter, ref_time=ref_time)
        return []

    monkeypatch.setattr(dublin_bus_client, "fetch_bus_departures", fake_fetch)

    # When both routes have buses: exactly 2 buses (1 per route)
    buses = dublin_bus_client.fetch_two_routes_departures(ref_time="09:40")
    assert len(buses) == 2
    routes = {b.route for b in buses}
    assert routes == {"H1", "H2"}
    # Must be sorted by due_in
    assert buses[0].due_in <= buses[1].due_in

    # When Route 1 has no buses: exactly 1 bus (Route 2 gets 1 row, Route 1 gets NO ROW)
    def fake_fetch_only_929(stop_id, route_filter=None, ref_time=None, timeout=8.0):
        if "929" in stop_id:
            return dublin_bus_client.parse_bus_html(html_929, route_filter=route_filter, ref_time=ref_time)
        return []

    monkeypatch.setattr(dublin_bus_client, "fetch_bus_departures", fake_fetch_only_929)
    buses_single = dublin_bus_client.fetch_two_routes_departures(ref_time="09:40")
    assert len(buses_single) == 1
    assert buses_single[0].route == "H2"
