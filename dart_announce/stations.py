"""Resolve a human-friendly station name (or code) to an Irish Rail station code."""

from functools import lru_cache

from . import irish_rail_client


class UnknownStationError(Exception):
    """Raised when a configured station name/code doesn't match any known station."""


# Default fallback 33 DART stations ordered North to South along the coastline
DEFAULT_DART_STATIONS = [
    ("Malahide", "MHIDE"),
    ("Portmarnock", "PMNCK"),
    ("Clongriffin", "GRGRD"),
    ("Sutton", "SUTTN"),
    ("Bayside", "BYSDE"),
    ("Howth Junction", "HWTHJ"),
    ("Howth", "HOWTH"),
    ("Kilbarrack", "KBRCK"),
    ("Raheny", "RAHNY"),
    ("Harmonstown", "HTOWN"),
    ("Killester", "KLSTR"),
    ("Clontarf Road", "CTARF"),
    ("Dublin Connolly", "CNLLY"),
    ("Tara Street", "TARA"),
    ("Dublin Pearse", "PERSE"),
    ("Grand Canal Dock", "GCDK"),
    ("Lansdowne Road", "LDWNE"),
    ("Sandymount", "SMONT"),
    ("Sydney Parade", "SIDNY"),
    ("Booterstown", "BTSTN"),
    ("Blackrock", "BROCK"),
    ("Seapoint", "SEAPT"),
    ("Salthill and Monkstown", "SHILL"),
    ("Dun Laoghaire", "DLERY"),
    ("Sandycove", "SCOVE"),
    ("Glenageary", "GLGRY"),
    ("Dalkey", "DLKEY"),
    ("Killiney", "KILNY"),
    ("Shankill", "SKILL"),
    ("Woodbrook", "WBROK"),
    ("Bray", "BRAY"),
    ("Greystones", "GSTNS"),
    ("Kilcoole", "KCOOL"),
]

# Backward-compatibility alias
DART_STATIONS = DEFAULT_DART_STATIONS

_cached_dart_stations: list[dict] | None = None


@lru_cache(maxsize=1)
def _all_stations() -> list[dict]:
    return irish_rail_client.fetch_all_stations()


def get_dart_stations(force_refresh: bool = False) -> list[dict]:
    """Return list of dicts with name and code for all DART stations.

    Fetches dynamically from Irish Rail's official StationType='D' API and sorts
    geographically North to South. Automatically falls back to DEFAULT_DART_STATIONS
    if Irish Rail API is unreachable.
    """
    global _cached_dart_stations

    if _cached_dart_stations is not None and not force_refresh:
        return _cached_dart_stations

    try:
        raw_stations = irish_rail_client.fetch_stations_by_type("D")
        if raw_stations:
            def _lat(st: dict) -> float:
                try:
                    return float(st.get("StationLatitude", 0))
                except (ValueError, TypeError):
                    return 0.0

            raw_stations.sort(key=_lat, reverse=True)
            _cached_dart_stations = [
                {
                    "name": s.get("StationDesc", "").strip(),
                    "code": (s.get("StationCode") or "").strip(),
                }
                for s in raw_stations
                if s.get("StationDesc")
            ]
            return _cached_dart_stations
    except Exception:
        pass

    if _cached_dart_stations is not None:
        return _cached_dart_stations

    return [{"name": name, "code": code} for name, code in DEFAULT_DART_STATIONS]


def reload_dart_stations() -> tuple[list[dict], bool]:
    """Force an immediate reload of DART stations from Irish Rail API.

    Also flushes the station resolver cache so newly opened stations resolve immediately.
    Returns (stations, is_live_from_api).
    """
    global _cached_dart_stations
    _all_stations.cache_clear()
    try:
        raw = irish_rail_client.fetch_stations_by_type("D")
        if raw:
            def _lat(st: dict) -> float:
                try:
                    return float(st.get("StationLatitude", 0))
                except (ValueError, TypeError):
                    return 0.0

            raw.sort(key=_lat, reverse=True)
            _cached_dart_stations = [
                {
                    "name": s.get("StationDesc", "").strip(),
                    "code": (s.get("StationCode") or "").strip(),
                }
                for s in raw
                if s.get("StationDesc")
            ]
            return _cached_dart_stations, True
    except Exception:
        pass

    stations = get_dart_stations()
    return stations, False


def resolve_station(station: str, stations: list[dict] | None = None) -> dict:
    """Resolve `station` (a code like 'SUTTN' or a name like 'Sutton') to its full station entry.

    Checks for a code match first, then falls back to a case-insensitive name match.
    `stations` is injectable for testing; defaults to the live (cached) station list.
    """
    station = station.strip()
    if not station:
        raise UnknownStationError("No station configured")

    candidates = _all_stations() if stations is None else stations
    upper = station.upper()

    for entry in candidates:
        if (entry.get("StationCode") or "").strip().upper() == upper:
            return entry

    for entry in candidates:
        if (entry.get("StationDesc") or "").strip().upper() == upper:
            return entry

    for entry in candidates:
        if (entry.get("StationAlias") or "").strip().upper() == upper:
            return entry

    raise UnknownStationError(f"Could not find a station matching '{station}'")


def resolve_station_code(station: str, stations: list[dict] | None = None) -> str:
    """Resolve `station` to just its station code."""
    return resolve_station(station, stations)["StationCode"].strip()
