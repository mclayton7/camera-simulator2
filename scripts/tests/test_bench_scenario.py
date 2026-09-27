"""The bench scenario is deterministic and every phase produces valid CIGI datagrams."""
import struct

from bench import scenario


def test_phases_have_expected_names_and_are_deterministic():
    names = [p.name for p in scenario.build_phases()]
    assert names == ["warmup", "orbit", "slew", "low_pass", "far_origin"]
    a = [p.pose_at(t) for p in scenario.build_phases() for t in (0.0, 1.5, 7.25)]
    b = [p.pose_at(t) for p in scenario.build_phases() for t in (0.0, 1.5, 7.25)]
    assert a == b


def test_only_warmup_is_unmeasured():
    measured = {p.name: p.measured for p in scenario.build_phases()}
    assert measured == {"warmup": False, "orbit": True, "slew": True, "low_pass": True, "far_origin": True}


def test_smoke_is_short_and_has_one_shot():
    phases = scenario.build_phases(smoke=True)
    assert sum(p.duration_s for p in phases) <= 25
    assert len(scenario.build_shots(smoke=True)) == 1


def test_slew_phase_reaches_fast_gimbal_rates():
    slew = next(p for p in scenario.build_phases() if p.name == "slew")
    dt = 1.0 / 30.0
    rates = [abs(slew.pose_at(t + dt).gimbal_yaw - slew.pose_at(t).gimbal_yaw) / dt
             for t in [i * dt for i in range(int(slew.duration_s * 30) - 1)]]
    assert max(rates) >= 40.0  # deg/s: well above the 10 deg/s prefetch threshold


def test_far_origin_is_about_300_km_from_orbit():
    orbit = next(p for p in scenario.build_phases() if p.name == "orbit").pose_at(0.0)
    far = next(p for p in scenario.build_phases() if p.name == "far_origin").pose_at(0.0)
    # 1 deg longitude at ~37.8 N is about 87.9 km.
    km = abs(far.lon - orbit.lon) * 87.9
    assert 280.0 <= km <= 320.0


def test_host_datagram_starts_with_ig_control_and_carries_art_part():
    pose = scenario.build_phases()[1].pose_at(0.0)
    dgram = scenario.host_datagram(7, pose)
    assert dgram[0] == 1 and dgram[1] == 24          # IG Control
    assert struct.unpack(">I", dgram[8:12])[0] == 7  # frame counter
    ids = []
    i = 0
    while i < len(dgram):
        ids.append(dgram[i])
        i += dgram[i + 1]
    assert i == len(dgram)                            # packets tile the datagram
    assert 2 in ids and 6 in ids and 9 in ids         # entity, art part, celestial


def test_shot_names_are_unique_and_filesystem_safe():
    names = [s.name for s in scenario.build_shots()]
    assert len(names) == len(set(names)) >= 6
    assert all(n.replace("_", "").isalnum() for n in names)
