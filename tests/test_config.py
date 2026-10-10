import pytest
from dart_announce import config


def test_load_settings_defaults(monkeypatch):
    monkeypatch.setenv("STATION", "Sutton")
    monkeypatch.setenv("DIRECTION", "")
    monkeypatch.setenv("NUM_MINS", "90")
    monkeypatch.setenv("MAX_DEPARTURES", "5")

    settings = config.load_settings()
    assert settings.station == "Sutton"
    assert settings.direction is None
    assert settings.num_mins == 90
    assert settings.max_departures == 5


def test_load_settings_direction_validation(monkeypatch):
    monkeypatch.setenv("STATION", "Sutton")
    monkeypatch.setenv("DIRECTION", "InvalidDirection")

    with pytest.raises(config.InvalidSettingsError, match="DIRECTION must be one of"):
        config.load_settings()


def test_load_settings_empty_station(monkeypatch):
    monkeypatch.setenv("STATION", "   ")

    with pytest.raises(config.InvalidSettingsError, match="STATION must not be empty"):
        config.load_settings()


def test_load_settings_num_mins_range(monkeypatch):
    monkeypatch.setenv("STATION", "Sutton")
    monkeypatch.setenv("NUM_MINS", "4")

    with pytest.raises(config.InvalidSettingsError, match="NUM_MINS must be between 5 and 90"):
        config.load_settings()


def test_load_settings_max_departures_range(monkeypatch):
    monkeypatch.setenv("STATION", "Sutton")
    monkeypatch.setenv("MAX_DEPARTURES", "0")

    with pytest.raises(config.InvalidSettingsError, match="MAX_DEPARTURES must be at least 1"):
        config.load_settings()


def test_load_settings_display_style(monkeypatch):
    monkeypatch.setenv("STATION", "Sutton")
    monkeypatch.delenv("DISPLAY_STYLE", raising=False)
    settings = config.load_settings()
    assert settings.display_style == "solari"

    monkeypatch.setenv("DISPLAY_STYLE", "matrix")
    settings_matrix = config.load_settings()
    assert settings_matrix.display_style == "matrix"

    monkeypatch.setenv("DISPLAY_STYLE", "dotmatrix")
    settings_dotmatrix = config.load_settings()
    assert settings_dotmatrix.display_style == "matrix"

    monkeypatch.setenv("DISPLAY_STYLE", "modern")
    settings_modern = config.load_settings()
    assert settings_modern.display_style == "matrix"

    monkeypatch.setenv("DISPLAY_STYLE", "plain")
    settings_plain = config.load_settings()
    assert settings_plain.display_style == "plain"

    monkeypatch.setenv("DISPLAY_STYLE", "reverse")
    settings_reverse = config.load_settings()
    assert settings_reverse.display_style == "reverse"

    monkeypatch.setenv("DISPLAY_STYLE", "inverted")
    settings_inverted = config.load_settings()
    assert settings_inverted.display_style == "reverse"

    monkeypatch.setenv("DISPLAY_STYLE", "dark")
    settings_dark = config.load_settings()
    assert settings_dark.display_style == "reverse"

    monkeypatch.setenv("DISPLAY_STYLE", "unknown_style")
    with pytest.raises(config.InvalidSettingsError, match="DISPLAY_STYLE must be one of"):
        config.load_settings()


def test_save_settings_with_display_style(monkeypatch, tmp_path):
    env_file = tmp_path / ".env"
    monkeypatch.setattr(config, "_ENV_PATH", env_file)

    saved = config.save_settings(
        station="Howth",
        direction="Southbound",
        display_style="matrix",
    )
    assert saved.display_style == "matrix"
    assert "DISPLAY_STYLE=matrix" in env_file.read_text()

    with pytest.raises(config.InvalidSettingsError, match="DISPLAY_STYLE must be one of"):
        config.save_settings(
            station="Howth",
            direction="Southbound",
            display_style="invalid",
        )


def test_load_settings_scheduler_defaults(monkeypatch):
    monkeypatch.setenv("STATION", "Sutton")
    monkeypatch.delenv("SCHEDULE_ENABLED", raising=False)
    monkeypatch.delenv("SCHEDULE_START_HOUR", raising=False)
    monkeypatch.delenv("SCHEDULE_END_HOUR", raising=False)

    settings = config.load_settings()
    assert settings.schedule_enabled is True
    assert settings.schedule_start_hour == 6
    assert settings.schedule_end_hour == 19


