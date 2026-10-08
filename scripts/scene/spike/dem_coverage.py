import json, sys, glob, rasterio, numpy as np
from rasterio.enums import Resampling
d = json.load(open(sys.argv[1]))
for it in sorted(d['items'], key=lambda i: i['title'][-30:]):
    url = it['downloadURL']
    try:
        with rasterio.Env(GDAL_DISABLE_READDIR_ON_OPEN='EMPTY_DIR', CPL_VSIL_CURL_ALLOWED_EXTENSIONS='.tif'):
            with rasterio.open('/vsicurl/' + url) as ds:
                ovr = ds.overviews(1)
                f = max(ovr) if ovr else 32
                a = ds.read(1, out_shape=(ds.height // f, ds.width // f), resampling=Resampling.nearest, masked=True)
                valid = 1 - np.ma.getmaskarray(a).mean()
                crs = ds.crs.to_string() if ds.crs else None
                print(f"{it['title'][-45:]:45s} valid={valid:5.1%} {ds.width}x{ds.height} res={ds.res[0]:.2f} ovr={ovr[:3]} tiled={ds.profile.get('tiled')} comp={ds.profile.get('compress')} dtype={ds.dtypes[0]} crs={crs[:60]}")
    except Exception as e:
        print(it['title'][-45:], 'ERR', str(e)[:120])
