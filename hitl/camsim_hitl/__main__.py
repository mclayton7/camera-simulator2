"""IG host entry point: python -m camsim_hitl --config hitl.toml

Wires the truth receiver, the SOF-paced CIGI host, the gimbal device and the
camera component (each on the MAVLink link named in the config), then runs
until Ctrl-C / SIGTERM.
"""

from __future__ import annotations

import argparse
import logging
import signal
import sys
import threading
from pathlib import Path

from .camera import CameraComponent
from .cigi_host import CigiHost
from .config import Config, load_config, parse_hostport, resolve_path
from .geodesy import Geoid
from .gimbal import GimbalDevice
from .mavlink_link import MavlinkLink, open_transport
from .truth import TruthBuffer, TruthReceiver

log = logging.getLogger("camsim_hitl")


def setup_logging(level: str, file: str = "") -> None:
    handlers: list[logging.Handler] = [logging.StreamHandler(sys.stderr)]
    if file:
        handlers.append(logging.FileHandler(file))
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s.%(msecs)03d %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
        handlers=handlers,
        force=True,
    )


class App:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        geoid_path = resolve_path(cfg, cfg.geodesy.geoid_path) if cfg.geodesy.geoid_path else None
        self.geoid = Geoid(geoid_path, allow_missing=cfg.geodesy.allow_missing_geoid)
        t = cfg.truth
        self.truth = TruthBuffer(
            max_extrapolation_s=t.max_extrapolation_s,
            stale_after_s=t.stale_after_s,
            use_sender_clock=t.use_sender_clock,
            replay_policy=t.replay_policy,
            sim_speed_warn_below=t.sim_speed_warn_below,
        )
        host, port = parse_hostport(t.listen)
        self.truth_rx = TruthReceiver(self.truth, host, port)
        self.host = CigiHost(cfg, self.truth, self.geoid)

        self.links: dict[str, MavlinkLink] = {}
        for name, lc in cfg.mavlink.links.items():
            if not lc.url:
                raise ValueError(f"mavlink.links.{name}.url is empty")
            self.links[name] = MavlinkLink(name, open_transport(lc.url), lc.drop_msg_ids)

        self.gimbal: GimbalDevice | None = None
        if cfg.gimbal.enabled:
            self.gimbal = GimbalDevice(cfg.gimbal, cfg.mavlink.sysid, truth_fn=self.host.truth_at)
            self.host.gimbal_sample = self.gimbal.sample_q_bc
            self._attach(self.gimbal, cfg.gimbal.link, "gimbal")

        self.camera: CameraComponent | None = None
        if cfg.camera.enabled:
            self.camera = CameraComponent(cfg.camera, cfg.mavlink.sysid, host=self.host, base_dir=Path(cfg.base_dir))
            self.host.optics_fn = self.camera.optics
            self._attach(self.camera, cfg.camera.link, "camera")

    def _attach(self, comp, link: str, what: str) -> None:
        if not link:
            log.warning("%s: no MAVLink link configured; running without MAVLink", what)
            return
        if link not in self.links:
            raise ValueError(f"{what}.link = {link!r} is not in [mavlink.links]")
        self.links[link].register(comp)
        log.info("%s: sysid %d compid %d on link %s", what, comp.sysid, comp.compid, link)

    def start(self) -> None:
        self.truth_rx.start()
        if self.gimbal is not None:
            self.gimbal.start()
        if self.camera is not None:
            self.camera.start()
        for link in self.links.values():
            link.start()
        self.host.start()

    def stop(self) -> None:
        log.info("shutting down")
        self.host.stop()
        for link in self.links.values():
            link.stop()
        if self.camera is not None:
            self.camera.stop()
        if self.gimbal is not None:
            self.gimbal.stop()
        self.truth_rx.stop()
        self.truth_rx.join(timeout=1.0)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="camsim_hitl", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", "-c", help="TOML config (see hitl/hitl.example.toml); omit for defaults")
    ap.add_argument("--log-level", help="override log.level (DEBUG, INFO, WARNING)")
    ap.add_argument("--camsim", metavar="HOST:PORT", help="override cigi.camsim_host/camsim_port")
    ap.add_argument("--check-config", action="store_true", help="load the config, print it and exit")
    args = ap.parse_args(argv)

    cfg = load_config(args.config)
    if args.camsim:
        cfg.cigi.camsim_host, cfg.cigi.camsim_port = parse_hostport(args.camsim, "127.0.0.1")
    setup_logging(args.log_level or cfg.log.level, cfg.log.file)
    if args.check_config:
        import pprint

        pprint.pprint(cfg)
        return 0
    try:
        app = App(cfg)
    except (OSError, ValueError) as e:
        log.error("%s", e)
        return 2
    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    app.start()
    log.info("IG host running (Ctrl-C to stop)")
    try:
        while not stop.wait(0.5):
            pass
    finally:
        app.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
