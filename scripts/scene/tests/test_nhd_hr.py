import sqlite3
import struct

import shapely
from shapely.geometry import Polygon, box

from camsim_scene.sources.nhd_hr import gpkg_geometry, keep_fcode, read_nhd


def _blob(geom) -> bytes:
    return b"GP" + bytes([0, 0x01]) + struct.pack("<i", 4326) + shapely.to_wkb(geom)  # flags: LE, no envelope


def _gpkg(path):
    c = sqlite3.connect(path)
    c.execute("create table gpkg_geometry_columns (table_name, column_name, geometry_type_name, srs_id, z, m)")
    for t in ("NHDWaterbody", "NHDArea"):
        c.execute(f"create table {t} (Permanent_Identifier text, FType int, FCode int, Shape blob)")
        c.execute("insert into gpkg_geometry_columns values (?, 'Shape', 'MULTIPOLYGON', 4326, 0, 0)", (t,))
    c.execute("create table NHDFCode (FCode int, Description text)")
    c.executemany("insert into NHDFCode values (?, ?)", [
        (39004, "Lake/Pond: Hydrographic Category = Perennial"),
        (39001, "Lake/Pond: Hydrographic Category = Intermittent"),
        (46006, "Stream/River: Hydrographic Category = Perennial"),
        (44500, "Sea/Ocean"),
    ])
    sq = box(-117.45, 33.25, -117.44, 33.26)
    c.executemany("insert into NHDWaterbody values (?, ?, ?, ?)", [
        ("lake", 390, 39004, _blob(sq)), ("dry", 390, 39001, _blob(sq)), ("sea", 445, 44500, _blob(sq))])
    c.execute("insert into NHDArea values ('river', 460, 46006, ?)", (_blob(sq),))
    c.commit()
    c.close()


def test_read_nhd_keeps_perennial_water_only(tmp_path):
    p = tmp_path / "t.gpkg"
    _gpkg(p)
    ids = sorted(f.source_id for f in read_nhd(p, (-117.5, 33.2, -117.4, 33.3)))
    assert ids == ["nhd:lake", "nhd:river"]


def test_read_nhd_clips_to_area(tmp_path):
    p = tmp_path / "t.gpkg"
    _gpkg(p)
    assert read_nhd(p, (-118.0, 34.0, -117.9, 34.1)) == []


def test_gpkg_geometry_skips_envelope():
    g = Polygon([(0, 0), (1, 0), (1, 1)])
    env = struct.pack("<4d", 0, 1, 0, 1)
    blob = b"GP" + bytes([0, 0x03]) + struct.pack("<i", 4326) + env + shapely.to_wkb(g)  # flags: LE, xy envelope
    assert gpkg_geometry(blob).equals(g)


def test_keep_fcode():
    assert keep_fcode(390, "Lake/Pond: Hydrographic Category = Perennial")
    assert not keep_fcode(390, "Lake/Pond: Hydrographic Category = Intermittent")
    assert not keep_fcode(460, "Stream/River: Hydrographic Category = Ephemeral")
    assert keep_fcode(436, "Reservoir")
    assert not keep_fcode(445, "Sea/Ocean")


def test_discover_keeps_hu4_geopackages(tmp_path):
    from camsim_scene.sources import nhd_hr
    from camsim_scene.sources.base import Area

    base = "https://prd-tnm.s3.amazonaws.com/StagedProducts/Hydrography/NHD/"
    items = [
        {"title": "NHD for Hydrological Unit (HU) 4 - 1807", "format": "GeoPackage", "sizeInBytes": 5,
         "downloadURL": base + "HU4/GPKG/NHD_H_1807_HU4_GPKG.zip"},
        {"title": "NHD - California", "format": "GeoPackage", "downloadURL": base + "State/GPKG/NHD_H_California_State_GPKG.zip"},
        {"title": "NHD for HU 4 - 1807", "format": "FileGDB", "downloadURL": base + "HU4/GDB/NHD_H_1807_HU4_GDB.zip"},
    ]
    orig = nhd_hr.tnm_products
    nhd_hr.tnm_products = lambda http, b, d: items
    try:
        a = nhd_hr.NhdHr({}, None).discover(Area(bounds=(-117.6, 33.2, -117.3, 33.5)))
    finally:
        nhd_hr.tnm_products = orig
    assert [x.id for x in a] == ["NHD_H_1807_HU4_GPKG"]
    assert a[0].size == 5


def test_prepare_unzips_once(tmp_path):
    import zipfile

    from camsim_scene.cache import Cache
    from camsim_scene.sources.base import Asset
    from camsim_scene.sources.nhd_hr import NhdHr

    z = tmp_path / "a.zip"
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("x.gpkg", b"abc")
    cache = Cache(tmp_path / "c")
    a = Asset(id="a", url="u", sha256="ab" * 32)
    out = NhdHr({}, None).prepare(z, a, cache)
    assert out.read_bytes() == b"abc" and out.suffix == ".gpkg"
    assert NhdHr({}, None).prepare(z, a, cache) == out
