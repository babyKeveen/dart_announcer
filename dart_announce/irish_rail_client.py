"""Thin client for Irish Rail's public real-time API.

No API key is required. Irish Rail explicitly offers no support or SLA for
this API, so every call here is wrapped to raise a clear IrishRailError
rather than letting raw network/XML exceptions bubble up.
"""

import httpx
import xmltodict

BASE_URL = "http://api.irishrail.ie/realtime/realtime.asmx"


class IrishRailError(Exception):
    """Raised when the Irish Rail API can't be reached or returns unparseable data."""


def _as_list(parsed: dict, array_key: str, item_key: str) -> list[dict]:
    """xmltodict collapses a single-item list to a bare dict; normalize to a list."""
    array = parsed.get(array_key) or {}
    items = array.get(item_key)
    if items is None:
        return []
    return items if isinstance(items, list) else [items]


def fetch_station_data(station_code: str, num_mins: int = 90, timeout: float = 10.0) -> list[dict]:
    """Return raw per-train dicts for the given station code, next `num_mins` minutes."""
    if not 5 <= num_mins <= 90:
        raise ValueError("num_mins must be between 5 and 90 (Irish Rail API limit)")

    url = f"{BASE_URL}/getStationDataByCodeXML_WithNumMins"
    params = {"StationCode": station_code, "NumMins": num_mins}
    try:
        response = httpx.get(url, params=params, timeout=timeout)
        response.raise_for_status()
    except httpx.HTTPError as exc:
        raise IrishRailError(f"Failed to fetch station data for '{station_code}': {exc}") from exc

    try:
        parsed = xmltodict.parse(response.text)
    except Exception as exc:
        raise IrishRailError(f"Failed to parse station data for '{station_code}': {exc}") from exc

    return _as_list(parsed, "ArrayOfObjStationData", "objStationData")


def fetch_all_stations(timeout: float = 10.0) -> list[dict]:
    """Return raw dicts for every station Irish Rail knows about (name, code, coords)."""
    url = f"{BASE_URL}/getAllStationsXML"
    try:
        response = httpx.get(url, timeout=timeout)
        response.raise_for_status()
    except httpx.HTTPError as exc:
        raise IrishRailError(f"Failed to fetch station list: {exc}") from exc

    try:
        parsed = xmltodict.parse(response.text)
    except Exception as exc:
        raise IrishRailError(f"Failed to parse station list: {exc}") from exc

    return _as_list(parsed, "ArrayOfObjStation", "objStation")


def fetch_stations_by_type(station_type: str = "D", timeout: float = 10.0) -> list[dict]:
    """Return raw station dicts filtered by StationType ('D' = DART, 'S' = Suburban, 'M' = Mainline)."""
    url = f"{BASE_URL}/getAllStationsXML_WithStationType"
    params = {"StationType": station_type}
    try:
        response = httpx.get(url, params=params, timeout=timeout)
        response.raise_for_status()
    except httpx.HTTPError as exc:
        raise IrishRailError(f"Failed to fetch stations of type '{station_type}': {exc}") from exc

    try:
        parsed = xmltodict.parse(response.text)
    except Exception as exc:
        raise IrishRailError(f"Failed to parse stations of type '{station_type}': {exc}") from exc

    return _as_list(parsed, "ArrayOfObjStation", "objStation")

