import os, sys, re
from PIL import Image
root = sys.argv[1]; z = int(sys.argv[2])  # existing lowest zoom
while z > 0:
    tiles = {}
    for x in os.listdir(f'{root}/{z}'):
        for f in os.listdir(f'{root}/{z}/{x}'):
            y = int(f.split('.')[0]); tiles.setdefault((int(x) // 2, y // 2), []).append((int(x), y))
    for (px, py), kids in tiles.items():
        im = Image.new('RGB', (512, 512))
        for x, y in kids:
            im.paste(Image.open(f'{root}/{z}/{x}/{y}.jpg'), ((x - 2 * px) * 256, (1 - (y - 2 * py)) * 256))
        os.makedirs(f'{root}/{z-1}/{px}', exist_ok=True); im.resize((256, 256), Image.LANCZOS).save(f'{root}/{z-1}/{px}/{py}.jpg', quality=85)
    z -= 1
x = open(f'{root}/tilemapresource.xml').read()
sets = ''.join(f'<TileSet href="{zz}" units-per-pixel="{180.0/256/(1<<zz):.16g}" order="{zz}"/>' for zz in range(0, 18))
x = re.sub(r'<TileSets profile="geodetic">.*</TileSets>', f'<TileSets profile="geodetic">{sets}</TileSets>', x, flags=re.S)
open(f'{root}/tilemapresource.xml', 'w').write(x); print('ok')
