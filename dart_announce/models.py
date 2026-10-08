"""Clean data model for a single upcoming train, mapped from Irish Rail's raw XML fields."""

from dataclasses import dataclass


def _safe_int(value: any, default: int = 0) -> int:
    try:
        return int(str(value).strip())
    except (ValueError, TypeError, AttributeError):
        return default


@dataclass
class Departure:
    train_code: str
    origin: str
    destination: str
    due_in: int
    scheduled: str
    expected: str
    status: str
    late: int
    direction: str
    train_type: str

    @classmethod
    def from_raw(cls, raw: dict) -> "Departure":
        return cls(
            train_code=(raw.get("Traincode") or "").strip(),
            origin=raw.get("Origin") or "",
            destination=raw.get("Destination") or "",
            due_in=_safe_int(raw.get("Duein"), 0),
            scheduled=raw.get("Schdepart") or "",
            expected=raw.get("Expdepart") or "",
            status=raw.get("Status") or "",
            late=_safe_int(raw.get("Late"), 0),
            direction=raw.get("Direction") or "",
            train_type=raw.get("Traintype") or "",
        )

    def to_dict(self) -> dict:
        return {
            "destination": self.destination,
            "due_in": self.due_in,
            "due_in_text": "Due" if self.due_in <= 0 else f"{self.due_in} min",
            "scheduled": self.scheduled,
            "expected": self.expected,
            "status": self.status,
            "late": self.late,
            "direction": self.direction,
            "train_type": self.train_type,
        }
