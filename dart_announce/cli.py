"""Terminal CLI display for DART Announce.

Renders a departure board in the terminal using Unicode or ASCII box-drawing.
Can be run directly via:
    python -m dart_announce.cli
or viewed through HTTP via:
    curl http://<ip>:8000/terminal
"""

import argparse
import sys
import time
from datetime import datetime

from .config import Settings, load_settings
from .service import DeparturesResult, get_departures

# Ensure UTF-8 output on Windows terminals if supported
if sys.stdout and hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def render_terminal_board(
    result: DeparturesResult,
    direction: str | None = None,
    width: int = 68,
    use_ansi: bool = True,
    ascii_only: bool = False,
) -> str:
    """Render a formatted transit departure board as a multi-line string."""
    bold = "\033[1m" if use_ansi else ""
    inverse = "\033[7m" if use_ansi else ""
    reset = "\033[0m" if use_ansi else ""
    dim = "\033[2m" if use_ansi else ""

    # Box-drawing characters
    if ascii_only:
        tl, tr, bl, br = "+", "+", "+", "+"
        hz, vt, ml, mr = "-", "|", "+", "+"
        dash = "--"
    else:
        tl, tr, bl, br = "┌", "┐", "└", "┘"
        hz, vt, ml, mr = "─", "│", "├", "┤"
        dash = "──"

    lines = []
    lines.append(f"{tl}{hz * (width - 2)}{tr}")

    # Header: Station title + current time
    heading = f"DART {dash} {result.station_name.upper()}"
    if direction:
        heading += f" ({direction.upper()})"
    now_str = datetime.now().strftime("%H:%M:%S")

    pad_header = max(1, width - 4 - len(heading) - len(now_str))
    lines.append(f"{vt} {bold}{heading}{reset}{' ' * pad_header}{bold}{now_str}{reset} {vt}")

    # Subheader: Weather forecast
    if result.rain_chance is not None:
        if ascii_only:
            weather_text = f"Rain chance: {result.rain_chance}%"
        else:
            symbol = "☔" if result.rain_chance >= 40 else "☀"
            weather_text = f"Rain chance: {result.rain_chance}% {symbol}"
        pad_weather = max(1, width - 4 - len(weather_text))
        lines.append(f"{vt} {dim}{weather_text}{reset}{' ' * pad_weather}{vt}")

    lines.append(f"{ml}{hz * (width - 2)}{mr}")

    # Column widths
    col_dest = 24
    col_due = 10
    col_exp = 12
    col_status = width - 4 - col_dest - col_due - col_exp

    hdr = f"{'DESTINATION':<{col_dest}}{'DUE':>{col_due}}{'EXPECTED':>{col_exp}}{'STATUS':>{col_status}}"
    lines.append(f"{vt} {bold}{hdr}{reset} {vt}")
    lines.append(f"{ml}{hz * (width - 2)}{mr}")

    if result.departures:
        for d in result.departures:
            dest = d.destination[: col_dest - 1]
            raw_due = "DUE" if d.due_in <= 0 else f"{d.due_in} min"

            if d.due_in <= 0 and use_ansi:
                due_display = f"{inverse} DUE {reset}"
                due_pad = " " * max(0, col_due - 5)
                due_col = f"{due_pad}{due_display}"
            else:
                due_col = f"{raw_due:>{col_due}}"

            exp_col = f"{d.expected:>{col_exp}}"
            status_col = f"{d.status[: col_status - 1]:>{col_status}}"

            row = f"{dest:<{col_dest}}{due_col}{exp_col}{status_col}"
            lines.append(f"{vt} {row} {vt}")
    else:
        empty_text = "No upcoming departures scheduled"
        pad_left = (width - 4 - len(empty_text)) // 2
        pad_right = width - 4 - len(empty_text) - pad_left
        lines.append(f"{vt} {' ' * pad_left}{empty_text}{' ' * pad_right} {vt}")

    lines.append(f"{bl}{hz * (width - 2)}{br}")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Display live DART departures in the terminal")
    parser.add_argument("--station", "-s", default=None, help="Station name or code (e.g. Sutton, Connolly, SUTTN)")
    parser.add_argument("--direction", "-d", default=None, choices=["Northbound", "Southbound"], help="Direction filter")
    parser.add_argument("--watch", "-w", type=int, default=0, help="Refresh every N seconds (e.g. -w 30)")
    parser.add_argument("--plain", action="store_true", help="Disable ANSI colors")
    parser.add_argument("--ascii", action="store_true", help="Use standard ASCII borders (+, -, |)")
    parser.add_argument("--width", type=int, default=68, help="Table width in columns (default 68)")

    args = parser.parse_args()

    base_settings = load_settings()
    settings = Settings(
        station=args.station or base_settings.station,
        direction=args.direction if args.direction is not None else base_settings.direction,
        num_mins=base_settings.num_mins,
        max_departures=base_settings.max_departures,
        trmnl_webhook_url=base_settings.trmnl_webhook_url,
    )

    use_ansi = not args.plain and sys.stdout.isatty()

    try:
        while True:
            result = get_departures(settings)
            board = render_terminal_board(
                result,
                settings.direction,
                width=args.width,
                use_ansi=use_ansi,
                ascii_only=args.ascii,
            )

            if args.watch > 0:
                print("\033[2J\033[H", end="")

            print(board)

            if args.watch <= 0:
                break
            time.sleep(args.watch)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
