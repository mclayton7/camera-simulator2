import pyproj
from pyproj.transformer import TransformerGroup
pyproj.network.set_network_enabled(True)
lon, lat, h = -117.40, 33.30, 100.0
x, y = pyproj.Transformer.from_crs(4326, 6340, always_xy=True).transform(lon, lat)
for src, dst, label in [("EPSG:6340+5703", "EPSG:7912", "NAD83(2011) UTM11 + NAVD88 -> ITRF2014 3D"),
                        ("EPSG:26911+5703", "EPSG:4979", "NAD83 UTM11 + NAVD88 -> WGS84 3D (as labelled in the 3DEP tiles)")]:
    g = TransformerGroup(src, dst, always_xy=True)
    t = g.transformers[0]
    print(label); print('  op:', t.description[:150]); print('  unavailable ops:', len(g.unavailable_operations))
    print('  result', [round(v, 3) for v in t.transform(x, y, h)])
g = pyproj.Transformer.from_crs("EPSG:4979", "EPSG:4326+5773", always_xy=True)
print('EGM96 geoid undulation at point:', round(h - g.transform(lon, lat, h)[2], 3))
