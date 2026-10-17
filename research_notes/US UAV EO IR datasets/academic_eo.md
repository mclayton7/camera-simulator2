# Academic / open visible-band (EO) aerial and UAV vehicle datasets: US coverage, licences, geolocation

Scope: EO aerial/UAV datasets with labelled vehicles, judged as a fixed real test set for detectors trained on CamSim synthetic data. The scene would be rebuilt from USGS 3DEP + NAIP, so the data must be over the US, from UAV or aircraft at about 50 m to 3 km. Researched 2026-10-15. Each fact below cites what was actually retrieved. Items marked "(unverified, prior knowledge)" were not confirmed this session and sit in the Gaps sections.

## Which candidates are genuinely over the US AND from an aerial (not satellite) platform with vehicle labels?

### Takeaway
Only a few verified candidates are both US and aerial with vehicle labels:
- **Stanford Drone Dataset** (Stanford campus, CA; about 80 m UAV, nadir; cars, buses and golf carts are minority classes).
- **The US parts of COWC** (Columbus, OH and Utah; 15 cm aerial ortho, car labels).
- **VEDAI** (Utah AGRC 2012 6-inch aerial orthophotos; RGB + NIR; vehicle classes).
- **The US intersection and roundabout sites of CitySim** (UCF; drone video; rotated boxes; GPS coordinates per position).

There are also three FHWA/DOT trajectory sets: TGSIM (helicopter, Chicago and Washington DC), HIGH-SIM (helicopter, I-75 Florida) and the AFRL WAMI set WPAFB 2009 (Dayton, OH). They are US and aerial, but they are trajectory or tracking products rather than detector-ready image sets. CARPK, PUCPR+, DOTA/iSAID (aerial part), UAV-OBB, VisDrone, UAVDT and the other UAV benchmarks are not US.

### Cited Findings

