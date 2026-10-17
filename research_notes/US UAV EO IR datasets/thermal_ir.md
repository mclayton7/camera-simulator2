# Aerial / UAV thermal IR (LWIR/MWIR) and paired RGB-T datasets with vehicles, people or vessels: US coverage check (as of Oct 2026)

Scope: real (non-synthetic) aerial or UAV thermal datasets that could serve as a fixed real test set for detectors trained on CamSim synthetic IR. A US location is strongly preferred, because the matching scene package is built from USGS 3DEP + NAIP. Research ran 2026-10-15. Where a fact could not be verified from a primary page, it goes under Gaps or Inferences, not Cited Findings.

## Q1. Is there any openly licensed aerial thermal dataset over the US with labelled vehicles? If not, what is the closest?

### Takeaway
No public, open-licence US aerial thermal dataset with vehicle *detection boxes* turned up. The closest is the **Caltech Aerial RGB-Thermal dataset (CART)**. It was flown by UAV over California, Kentucky and North Carolina, and its semantic segmentation masks include a "Vehicles" class (boats, cars) and a "Person" class. It has limits:
- Vehicles are rare.
- The thermal camera is non-radiometric.
- Altitude is mostly about 40 m.
- The licence is non-commercial: CaltechDATA says CC BY-NC 4.0, while the GitHub README says "non-commercial only" + CC BY-SA 4.0, a conflict.

Other US aerial thermal sets with labels are:
- **WiSARD** (Washington State): people only, MIT-style licence.
- **FLAME 3** (AZ/OR/FL): radiometric, fire labels only.
- **Boson-nighttime** (US desert, geo-localization): no object labels.
- **NOAA KAMERA Arctic seals** (Alaska, manned aircraft): seals only.

The best vehicle-labelled aerial thermal sets are all outside the US:
- **HIT-UAV** (China): CC BY 4.0 per the repo; has per-image altitude and angle.
- **DroneVehicle** (China): RGB+IR pairs with oriented boxes.
- **MONET** (rural, country not confirmed): people + vehicles, per-frame GPS/attitude.

US MWIR vehicle data exists, but it is ground-to-ground and government-controlled:
- **DSIAC ATR Algorithm Development Image Database** (NVESD, 1–5 km ranges). It is listed as Distribution A on one DSIAC page; a survey says it is NATO-only.
- **Eglin Signature Data Center** data: CUI or classified.

### Cited Findings

