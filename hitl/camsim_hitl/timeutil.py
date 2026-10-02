"""UTC from X-Plane 11's clock datarefs (HITL.md "Time and date").

X-Plane 11 has no UTC date and no year dataref; the date must be derived from
the local date. Pass the scenario's LOCAL year: a UTC year breaks the result for
up to about 14 h around every 1 January.
"""

from __future__ import annotations

from datetime import UTC, date, datetime, timedelta


def xp11_utc(
    local_time_sec: float,
    zulu_time_sec: float,
    local_month: int,
    local_day: int,
    lon_deg: float,
    local_year: int,
) -> datetime:
    """HITL.md's recipe; month and day from sim/cockpit2/clock_timer/current_*."""
    d = local_time_sec - zulu_time_sec  # = tz offset - 86400 * k
    k = round(((lon_deg / 15.0) * 3600.0 - d) / 86400.0)  # day wrap: -1, 0 or +1
    utc_day = date(local_year, local_month, local_day) - timedelta(days=k)
    return datetime(utc_day.year, utc_day.month, utc_day.day, tzinfo=UTC) + timedelta(seconds=zulu_time_sec)


def cigi_date(dt: datetime) -> tuple[int, int, int, int, int]:
    """(hour, minute, month, day, year) for Celestial Sphere Control."""
    dt = dt.astimezone(UTC)
    return dt.hour, dt.minute, dt.month, dt.day, dt.year