**Stanford Drone Dataset (SDD), US, Stanford campus, California**
- 8 scenes and 60 videos: gates 9, little 4, nexus 12, coupa 4, bookstore 7, deathCircle 5, quad 4, hyang 15. Classes are pedestrians, bicyclists, skateboarders, cars, buses and golf carts ("Cart"). Download is a single ~69 GB zip (`vatic2.stanford.edu/stanford_campus_dataset.zip`) — [SDD home](https://cvgl.stanford.edu/projects/uav_data/)
- The home page gives no resolution, altitude or GPS — [SDD home](https://cvgl.stanford.edu/projects/uav_data/)
- A secondary survey says "a 4k camera on a quadcopter … hovered above various intersections on campus with a flight height of about 80 meters" (no primary source found for the altitude) — [UAV survey, arXiv 2110.12638](https://arxiv.org/pdf/2110.12638)
- Citation: Robicquet et al., "Learning Social Etiquette: Human Trajectory Prediction in Crowded Scenes", ECCV 2016 — [SDD home](https://cvgl.stanford.edu/projects/uav_data/)

**CARPK, NOT US (very likely Taiwan); PUCPR+, NOT US (Curitiba, Brazil)**
- CARPK: Phantom 3 Professional at about 40 m, "4 different parking lots", 89,777 cars, bounding boxes — [CARPK/LPN page](https://lafi.github.io/LPN/); [Hsieh et al., ICCV 2017, arXiv 1707.05972](https://arxiv.org/pdf/1707.05972)
- Neither the paper nor the project page names the lots' location. The authors' affiliation is "National Taiwan University, Taipei, Taiwan", and the project page's demo video is of the Taipei Zoo parking lot (not a CARPK source) — [arXiv 1707.05972](https://arxiv.org/pdf/1707.05972); [LPN page](https://lafi.github.io/LPN/)
- Size: 1,444 images (989 train / 495 test). PUCPR+ has 125 images and 16,456 cars, while the original paper says "nearly 17,000" — [arXiv 2408.14457](https://arxiv.org/pdf/2408.14457); [arXiv 1707.05972](https://arxiv.org/pdf/1707.05972)
- PUCPR+ re-annotates the PUCPR subset of PKLot. That subset was shot by "a fixed camera view from a high story building", so it is not aerial — [arXiv 1707.05972](https://arxiv.org/pdf/1707.05972)
- PKLot images "were acquired in the parking lots of the Federal University of Parana (UFPR) and the Pontifical Catholic University of Parana (PUCPR), located in Curitiba, Brazil" — [PKLot survey draft (PUCPR)](https://www.ppgia.pucpr.br/~jean.barddal/assets/pdf/draft_pklot_survey2022.pdf)
- Licence: "made available for academic research purpose only". The files are password-protected and you get the password after an online EULA form. The data sits on Google Drive (~2.0 GB) — [LPN page](https://lafi.github.io/LPN/)

**COWC (Cars Overhead With Context, LLNL): partly US**
- Locations: "Toronto, Canada; Selwyn, New Zealand; Potsdam, Germany; Vaihingen, Germany; Columbus, Ohio, United States; Utah, United States". Imagery is "15 cm per pixel resolution at ground (all data is EO)". 32,716 unique annotated cars and 58,247 negative examples — [COWC home](https://gdo152.llnl.gov/cowc/)
- Downloads: `ftp://gdo152.ucllnl.org/pub/cowc/`, `https://gdo152.llnl.gov/cowc/download/`, and scripts on GitHub — [COWC home](https://gdo152.llnl.gov/cowc/)
- The COWC-M extension adds car-type labels: Sedan, Pickup, Other, Unknown — [LLNL/cowc GitHub](https://github.com/LLNL/cowc)
- Per the CARPK authors' comparison, COWC's resolution is low compared with drone imagery — [arXiv 1707.05972](https://arxiv.org/pdf/1707.05972)

**VEDAI: US (Utah), aerial orthophoto**
- The terms say the base is "4 sets of images, which are a modified version of the images from the website UTAH AGRC: http://gis.utah.gov/data/aerial-photography/2012-hro-6-inch-color-orthophotography/" — [VEDAI Terms of Use PDF](https://downloads.greyc.fr/vedai/TermsandConditionsofUseVeDAI2014.pdf)
- Comes in 512×512 and 1024×1024 sets, "each image is available in several spectral bands and resolutions", and vehicles appear in multiple orientations — [VEDAI download page](https://downloads.greyc.fr/vedai/)
- 12.5 cm GSD for 1024×1024 and 25 cm for 512×512 — [arXiv 2307.06724](https://arxiv.org/pdf/2307.06724) (via search snippet)

**CitySim (UCF SST Lab): partly US**
- 1,140 minutes of drone video at 30 fps over 12 locations. Data are vehicle trajectories with rotated bounding boxes, plus 3D base maps and signal timings — [CitySim GitHub](https://github.com/UCF-SST-Lab/UCF-SST-CitySim1-Dataset); [arXiv 2208.11036](https://arxiv.org/abs/2208.11036)
- Site table: Intersections A–F and Roundabouts A–B are "USA" (C and F are TBA). Expressway A/B and Freeways B/C/D are in "Asia" with partners Southwest Jiaotong University, Southeast University and Hong Kong PolyU. No city or state is named — [CitySim GitHub](https://github.com/UCF-SST-Lab/UCF-SST-CitySim1-Dataset)
- Per a search snippet of the paper, each position is given "in pixels, feet, and GPS coordinates" with bounding box vertices — [arXiv 2208.11036 PDF](https://arxiv.org/pdf/2208.11036)
- Full-dataset access requires a request form sent to the dataset holder — [tactics2d docs](https://tactics2d.readthedocs.io/en/latest/dataset_support/citysim/)

**TGSIM (FHWA Third Generation Simulation): US, helicopter aerial video, trajectories**
- "Fixed location aerial videography was used to collect data on I–90/I–94 in Chicago, IL … Moving aerial videography was used to collect four datasets on I–90/I–94 and I–294 in Chicago, IL." The I-294 L2 set used "one high-resolution 8K camera mounted on a helicopter". The deliverables are trajectory CSVs plus reference images and lane centrelines. The I-294 site is near Hinsdale, IL — [data.gov TGSIM I-294 L2](https://catalog.data.gov/dataset/third-generation-simulation-data-tgsim-i-294-l2-trajectories); [TGSIM report, ROSA P](https://rosap.ntl.bts.gov/view/dot/74647)
- Other TGSIM sets: Foggy Bottom (Washington DC), I-395, and I-90/I-94 stationary and moving — [data.gov Foggy Bottom](https://catalog.data.gov/dataset/third-generation-simulation-data-tgsim-foggy-bottom-trajectories); [data.gov I-395](https://catalog-old.data.gov/dataset/third-generation-simulation-data-tgsim-i-395-trajectories); [data.gov I-90/I-94 stationary](https://catalog.data.gov/dataset/third-generation-simulation-data-tgsim-i-90-i-94-stationary-trajectories)
- The search results do not show that the raw video can be downloaded, only the derived trajectories and reference images — [data.gov TGSIM I-294 L2](https://catalog.data.gov/dataset/third-generation-simulation-data-tgsim-i-294-l2-trajectories)

**HIGH-SIM: US, helicopter, I-75 Florida**
- "a 2-hour, 8000 ft vehicle trajectory data from aerial video on I-75". Recorded 16:15–18:15 on Tuesday 14 May 2019 from helicopter video, by USF and Leidos (Shi et al. 2021, *Communications in Transportation Research*). The authors say it is published for public use — [DOAJ record](https://doaj.org/article/c49abf4cd27f4beba34d1b152d654009); [UW CATS lab](https://catslab.engr.wisc.edu/trajectory-data-collection/)

**WPAFB 2009 (AFRL wide-area motion imagery): US, Dayton OH area, airborne**
- 1,025 frames, all vehicles and trajectories manually annotated, about 35 km², ~0.25 m GSD. It is "one of the only publicly available [WAMI datasets] with human-annotated vehicle locations". Distributed through AFRL SDMS (collection id `wpafb2009`) — [arXiv 1911.01727](https://arxiv.org/pdf/1911.01727); [ClusterNet arXiv 1704.02694](https://arxiv.org/pdf/1704.02694)
- UCF CRCV generated ground truth for 4 sub-sequences of the CLIF (Columbus Large Image Format) data (seen only via a mirror) — [UCF CRCV CLIF (mirror)](https://backiee.wasmer.app/http_crcv_ucf_edu/data/CLIF_php)

**DOTA / iSAID: mixed sources; no stated US portion**
- Image sources: Google Earth; GF-2 and JL-1 satellites; "aerial imagery provided by CycloMedia B.V." (a Netherlands company). Vehicle classes are "large vehicle" and "small vehicle". Labels are oriented boxes (4 vertices). Metadata gives acquisition date, source and GSD only, with no coordinates and no mention of US locations — [DOTA dataset page](https://captain-whu.github.io/DOTA/dataset.html)

**xView: satellite (0.3 m), global**
- 60 classes, about 1.0 M instances, 1,415 km², bounding boxes, "images from complex scenes around the world". No US share is given on the page — [xView home](https://xviewdataset.org/)

**Non-US UAV vehicle sets (confirmed)**
- UAV-OBB (2026): Chongqing and Wuhan, China. DJI Mavic 3, 75–108 m, near nadir, 1,617 images at 1920×1080, 46,807 oriented boxes, 6 classes, CC BY 4.0, Mendeley DOI 10.17632/6snrjwcpkh.4 — [UAV-OBB, Data in Brief (PMC)](https://pmc.ncbi.nlm.nih.gov/articles/PMC13092195/)
- UAVDT: 80,000 frames at 1080×540, cars/buses/trucks — [UAVDT ECCV 2018](https://openaccess.thecvf.com/content_ECCV_2018/papers/Dawei_Du_The_Unmanned_Aerial_ECCV_2018_paper.pdf)
- Songdo Vision / Songdo Traffic: georeferenced drone trajectories, 5,000+ annotated frames and ~300k vehicles, Songdo, South Korea — [arXiv 2411.02136](https://arxiv.org/html/2411.02136v2)
- UTUAV (Drones 2025): 100 and 120 m semi-static drone, cars/motorcycles/large vehicles. The search results did not state a location — [UTUAV, Drones 10(1):15](https://doi.org/10.3390/drones10010015)

### Inferences
- **Best US aerial EO fits for CamSim's real-domain test set, in order:**
  1. **VEDAI.** Utah orthophotos at 12.5 cm, which matches a NAIP-like nadir look, and it includes NIR. It is effectively an aerial orthophoto (several hundred m to km altitude), not a UAV view.
  2. **COWC Columbus + Utah.** 15 cm ortho with car labels. Its labels are car centre points (see Gaps), so they need box conversion.
  3. **SDD.** True UAV at ~80 m nadir over a known campus that can be rebuilt from 3DEP + NAIP. But vehicles are a small share of its labels (pedestrians and bikers dominate), the labels are axis-aligned tracks, and there is no geotag.
  4. **CitySim US sites.** UAV video with rotated boxes and GPS coordinates, a strong match for georeferenced reconstruction. But the sites are not named and access is by request.
  5. **TGSIM / HIGH-SIM.** Helicopter at higher altitude over US highways. Trajectories only, so imagery must be requested or is not released.
  6. **WPAFB 2009.** Grayscale WAMI from an aircraft at roughly 2 km, US, with vehicle tracks. It fits the 3 km upper bound but is panchromatic.
- Even though the data are US, VEDAI cannot be used for anything commercial. COWC's terms need confirming (see Gaps).
- Inferred from the image sources: the DOTA/iSAID aerial portion is CycloMedia (Netherlands), and any US content would be inside Google Earth tiles with no geotag. That makes it a poor fit.

### Gaps
- CARPK's exact location is never stated in the primary sources. "Taiwan" is an inference from the NTU authorship and the Taipei Zoo demo. Nothing suggests the US.
- SDD frame resolution (often quoted as about 1400×1904, varying per scene) and its altitude were not confirmed from a primary source.
- COWC label format: it was not confirmed this session that COWC labels are single-pixel car centres (prior knowledge says so).
- CitySim US site cities: prior knowledge says Florida (Orlando area). The repo only says "USA", so this is unverified.
- WPAFB 2009 availability on AFRL SDMS in 2026 is unverified. The SDMS site did not appear in search, and registration has historically been required (prior knowledge).
- Not verified this session (prior knowledge only, treat as provisional):
  - VisDrone: 14 cities in China. A search summary said so, but the primary source was not retrieved.
  - AU-AIR: Aarhus, Denmark.
  - Okutama-Action: Okutama, Japan.
  - UAVid: China and Germany.
  - ManipalUAV: Manipal, India.
  - MOR-UAV: compiled from web/YouTube videos.
  - ERA (Event Recognition in Aerial videos): YouTube clips with event labels, not vehicle boxes.
  - DLR 3K / DLR-MVDA: Munich, Germany.
  - ITCVD: Enschede, Netherlands.
  - UAVDT: China.
  - highD/inD/rounD/exiD: Germany.
  - pNEUMA: Athens.
  - INTERACTION: has some US drone sites, but trajectories only.
  - UCF-ARG: UCF campus, Florida, mostly human actions.
  - VIVID (DARPA): Eglin AFB, FL, airborne EO/IR vehicle tracking.

  None of these were source-checked here.
- No 2022–2026 US-campus UAV vehicle detection dataset (images + boxes + per-frame GPS + open licence) was found in two targeted searches. This is a real gap, not proof that none exists.

## Which allow use by an open-source project (and publishing results)?

### Takeaway
None of the verified US aerial candidates is under a permissive licence that clearly allows commercial use:
- **SDD** is CC BY-NC-SA 3.0. An open-source, non-commercial project can use it and publish numbers, but cannot redistribute modified data except under the same licence.
- **VEDAI** is research and non-commercial only. Redistribution must carry the licence.
- **CARPK** is academic-only behind a EULA.
- **xView** is CC BY-NC-SA 4.0.
- **DOTA** is academic-only, with Google Earth terms on top.
- **COWC**: the repo is AGPL-3.0, and the data's terms were not found on the home page.
- **CitySim**: the repo says Apache-2.0, but data access is by request.
- **US-government (FHWA) TGSIM data** is the most permissive in principle: a federal work on data.gov. The exact licence still needs checking.

### Cited Findings
- **SDD:** "The datasets provided on this page are published under the Creative Commons Attribution-NonCommercial-ShareAlike 3.0 License" — [SDD home](https://cvgl.stanford.edu/projects/uav_data/)
- **VEDAI:**
  - "The base is given to the scientific community for research, and non-commercial purposes only, such as academic research, personal experimentation, and teaching. Any use for commercial and / or industrial purpose is forbidden."
  - "Permission is granted to use and copy freely the given data."
  - "The user also accept not to use or distribute the Base for a commercial usage (sells or licensing), or use the data to obtain any financial gain. The user agrees to have this license with all the copy of the data."
  - Publications must cite Razakarivony & Jurie, GREYC-2015-03-04 <hal-01122605>, and "mention in any publication if the base was altered".
  - Provider: SAGEM DEFENSE SECURITE and CNRS.

  All from the [VEDAI Terms of Use PDF](https://downloads.greyc.fr/vedai/TermsandConditionsofUseVeDAI2014.pdf)
- **CARPK / PUCPR+:** "made available for academic research purpose only", with a password-protected archive after an EULA form — [LPN page](https://lafi.github.io/LPN/)
- **COWC:** the GitHub repo is "AGPL-3.0". The home page carries no licence text — [LLNL/cowc](https://github.com/LLNL/cowc); [COWC home](https://gdo152.llnl.gov/cowc/)
- **xView:** "Attribution-Noncommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0) license" (Terms §5.1) — [xView terms](https://xviewdataset.org/terms.html)
- **DOTA:** "can be used for academic purposes only, but any commercial use is prohibited". "Use of the Google Earth images must respect the" Google Earth terms of use — [DOTA dataset page](https://captain-whu.github.io/DOTA/dataset.html)
- **CitySim:** the repository lists "Apache-2.0 license". For the collaboration (Asia) sites, "contact the relevant team to access the data" — [CitySim GitHub](https://github.com/UCF-SST-Lab/UCF-SST-CitySim1-Dataset)
- **UAV-OBB (non-US):** CC BY 4.0 — [PMC](https://pmc.ncbi.nlm.nih.gov/articles/PMC13092195/)
- **TGSIM:** published by USDOT/FHWA on data.gov and data.transportation.gov — [data.gov](https://catalog.data.gov/dataset/third-generation-simulation-data-tgsim-i-294-l2-trajectories)

### Inferences
- CamSim is open source and non-commercial as a project. Training or evaluating on SDD, VEDAI, xView or CARPK and publishing benchmark numbers in a paper or README is consistent with "research / non-commercial" wording.
- Three practices are not cleared by these licences:
  - committing the data, or derived crops, to the repo;
  - shipping it in a Docker image;
  - letting downstream commercial users rely on it.

  A download script plus published metrics is the safe pattern.
- VEDAI additionally requires the citation and a note if the data is altered. Also, "use the data to obtain any financial gain" would bar commercial ATR vendors from using a CamSim benchmark built on VEDAI.
- AGPL on COWC's repo likely covers the scripts. Whether the imagery inherits AGPL or source-imagery terms (Utah AGRC is public, the others are mixed) is unclear.
- US federal works (TGSIM, and Utah AGRC public orthophotos as source) are generally the most reusable. NAIP itself is public domain, so CamSim's own synthetic-vs-real comparison over NAIP has no licence issue on the imagery side.

### Gaps
- The COWC data licence (separate from the repo's AGPL) was not found on the home page. The `download/` directory README may hold it.
- The CitySim LICENSE text was not read, and it is unclear whether Apache-2.0 covers the data. The request form's terms are unknown.
- The licence string on the TGSIM data.gov records was not captured (US federal data is usually public domain / "U.S. Government Work").
- The HIGH-SIM licence and download URL were not retrieved.
- iSAID's own licence was not checked (it reuses DOTA images, so the academic-only terms likely carry over).

## Which carry per-image geolocation or georeferencing so the scene can be reconstructed?

### Takeaway
Explicit georeferencing in the retrieved sources:
- **CitySim:** positions in GPS coordinates.
- **TGSIM:** trajectories plus lane-centreline geometry at known highway sites.
- **WPAFB 2009 / AFRL WAMI:** historically comes with platform metadata. Unconfirmed this session.

VEDAI and COWC derive from georeferenced orthophotos (Utah AGRC 2012 HRO), but the released chips carry no coordinates. Recovering location would require matching against the source ortho. SDD has no geotags, but its 8 scenes are fixed, nameable spots on Stanford campus, so a scene package can be placed by hand. CARPK, DOTA and xView expose no coordinates (DOTA metadata is date, source and GSD only).

### Cited Findings
- **CitySim:** per-vehicle bounding box vertices, with positions "in pixels, feet, and GPS coordinates", plus 3D base maps and signal timings for digital twins — [arXiv 2208.11036](https://arxiv.org/pdf/2208.11036); [CitySim GitHub](https://github.com/UCF-SST-Lab/UCF-SST-CitySim1-Dataset)
- **TGSIM I-294:** a trajectory CSV with position, speed and acceleration, plus reference images for twelve runs. Lane centreline files flag on-ramps, off-ramps and weaving segments. The site is near Hinsdale, IL — [data.gov I-294 L2](https://catalog.data.gov/dataset/third-generation-simulation-data-tgsim-i-294-l2-trajectories)
- **DOTA:** metadata gives acquisition date, image source and GSD ("None" if unknown), with no coordinates — [DOTA dataset page](https://captain-whu.github.io/DOTA/dataset.html)
- **VEDAI:** images are "a modified version" of Utah AGRC 2012 HRO 6-inch colour orthophotography, and the download page doesn't mention georeferencing — [VEDAI terms](https://downloads.greyc.fr/vedai/TermsandConditionsofUseVeDAI2014.pdf); [VEDAI page](https://downloads.greyc.fr/vedai/)
- **COWC:** the home page says nothing on CRS or projection — [COWC home](https://gdo152.llnl.gov/cowc/)
- **SDD:** no GPS or georeferencing information on the page — [SDD home](https://cvgl.stanford.edu/projects/uav_data/)
- **UAV-OBB:** no per-image GPS mentioned (non-US anyway) — [PMC](https://pmc.ncbi.nlm.nih.gov/articles/PMC13092195/)
- **Songdo Traffic (non-US):** trajectories "in global geographic coordinates as well as local and orthophoto systems", an example of what a fully georeferenced drone set looks like — [arXiv 2411.02136](https://arxiv.org/html/2411.02136v2)

### Inferences
- For a CamSim "same place, real vs synthetic" test, **CitySim's US intersections** are the strongest candidate if UCF grants access. They have GPS, rotated boxes (matching CamSim's OBB output), drone altitude, and nadir video. Ask UCF for site coordinates and the camera altitude.
- **SDD** is the easiest open option. The scenes are recognisable campus spots (e.g. "deathCircle" is the roundabout by the Stanford bookstore and Clark Center area), so a scene package can be built from a hand-picked bbox. Ground-plane registration would come from a homography to NAIP, not from metadata.
- **VEDAI and COWC-Utah** could be geolocated by template-matching chips against the public Utah AGRC 2012 HRO mosaic. That is feasible because the source is public US ortho, but it is labour.
- Satellite xView and mixed-source DOTA are weak fits: no coordinates, and xView is satellite.

### Gaps
- The CitySim US site coordinates and drone altitude are unknown until access is granted.
- The SDD per-scene pixel-to-metre scale and homography are not supplied by the official page. Some trajectory-prediction repos publish per-scene scale factors, but these were not checked.
- WPAFB 2009 / CLIF metadata (INS pose per frame) was not verified this session.
- Whether TGSIM reference images or raw 8K video can be obtained was not determined.
