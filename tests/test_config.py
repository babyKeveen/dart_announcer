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
