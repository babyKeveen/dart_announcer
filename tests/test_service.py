from dart_announce import irish_rail_client, stations, weather_client
from dart_announce.config import Settings
from dart_announce.service import get_departures

SAMPLE_STATION = {"StationCode": "SUTTN", "StationLatitude": "53.3925", "StationLongitude": "-6.1082"}

SAMPLE_RAW_DEPARTURES = [
    {
        "Stationfullname": "Sutton", "Traincode": "E948 ", "Origin": "Greystones",
        "Destination": "Howth", "Status": "En Route", "Duein": "8", "Late": "2",
        "Scharrival": "22:34", "Schdepart": "22:34", "Exparrival": "22:36",
        "Expdepart": "22:36", "Direction": "Northbound", "Traintype": "DART",
    },
    {
        "Stationfullname": "Sutton", "Traincode": "E263 ", "Origin": "Howth",
        "Destination": "Bray", "Status": "No Information", "Duein": "21", "Late": "0",
        "Scharrival": "22:49", "Schdepart": "22:49", "Exparrival": "22:49",
        "Expdepart": "22:49", "Direction": "Southbound", "Traintype": "DART",
    },
    {
        "Stationfullname": "Sutton", "Traincode": "E950 ", "Origin": "Greystones",
        "Destination": "Howth", "Status": "En Route", "Duein": "72", "Late": "0",
        "Scharrival": "23:39", "Schdepart": "23:40", "Exparrival": "23:40",
        "Expdepart": "23:40", "Direction": "Northbound", "Traintype": "DART",
    },
]


def _patch(monkeypatch, raw=SAMPLE_RAW_DEPARTURES, rain_chance=40):
    monkeypatch.setattr(stations, "resolve_station", lambda station: SAMPLE_STATION)
    monkeypatch.setattr(irish_rail_client, "fetch_station_data", lambda code, num_mins=90: raw)
    monkeypatch.setattr(weather_client, "fetch_rain_chance", lambda lat, lon: rain_chance)


def test_get_departures_filters_by_direction(monkeypatch):
    _patch(monkeypatch)
    settings = Settings(station="Sutton", direction="Northbound", num_mins=90, max_departures=5)

    result = get_departures(settings)

    assert result.station_name == "Sutton"
    assert result.station_code == "SUTTN"
    assert [d.due_in for d in result.departures] == [8, 72]
    assert all(d.direction == "Northbound" for d in result.departures)
    assert result.rain_chance == 40


def test_get_departures_sorts_by_due_in_and_limits(monkeypatch):
    _patch(monkeypatch)
    settings = Settings(station="Sutton", direction=None, num_mins=90, max_departures=2)

    result = get_departures(settings)

    assert [d.due_in for d in result.departures] == [8, 21]


def test_get_departures_handles_no_upcoming_trains(monkeypatch):
    _patch(monkeypatch, raw=[])
    settings = Settings(station="Sutton", direction=None, num_mins=90, max_departures=5)

    result = get_departures(settings)

    assert result.departures == []
    assert result.station_name == "Sutton"


def test_get_departures_handles_weather_failure(monkeypatch):
    _patch(monkeypatch)

    def _raise(lat, lon):
        raise weather_client.WeatherError("unreachable")

    monkeypatch.setattr(weather_client, "fetch_rain_chance", _raise)
    settings = Settings(station="Sutton", direction=None, num_mins=90, max_departures=5)

    result = get_departures(settings)

    assert result.rain_chance is None
    assert result.departures


def test_get_departures_includes_tomorrow_weather(monkeypatch):
    _patch(monkeypatch)
    mock_forecast = weather_client.DailyForecast(
        date_str="2026-10-11",
        temp_max=16.0,
        temp_min=9.0,
        rain_chance=30,
        weather_code=2,
        condition="Partly Cloudy",
        symbol="⛅",
    )
    monkeypatch.setattr(weather_client, "fetch_tomorrow_weather", lambda lat, lon: mock_forecast)

    settings = Settings(station="Sutton", direction=None, num_mins=90, max_departures=5)
    result = get_departures(settings)

    assert result.tomorrow_weather is not None
    assert result.tomorrow_weather.temp_max == 16.0
    assert result.tomorrow_weather.condition == "Partly Cloudy"


def test_get_departures_handles_tomorrow_weather_failure(monkeypatch):
    _patch(monkeypatch)

    def _raise(lat, lon):
        raise weather_client.WeatherError("weather unavailable")

    monkeypatch.setattr(weather_client, "fetch_tomorrow_weather", _raise)

    settings = Settings(station="Sutton", direction=None, num_mins=90, max_departures=5)
    result = get_departures(settings)

    assert result.tomorrow_weather is None
    assert result.departures

