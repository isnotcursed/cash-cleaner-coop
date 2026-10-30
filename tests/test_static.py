from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LUA = ROOT / "ue4ss/Mods/CashCleanerCoopProbe/Scripts/main.lua"


def test_probe_is_read_only():
    text = LUA.read_text(encoding="utf-8")
    forbidden = (
        "io.open",
        "os.execute",
        "io.popen",
        "SetPropertyValue",
        "StaticConstructObject",
        "LoadAsset",
        "CallFunction",
    )
    for token in forbidden:
        assert token not in text


def test_probe_has_bounded_inventory():
    text = LUA.read_text(encoding="utf-8")
    assert "MAX_CLASSES" in text
    assert "count >= MAX_CLASSES" in text


def test_probe_maps_network_defaults_and_function_flags():
    text = LUA.read_text(encoding="utf-8")
    assert 'safe_property(cdo, "bReplicates")' in text
    assert 'safe_property(cdo, "NetDormancy")' in text
    assert "fn:GetFunctionFlags()" in text
    assert "SetFunctionFlags" not in text


def test_install_layout():
    mod = ROOT / "ue4ss/Mods/CashCleanerCoopProbe"
    assert (mod / "enabled.txt").is_file()
    assert (mod / "Scripts/main.lua").is_file()
