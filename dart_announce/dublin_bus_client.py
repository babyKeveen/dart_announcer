"""Client for Dublin Bus real-time timetables (via bustimes.org open transit feed).

Supports Stop 917 (Admiral Park, Baldoyle) and Route H1, parsing upcoming departures
and computing live countdown minutes without requiring proprietary API keys.
"""

from datetime import datetime
import re
from typing import List, Optional

try:
    import httpx
except ImportError:
    httpx = None
import urllib.request

from .models import BusDeparture

DEFAULT_STOP_ID = "8240DB000917"  # Stop 917, Admiral Park, Willie Nolan Rd, Baldoyle
DEFAULT_ROUTE = "H1"
DEFAULT_STOP_NAME = "Stop 917 (Admiral Park)"

DEFAULT_STOP_ID_2 = "8240DB000929"  # Stop 929, Warrenhouse Road, Baldoyle
DEFAULT_ROUTE_2 = "H2"
DEFAULT_STOP_NAME_2 = "Stop 929 (Warrenhouse Rd)"


class DublinBusError(Exception):
    """Raised when bus timetable cannot be fetched or parsed."""


def strip_html_tags(text: str) -> str:
    cleaned = re.sub(r"<[^>]+>", "", text)
    return cleaned.replace("&nbsp;", " ").strip()


def find_time_hhmm(text: str) -> str:
    m = re.search(r"([0-2]?[0-9]:[0-5][0-9])", text)
    return m.group(1) if m else ""


def parse_bus_html(
    html: str,
    route_filter: Optional[str] = DEFAULT_ROUTE,
    ref_time: Optional[str] = None,
) -> List[BusDeparture]:
    """Parse bustimes.org HTML table and return departures sorted by due_in."""
    departures: List[BusDeparture] = []
    if not html:
        return departures

    ref_mins: Optional[int] = None
    if ref_time and len(ref_time) >= 5 and ":" in ref_time:
        try:
            rh, rm = map(int, ref_time[:5].split(":"))
            ref_mins = rh * 60 + rm
        except ValueError:
            pass

    if ref_mins is None:
        now = datetime.now()
        ref_mins = now.hour * 60 + now.minute

    target_rt = (route_filter or "").strip().upper()

    rows = re.findall(r"<tr[^>]*>(.*?)</tr>", html, re.DOTALL)
    for row in rows:
        tds = re.findall(r"<td[^>]*>(.*?)</td>", row, re.DOTALL)
        if len(tds) < 3:
            continue

        route = strip_html_tags(tds[0])
        if target_rt and route.upper() != target_rt:
            continue

        dest_cell = re.sub(r"<div[^>]*>.*?</div>", "", tds[1], flags=re.DOTALL)
        dest_raw = strip_html_tags(dest_cell)
        dest = dest_raw.splitlines()[0].strip() if dest_raw else "Dublin City"
        sched = find_time_hhmm(tds[2])

        exp = ""
        is_live = False
        if len(tds) >= 4:
            exp = find_time_hhmm(tds[3])
            if exp:
                is_live = True

        if not exp:
            exp = sched

        if not sched and not exp:
            continue

        due_in = 0
        if exp and ":" in exp:
            try:
                bh, bm = map(int, exp.split(":"))
                bus_mins = bh * 60 + bm
                diff = bus_mins - ref_mins
                if diff < -720:
                    diff += 1440
                elif diff > 720:
                    diff -= 1440

                if diff < -5:
                    # Already departed in the past
                    continue
                due_in = max(0, diff)
            except ValueError:
                due_in = 0

        departures.append(
            BusDeparture(
                route=route or (route_filter or "H1"),
                destination=dest or "Dublin City",
                due_in=due_in,
                scheduled=sched or "--:--",
                expected=exp or sched or "--:--",
                status="Live" if is_live else "Sched",
            )
        )

    departures.sort(key=lambda d: d.due_in)
    return departures


def fetch_bus_departures(
    stop_id: str = DEFAULT_STOP_ID,
    route_filter: Optional[str] = DEFAULT_ROUTE,
    ref_time: Optional[str] = None,
    timeout: float = 8.0,
) -> List[BusDeparture]:
    """Fetch timetable for stop_id from bustimes.org and parse upcoming departures."""
    url = f"https://bustimes.org/stops/{stop_id}"
    html = ""
    try:
        if httpx is not None:
            headers = {"User-Agent": "Mozilla/5.0 (ESP32-S3 Commute Board)"}
            resp = httpx.get(url, headers=headers, timeout=timeout, follow_redirects=True)
            resp.raise_for_status()
            html = resp.text
        else:
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (ESP32-S3 Commute Board)"})
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                html = resp.read().decode("utf-8", errors="replace")
    except Exception as exc:
        raise DublinBusError(f"Failed to fetch Dublin Bus data for stop '{stop_id}': {exc}") from exc

    return parse_bus_html(html, route_filter=route_filter, ref_time=ref_time)


def fetch_two_routes_departures(
    stop_id_1: str = DEFAULT_STOP_ID,
    route_1: str = DEFAULT_ROUTE,
    stop_id_2: str = DEFAULT_STOP_ID_2,
    route_2: str = DEFAULT_ROUTE_2,
    ref_time: Optional[str] = None,
    timeout: float = 8.0,
) -> List[BusDeparture]:
    """Fetch next bus from Route 1 and Route 2 (one row each or no row), sorted by countdown minutes."""
    results: List[BusDeparture] = []

    # Route 1: H1 from Stop 917 (Admiral Park)
    try:
        deps1 = fetch_bus_departures(stop_id=stop_id_1, route_filter=route_1, ref_time=ref_time, timeout=timeout)
        if deps1:
            bus1 = deps1[0]
            bus1.route = route_1
            results.append(bus1)
    except Exception:
        pass

    # Route 2: H2 from Stop 929 (Warrenhouse Road)
    try:
        deps2 = fetch_bus_departures(stop_id=stop_id_2, route_filter=route_2, ref_time=ref_time, timeout=timeout)
        if deps2:
            bus2 = deps2[0]
            bus2.route = route_2
            results.append(bus2)
    except Exception:
        pass

    results.sort(key=lambda d: d.due_in)
    return results
