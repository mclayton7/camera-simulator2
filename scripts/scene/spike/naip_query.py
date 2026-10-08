import pystac_client, collections
cat = pystac_client.Client.open("https://planetarycomputer.microsoft.com/api/stac/v1")
items = list(cat.search(collections=["naip"], bbox=[-117.62,33.19,-117.24,33.52]).items())
by = collections.defaultdict(list)
for i in items: by[i.properties.get('naip:year')].append(i)
for y in sorted(by):
    its = by[y]; i=its[0]
    print(y, len(its), 'gsd', i.properties.get('gsd'), 'dates', min(x.properties['datetime'][:10] for x in its), max(x.properties['datetime'][:10] for x in its), 'bands', [b.get('name') for b in i.assets['image'].extra_fields.get('eo:bands',[])], 'proj', i.properties.get('proj:epsg'))
latest = by[max(by)]
print('latest asset example', latest[0].assets['image'].href[:110])