def test_load_settings_scheduler_custom(monkeypatch):
    monkeypatch.setenv("STATION", "Sutton")
    monkeypatch.setenv("SCHEDULE_ENABLED", "false")
    monkeypatch.setenv("SCHEDULE_START_HOUR", "7")
    monkeypatch.setenv("SCHEDULE_END_HOUR", "22")

    settings = config.load_settings()
    assert settings.schedule_enabled is False
    assert settings.schedule_start_hour == 7
    assert settings.schedule_end_hour == 22


def test_load_settings_scheduler_invalid_hours(monkeypatch):
    monkeypatch.setenv("STATION", "Sutton")
    monkeypatch.setenv("SCHEDULE_START_HOUR", "25")
    with pytest.raises(config.InvalidSettingsError, match="SCHEDULE_START_HOUR must be between 0 and 23"):
        config.load_settings()

    monkeypatch.setenv("SCHEDULE_START_HOUR", "6")
    monkeypatch.setenv("SCHEDULE_END_HOUR", "-1")
    with pytest.raises(config.InvalidSettingsError, match="SCHEDULE_END_HOUR must be between 0 and 23"):
        config.load_settings()


def test_settings_is_active_hours():
    from datetime import datetime

    # Disabled schedule: always active
    disabled_sched = config.Settings(
        station="Sutton", direction=None, num_mins=90, max_departures=5,
        schedule_enabled=False, schedule_start_hour=6, schedule_end_hour=19,
    )
    assert disabled_sched.is_active_hours(datetime(2026, 10, 10, 2, 0)) is True
    assert disabled_sched.is_active_hours(datetime(2026, 10, 10, 12, 0)) is True

    # Standard daytime schedule: 6:00 to 19:00
    normal_sched = config.Settings(
        station="Sutton", direction=None, num_mins=90, max_departures=5,
        schedule_enabled=True, schedule_start_hour=6, schedule_end_hour=19,
    )
    assert normal_sched.is_active_hours(datetime(2026, 10, 10, 5, 59)) is False
    assert normal_sched.is_active_hours(datetime(2026, 10, 10, 6, 0)) is True
    assert normal_sched.is_active_hours(datetime(2026, 10, 10, 12, 30)) is True
    assert normal_sched.is_active_hours(datetime(2026, 10, 10, 18, 59)) is True
    assert normal_sched.is_active_hours(datetime(2026, 10, 10, 19, 0)) is False
    assert normal_sched.is_active_hours(datetime(2026, 10, 10, 23, 0)) is False

    # Overnight schedule (e.g. night shift: 22:00 to 6:00)
    night_sched = config.Settings(
        station="Sutton", direction=None, num_mins=90, max_departures=5,
        schedule_enabled=True, schedule_start_hour=22, schedule_end_hour=6,
    )
    assert night_sched.is_active_hours(datetime(2026, 10, 10, 22, 0)) is True
    assert night_sched.is_active_hours(datetime(2026, 10, 10, 23, 59)) is True
    assert night_sched.is_active_hours(datetime(2026, 10, 10, 3, 0)) is True
    assert night_sched.is_active_hours(datetime(2026, 10, 10, 5, 59)) is True
    assert night_sched.is_active_hours(datetime(2026, 10, 10, 6, 0)) is False
    assert night_sched.is_active_hours(datetime(2026, 10, 10, 14, 0)) is False


def test_save_settings_with_scheduler(monkeypatch, tmp_path):
    env_file = tmp_path / ".env"
    monkeypatch.setattr(config, "_ENV_PATH", env_file)

    saved = config.save_settings(
        station="Howth",
        direction="Southbound",
        schedule_enabled=False,
        schedule_start_hour=8,
        schedule_end_hour=18,
    )
    assert saved.schedule_enabled is False
    assert saved.schedule_start_hour == 8
    assert saved.schedule_end_hour == 18

    content = env_file.read_text()
    assert "SCHEDULE_ENABLED=false" in content
    assert "SCHEDULE_START_HOUR=8" in content
    assert "SCHEDULE_END_HOUR=18" in content