**Caltech Aerial RGB-Thermal Dataset in the Wild (CART), Lee et al., ECCV 2024. US: yes**
- Captured "37 aerial and ground trajectories covering lakes, rivers, coastlines, deserts, and mountains from around Southern California, Kentucky, and North Carolina." Of these, 18 are aerial trajectories that "mostly depict scenes from 40 m altitude"; 19 are ground-level, captured on foot. — [arXiv 2403.08997 PDF, §3.2](https://arxiv.org/pdf/2403.08997)
- Named sites: Kentucky River (KY), Colorado River (CA), Castaic Lake (CA), Duck (NC), Big Bear Lake (CA), Arroyo Seco (CA), Idyllwild (CA), Joshua Tree (CA), North Field at Caltech (CA). CaltechDATA also lists a "Beach" flight (2021-07-21). — [arXiv HTML v2 supplement](https://arxiv.org/html/2403.08997v2); [CaltechDATA record cks6g-ps927](https://data.caltech.edu/records/cks6g-ps927)
- The supplement's comparison table gives camera pose as "< 120 m" with 20° and 45° camera angles, i.e. oblique. — [arXiv HTML v2](https://arxiv.org/html/2403.08997v2)
- Hardware:
  - Thermal: "a non-radiometric FLIR ADK thermal camera (640×512 px and 75° horizontal FoV, 60 Hz)". The ADK is a Boson-based LWIR automotive camera; the paper's figure labels it "FLIR Boson (Thermal)".
  - EO: a pair of FLIR Blackfly monochrome and colour cameras (960×600, 75° HFoV, 30 Hz).
  - Pose: VectorNav VN100 IMU (200 Hz) and u-blox M8N GPS (5 Hz).
  - Cameras and IMU are hardware-synchronised. The stack is mounted on an Aurelia X6 hexacopter. — [arXiv PDF §3.1](https://arxiv.org/pdf/2403.08997)
- Thermal is 16-bit raw (non-radiometric). The baselines normalise by 1st–99th percentile rescaling plus CLAHE. The repo ships `thermal8` and `thermal16`. — [arXiv PDF §3.2](https://arxiv.org/pdf/2403.08997); [GitHub](https://github.com/aerorobotics/caltech-aerial-rgbt-dataset)
- Labels are semantic segmentation masks, 10 classes plus unknown and background:

  | Class | Notes |
  |---|---|
  | bare ground | |
  | rocky terrain | |
  | human-made structures | |
  | road | |
  | shrubs | |
  | trees | |
  | sky | |
  | water | |
  | vehicles | "boats, cars" |
  | person | |

  The paper says vehicle and person are "rare object classes" and leaves them out of the comparison metrics. — [GitHub](https://github.com/aerorobotics/caltech-aerial-rgbt-dataset); [arXiv PDF](https://arxiv.org/pdf/2403.08997)
- Time of day: sunrise, day and night/twilight splits exist, and the twilight and night sets are used for testing. — [GitHub](https://github.com/aerorobotics/caltech-aerial-rgbt-dataset); [arXiv PDF](https://arxiv.org/pdf/2403.08997)
- Size:
  - Labeled archives: `labeled_rgbt_pairs.zip` 4.3 GB and `labeled_thermal_singles.zip` 4.1 GB.
  - Total raw data: 16.2 TB, as ROS1 bags, .mp4 and Ardupilot logs.
  - The supplement lists 4,195 samples.
  - A third-party paper reports only 223 paired samples with vehicle boxes after matching and manual correction. — [CaltechDATA](https://data.caltech.edu/records/cks6g-ps927); [arXiv HTML v2](https://arxiv.org/html/2403.08997v2); [search snippet of arXiv 2609.02556 / related](https://arxiv.org/pdf/2609.02556)
- Metadata: synchronised GPS, IMU, timestamps and Kalibr calibration YAMLs in rosbags, plus raw Ardupilot logs. The README marks "preferred position data" and sync data as "COMING SOON!". — [GitHub](https://github.com/aerorobotics/caltech-aerial-rgbt-dataset)
- Licence (conflict):
  - CaltechDATA: "Creative Commons Attribution Non Commercial 4.0 International (CC BY-NC 4.0)". — [CaltechDATA](https://data.caltech.edu/records/cks6g-ps927)
  - GitHub README: "The dataset is for non-commercial use only" and "licensed under the terms of the Creative Commons Attribution ShareAlike 4.0 license". — [GitHub](https://github.com/aerorobotics/caltech-aerial-rgbt-dataset)
- Availability: published 2024-03-13 (v1.0.0). The labeled zips download directly from data.caltech.edu. Raw data comes from an `onr-processed` S3 bucket. Funded by ONR. — [CaltechDATA](https://data.caltech.edu/records/cks6g-ps927); [GitHub](https://github.com/aerorobotics/caltech-aerial-rgbt-dataset)

**WiSARD (Univ. of Washington, IROS 2022). US: yes (Washington State)**
- "Various wilderness environments in Washington, USA". The images are from a drone; the thermal band is described as "Long Wave Infrared". — [WiSARD site](https://sites.google.com/uw.edu/wisard/)
- Size: 26,862 labeled visual, 29,989 labeled thermal and 15,453 synchronised visual-thermal pairs (WiSARDv1, 40.54 GB, Google Drive). — [WiSARD site](https://sites.google.com/uw.edu/wisard/)
- Labels: humans (lost-person search). No vehicle class is stated. — [WiSARD site](https://sites.google.com/uw.edu/wisard/); [arXiv 2309.04453](https://arxiv.org/pdf/2309.04453)
- Licence: the page carries an MIT-style text, "Copyright 2022 Daniel Broyles, Christopher Hayner, Karen Leung". It permits use, copying, modification, merging, publishing, distribution, sublicensing and sale, with the notice retained, provided "AS IS". This is the most permissive US aerial thermal set found. — [WiSARD site](https://sites.google.com/uw.edu/wisard/)

**FLAME 3 (Hopkins et al., arXiv 2412.02831, Dec 2024). US: yes. Radiometric. Fire only**
- Six prescribed burns:

  | Burn | Location | Date | Drone / camera |
  |---|---|---|---|
  | Hanna Hammock | Tall Timbers, FL | Feb 2023 | Autel EVO II Dual 640T (XT709) |
  | Hundred | Tonto NF, AZ | Apr 2023 | Autel EVO II Dual 640T (XT709) |
  | Shoetank | San Carlos Apache, AZ | Nov 2022 | DJI Mavic 2 Enterprise Advanced |
  | Sycan Marsh | OR | Oct 2022 | DJI Matrice 30T |
  | Willamette Valley / Willow Creek | OR (inferred from the name) | Sep–Oct 2022 | DJI Mavic 2 Enterprise Advanced |
  | Wildbill | Coconino NF, AZ | May/Jun 2023 | Autel EVO II Dual 640T (XT709) |

  — [arXiv HTML](https://arxiv.org/html/2412.02831v1)
- Thermal: "Radiometric long-wave infrared (LWIR)" at 640×512. Radiometric TIFFs are single-band temperature rasters ("per-pixel temperature estimates"). Imagery is paired RGB/IR. — [arXiv HTML](https://arxiv.org/html/2412.02831v1); [arXiv abs](https://arxiv.org/abs/2412.02831v1)
- Altitude: oblique pre-burn imagery at 61–122 m AGL; nadir thermal plots at 122 m (400 ft). — [arXiv HTML](https://arxiv.org/html/2412.02831v1)
- Labels: Fire / No-Fire image labels (thermal thresholds, then manual review). There are no vehicle or person labels. The Kaggle CV subset has 622 Fire and 116 No-Fire "quartets" (raw RGB, raw thermal, FOV-corrected RGB, thermal TIFF). — [arXiv HTML](https://arxiv.org/html/2412.02831v1); [IEEE DataPort](https://ieee-dataport.org/node/14367)
- Availability: single-burn subset and nadir plots on Kaggle; full six burns "on request". The dataset licence is not stated on arXiv, and the IEEE DataPort licence field showed blank. — [arXiv HTML](https://arxiv.org/html/2412.02831v1); [IEEE DataPort](https://ieee-dataport.org/node/14367)
- Earlier sets, which FLAME 3 lists: FLAME 1 (2020; 47,992 frames; RGB and thermal not paired) and FLAME 2 (53,451 images; RGB/IR pairs), both from prescribed or pile burns. — [arXiv HTML](https://arxiv.org/html/2412.02831v1)

**Boson-nighttime (Xiao et al., NYU ARPL, 2023). US desert (state not confirmed). No object labels**
- FLIR Boson thermal from a UAV: six night flights, 33 km² of mostly desert with some farms, roads and buildings.
- Paired with Bing/Maxar satellite tiles: 10,256 train, 13,011 val and 26,568 test pairs, plus about 160K unpaired satellite images. The STHN repo adds synthetic thermal.
- The images are geo-registered to satellite tiles, so per-frame geolocation is implicit. — [arXiv 2306.02994](https://arxiv.org/pdf/2306.02994); [STHN GitHub](https://github.com/arplaboratory/STHN)
- A 2026 paper calls it the near-exclusive benchmark for thermal geo-localization and criticises its desert-only, night-only content. — [arXiv 2604.03120](https://arxiv.org/html/2604.03120v1)

**NOAA KAMERA Arctic seals (Alaska, manned aircraft). US: yes. Animals only**
- 2019 KAMERA test flights 4–7. Contents: imagery, footprints, ice seal detections and homography files. Built "for detecting ice seals and classifying ice seal species using IR and RGB imagery". Distributed via LILA (lila.science/datasets/noaa-arctic-seals-2019/). — [NOAA InPort 63322](https://fisheries.noaa.gov/inport/item/63322); [InPort 63329](https://www.fisheries.noaa.gov/inport/item/63329)
- One data.gov copy of the record says "non-public / No License Provided", which is a conflict. — [catalog.data.gov](https://catalog.data.gov/dataset/a-dataset-for-machine-learning-algorithm-development2/resource/d8070ef8-d0d5-4cd3-ae44-66a5be9db164)
- KAMERA has three colour, three UV and three IR cameras in the aircraft belly. The 2021 Beaufort Sea survey gave about 900k image triplets. — [Kitware](https://kitware.com/project-spotlight-noaa-kamera); [arXiv 2509.19129](https://arxiv.org/pdf/2509.19129)

**MassMIND (UMass Lowell, 2022). US: yes. Not aerial**
- About 2,900 LWIR images from a USV in Massachusetts Bay, Charles River and Boston Harbor. Instance and semantic segmentation into 7 classes (including vessels). The GitHub licence is tagged "other". — [DeepAI/arXiv 2209.04097](https://deepai.org/publication/massmind-massachusetts-maritime-infrared-dataset); [science.ecosyste.ms](https://science.ecosyste.ms/projects/28806)

**Teledyne FLIR ADAS v2. US: yes (Santa Barbara, CA). Ground vehicle, not aerial**
- Vehicle-mounted on Santa Barbara-area streets and highways. — [DSIAC IR datasets](https://dsiac.dtic.mil/technical-inquiries/notable/infrared-imagery-datasets)
- 26,442 annotated frames, about 520k boxes, 15 categories. Includes 16-bit pre-AGC frames. — [FLIR dataset page](https://oem.flir.com/solutions/automotive/adas-dataset-form/)
- Licence: gated by a "Non-Commercial Image Dataset License Agreement" (research and non-commercial ML development; FLIR keeps ownership). — [flir.ca agreement](https://flir.ca/oem/adas/adas-dataset-agree)
- No Teledyne FLIR aerial or drone dataset was found. Searches surfaced only the ADAS set. — [search: flir.com/adasdataset family](https://www.flir.com/adasdataset)

**US government MWIR (ground-based or controlled)**
- **DSIAC ATR Algorithm Development Image Database** (US Army NVESD):
  - Visible + MWIR, collected in 2006.
  - About 10 targets (pickup, SUV, D20, military vehicles) plus people, at 1–5 km ranges, 1,800 frames per sequence, 185 GB.
  - DSIAC lists it as "Distribution Statement A. Approved for public release". A 2024 survey says it is only available to researchers in NATO member countries; this conflicts.
  - Ground-to-ground oblique, not UAV. — [DSIAC IR datasets](https://dsiac.dtic.mil/technical-inquiries/notable/infrared-imagery-datasets); [dsiac.org](https://dsiac.org/?p=22005); [arXiv 2507.11267](https://arxiv.org/pdf/2507.11267); [arXiv 2305.13886](https://arxiv.org/pdf/2305.13886)
- **Eglin Signature Data Center** (Eglin AFB, FL): MWIR and other bands from ground, tower and airborne platforms; tactical vehicles, watercraft and personnel. Data is CUI to classified and needs a US Government sponsor. — [DSIAC IR datasets](https://dsiac.dtic.mil/technical-inquiries/notable/infrared-imagery-datasets)

**Non-US aerial thermal with vehicles or people**
- **HIT-UAV** (Suo et al., Sci. Data 2023):
  - Platform: DJI Matrice M210 V2 + Zenmuse XT2 (FLIR LWIR), 640×512, **8-bit**.
  - 2,898 images from 43,470 frames, day and night. Scenes: schools, parking lots, roads, playgrounds.
  - Classes: Person, Car, Bicycle, OtherVehicle, DontCare; 24,899 objects; HBB + OBB.
  - Per image: altitude (60–130 m), camera angle (30–90°), date and day/night, encoded in the filename.
  - Hosted on Zenodo, GitHub and pegasus.ac.cn. The repo links a "CC-BY-4.0 license".
  - Capture country is not stated on the pages read. Authors are from Yunnan University, CAS ICT and U. Delaware. — [PMC](https://pmc.ncbi.nlm.nih.gov/articles/PMC10119175/); [GitHub](https://github.com/suojiashun/HIT-UAV-Infrared-Thermal-Dataset)
- **DroneVehicle** (VisDrone/Tianjin):
  - 28,439 RGB-IR pairs (56,878 images); car, truck, bus, van, freight car; oriented boxes.
  - Urban roads, residential areas and parking lots, day to night.
  - The repo page states no licence. — [GitHub](https://github.com/VisDrone/DroneVehicle); [arXiv survey 2110.12638](https://arxiv.org/pdf/2110.12638)
- **MONET** (CVPRW 2023):
  - Drone thermal over two rural sites; about 53K images and about 162K boxes; classes people and vehicles.
  - Per-frame timestamp-aligned attitude, speed, altitude and GPS.
  - The arXiv licence link is CC BY 4.0 (it may cover only the paper). — [arXiv 2304.05417](https://arxiv.org/abs/2304.05417); [CVF](https://openaccess.thecvf.com/content/CVPR2023W/MULA/papers/Riz_The_MONET_Dataset_Multimodal_Drone_Thermal_Dataset_Recorded_in_Rural_CVPRW_2023_paper.pdf)
- **BIRDSAI** (WACV 2020):
  - LWIR at night from a fixed-wing UAV in "multiple African protected areas" (Southern Africa).
  - Real: 48 videos, about 62k images. Synthetic: 124 AirSim videos, about 100k images.
  - Classes: animals vs humans, plus species.
  - Licence: "Community Data License Agreement (permissive variant)" (CDLA-Permissive-1.0), hosted on LILA (GCS/S3/Azure). — [LILA](https://lila.science/datasets/conservationdrones); [WACV](https://openaccess.thecvf.com/content_WACV_2020/html/Bondi_BIRDSAI_A_Dataset_for_Detection_and_Tracking_in_Aerial_Thermal_WACV_2020_paper.html)
- **LLVIP**: about 30,976 visible-IR images for pedestrian detection. Licence: "freely available to academic and non-academic entities for non-commercial purposes", under a separate Term of Use. — [GitHub](https://github.com/bupt-ai-cz/LLVIP)
- **RGBTDronePerson**: 6,125 drone RGB-T pairs; person, rider and crowd; tiny objects (about 11.7 px); annotated on thermal only. — [arXiv 2502.09311](https://arxiv.org/pdf/2502.09311)
- **NII-CU MAPD**: only a leaderboard entry was found, tied to "Deep learning with RGB and thermal images onboard a drone for monitoring operations". — [HyperAI](https://hyper.ai/en/sota/tasks/object-detection/benchmark/object-detection-on-nii-cu-mapd)
- **2025 Scientific Data RGB-T UAV sets** with vehicles:
  - **M3OT**: two drones at 100–120 m; RGB + IR vehicle tracking. — [Nature](https://www.nature.com/articles/s41597-025-06204-0)
  - **Kust4K**: RGB-TIR urban traffic segmentation; car, truck, motorcycle among 8 classes. — [Nature](https://www.nature.com/articles/s41597-025-05994-7)
  - Locations were not verified. Nature pages redirect to auth.
- A 2026 SPIE paper on RGB-to-IR translation for UAV vehicle detection uses Kust4K and VTUAV as unseen target domains. It names no US-captured aerial IR vehicle dataset. — [arXiv 2609.02556](https://arxiv.org/abs/2609.02556)

### Inferences
- For a US test set with vehicles, CART is the only candidate. Its vehicle content is a small number of segmented boats and cars at about 40 m, and it lacks COCO boxes. Boxes could be derived from the masks (connected components). The licence is NC in both statements (BY-NC on CaltechDATA, BY-SA + "non-commercial only" on GitHub). Internal research or evaluation use is fine; redistributing derived data inside an open-source repo is questionable, so fetch it at test time rather than committing it.
- The realistic plan for a vehicle-detector test is probably **HIT-UAV** for altitude and angle coverage (60–130 m, 30–90°, day/night, per-image altitude and angle, CC BY 4.0). A US scene package cannot match it because the location is not given and is almost certainly China. For US geo-registered matching, use **CART** or **WiSARD** for people, and **FLAME 3** for radiometric checks of terrain and fire temperatures.
- Because FLAME 3 is radiometric and US, it is the best real-data sanity check for CamSim's absolute LWIR temperatures over real US terrain: AZ/OR/FL pine and grass at 61–122 m, with DJI and Autel metadata in the EXIF. It has no vehicles.
- Only US government data is MWIR (DSIAC ATR, Eglin). Every open UAV dataset found is LWIR, from FLIR Boson/Tau-based cores: Zenmuse XT2, ADK, Mavic 2 EA, M30T and Autel 640T.

### Gaps
- No 2023–2026 public US UAV thermal dataset with vehicle boxes was found, despite targeted searches (Texas A&M, Clemson, UCF, UMass, state DOT). Absence is not proof; university lab releases may exist that are not indexed.
- CART licence conflict (CC BY-NC 4.0 vs "CC BY-SA 4.0, non-commercial only"). The repo LICENSE file was not read.
- FLAME 3 Kaggle licence not confirmed. FLAME 1 and 2 locations and cameras were not verified in this pass. From memory: Northern Arizona, Flagstaff area; FLAME 1 used a Zenmuse X4S RGB with a FLIR Vue Pro R. Unverified.
- MONET capture country not stated on the arXiv abstract. It is believed to be Italy (FBK authors), unverified.
- Not verified in this pass, from memory only:
  - VTUAV: RGB-T tracking from a DJI drone, China (Dalian), CC licence unknown.
  - DroneVehicle and RGBTDronePerson: China.
  - NII-CU MAPD: Japan, NII + Chiba University.
  - TarDAL/M3FD: China, mostly ground binocular.
  - PST900: UPenn ground robot.
  - KAIST: Korea, ground. Exclude.
- WiSARD camera model, altitude and per-frame GPS were not stated on its site.
- Boson-nighttime exact state and licence not confirmed. It is collected by NYU ARPL with Teledyne FLIR involvement per the dataset name only.
- The NOAA Arctic Seals 2019 LILA licence was not confirmed. LILA datasets are usually CDLA-Permissive, but data.gov shows "No License Provided".
- No US aerial MWIR open dataset was found at all.

## Q2. Which are radiometric (temperatures) and which are AGC'd 8-bit?

### Takeaway
Only **FLAME 3** is confirmed radiometric (per-pixel temperature TIFFs). Two sets are 16-bit raw but **non-radiometric**: **CART** (FLIR ADK, 16-bit raw counts) and **FLIR ADAS** (16-bit pre-AGC frames, ground). **HIT-UAV** is 8-bit. The other sets (DroneVehicle, LLVIP, RGBTDronePerson, BIRDSAI, WiSARD) are distributed as display images (JPEG/PNG), with no radiometric claim found.

### Cited Findings
- FLAME 3: "radiometric thermal TIFFs", "per-pixel temperature estimates"; LWIR 640×512; single-band temperature rasters. — [arXiv abs](https://arxiv.org/abs/2412.02831v1); [arXiv HTML](https://arxiv.org/html/2412.02831v1)
- CART: "non-radiometric FLIR ADK thermal camera"; 16-bit data normalised by percentiles + CLAHE; the supplement says "our thermal camera was not radiometric". — [arXiv PDF](https://arxiv.org/pdf/2403.08997)
- FLIR ADAS: the spec lists 16-bit pre-AGC thermal frames. — [FLIR dataset page](https://oem.flir.com/solutions/automotive/adas-dataset-form/)
- HIT-UAV: bit depth 8. — [PMC](https://pmc.ncbi.nlm.nih.gov/articles/PMC10119175/)

### Inferences
- Comparing CamSim against real data in absolute temperature needs FLAME 3. For a detector test, 8-bit AGC'd imagery (HIT-UAV, DroneVehicle) matches what CamSim's H.264 stream delivers after its IR AGC, so that limitation is acceptable.
- CART's 16-bit raw counts allow re-applying CamSim's own AGC to the real data, which helps put the two domains on equal footing.

### Gaps
- No radiometric metadata (R-JPEG) was confirmed for WiSARD, BIRDSAI, DroneVehicle or RGBTDronePerson.
- Whether the FLAME 1 and 2 thermal JPEGs are R-JPEG was not verified.

## Q3. Which have per-frame geolocation / pose metadata?

### Takeaway
- **Full per-frame GPS + IMU**: CART (GPS 5 Hz, VN100 IMU 200 Hz, hardware-synced, rosbags + Ardupilot logs) and MONET (per-frame GPS, attitude, altitude, speed).
- **Per-image altitude and camera angle only, no GPS**: HIT-UAV.
- **Geo-registered to satellite** (pose implicit): Boson-nighttime.
- **Footprints + homographies**: NOAA KAMERA.
- **Not stated**: WiSARD, DroneVehicle, BIRDSAI, LLVIP, RGBTDronePerson.
- **FLAME 3**: per-image drone metadata likely in EXIF (not confirmed); nadir plots are georeferenced products.

### Cited Findings
- CART: synchronised RGB, thermal, GPS and IMU in ROS1 bags; raw Ardupilot logs; Kalibr calibration. — [GitHub](https://github.com/aerorobotics/caltech-aerial-rgbt-dataset); [arXiv PDF](https://arxiv.org/pdf/2403.08997)
- MONET: "each image is timestamp-aligned with drone data" covering attitude, speed, altitude and GPS. — [arXiv 2304.05417](https://arxiv.org/abs/2304.05417)
- HIT-UAV: flight altitude, camera perspective, date and daylight per image, in filenames. — [PMC](https://pmc.ncbi.nlm.nih.gov/articles/PMC10119175/)
- NOAA KAMERA 2019: imagery footprints and homography files. — [NOAA InPort](https://fisheries.noaa.gov/inport/item/63322)
- Boson-nighttime: thermal paired to satellite tiles. — [arXiv 2306.02994](https://arxiv.org/pdf/2306.02994)

### Inferences
- CART is the only US set where a CamSim scene package can be built for the exact flight path and the camera replayed with CIGI from logged GPS and IMU. That allows a like-for-like synthetic-vs-real pair at the same pose, at night and at sunrise, over a river, lake, coast or desert.

### Gaps
- The CART README marks "preferred position data" as "COMING SOON!". The status of that item in Oct 2026 was not checked.
- Per-frame GPS in WiSARD and FLAME 3 EXIF was not confirmed.
