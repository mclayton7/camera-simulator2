# Aerial/UAV maritime vessel datasets and recent (2024–2026) US aerial EO/IR datasets with telemetry

Scope note: about 22 search/fetch calls. Several primary pages (Roboflow Universe, MDPI, Nature) returned 403 or redirects, so some licences are still unverified. Anything below that comes from prior knowledge rather than a source fetched in this session is labelled as such and kept in Inferences or Gaps.

## Q1. Is there an openly licensed UAV vessel dataset over US waters?

### Takeaway
I found no openly licensed UAV vessel dataset that is verified to be over US waters and has per-frame telemetry. The best licensed UAV maritime set is SeaDronesSee, which is CC0 and has rich per-frame GPS, altitude and gimbal metadata. It was flown over Lake Constance in Germany, not US waters. The only US-adjacent maritime IR set found, MassMIND (Boston Harbor), was shot from a USV, not a UAV, and is non-commercial (CC BY-NC-SA 4.0).

### Cited findings

**SeaDronesSee (Univ. Tübingen)**
- Licence, quoted verbatim from the repo README: "Code is MIT license, dataset is Creative Commons CC0 1.0 Universal (CC0 1.0)." and "Essentially, do what you want!" Test labels for OD, SOT and MOT are withheld; you evaluate by uploading to their server. — [SeaDronesSee README](https://raw.githubusercontent.com/Ben93kie/SeaDronesSee/main/README.md)
- Splits:
  - Object Detection v2: 8,930 train, 1,547 val, 3,750 test.
  - Object Detection v1: 5,630 / 859 / 1,796.
  - SOT: 58 / 70 / 80 clips.
  - MOT: 22 clips, 54,105 frames.
  - Multispectral OD: 246 / 61 / 125 images, with no challenge or leaderboard.
  - Boat-MNIST: 3,765 / 1,506 / 2,259.
  - Source: [SeaDronesSee README](https://raw.githubusercontent.com/Ben93kie/SeaDronesSee/main/README.md)
- Per-frame metadata in the MOT annotations: date_time, gps_latitude/longitude (+ref), altitude, gimbal_pitch, compass_heading, gimbal_heading, speed and x/y/z speed. The example frame is 3840×2160 and sits at about 47.67 N, 9.27 E. The folder is named `lake_constance_v2021_tracking`, so the location is **Lake Constance, Germany**. — [SeaDronesSee dataset page](https://seadronessee.cs.uni-tuebingen.de/dataset); [README](https://raw.githubusercontent.com/Ben93kie/SeaDronesSee/main/README.md)
- OD v2 coverage:
  - 14,227 RGB images, captured at altitudes of 5–260 m and gimbal pitch 0–90°.
  - Altitude and viewing-angle metadata on most frames: 2,830 images have no metadata and 686 have pitch only.
  - Source: [Altitude-aware tiling paper, arXiv 2511.19728](https://arxiv.org/pdf/2511.19728)
- Classes on the community copies: boat, buoy, jet-ski, life-saving appliance, swimmer. — [Roboflow seadrone copies](https://universe.roboflow.com/seadrone/val_732)
- The multispectral subset is described as "several hundred frames captured from the viewpoint of a UAV showing humans and boats". The site also hosts Synthetic SeaDronesSee and MODS. — [SeaDronesSee dataset page](https://seadronessee.cs.uni-tuebingen.de/dataset)
- Still downloadable in 2026: the page is live, and MaCVi 2026 (CVPR 2026) is running its 4th workshop. — [MaCVi 2026 overview, arXiv 2604.13244](https://arxiv.org/abs/2604.13244)

**MassMIND (UMass Lowell)**
- LWIR images from the Boston Harbor area, Massachusetts, USA. Over 2,900 images, instance-segmented into 7 classes: sky, water, obstacle, living obstacle, bridge, self, background. Published at IJRR 2023 (arXiv 2209.04097). — [MassMIND, DeepAI summary](https://deepai.org/publication/massmind-massachusetts-maritime-infrared-dataset)
- Licence: the GitHub LICENSE file is **CC BY-NC-SA 4.0** (Attribution-NonCommercial-ShareAlike 4.0 International). It opens: "By exercising the Licensed Rights (defined below), You accept and agree to be bound by the terms and conditions of this…" — [MassMIND LICENSE](https://raw.githubusercontent.com/uml-marine-robotics/MassMIND/main/LICENSE)
- Platform is a USV (surface vessel) at near-water viewpoint, and the labels are segmentation, not vessel boxes. — [DeepAI summary](https://deepai.org/publication/massmind-massachusetts-maritime-infrared-dataset)

**Roboflow "Aerial Maritime Drone Dataset"**
- MIT licence. 74 images from a DJI Mavic Air 2 at 400 ft, 1,151 boxes, classes docks, boats, lifts, jetskis, cars. Collected and annotated by the Roboflow team. — [Roboflow public page, via search snippet](https://public.roboflow.com/object-detection/aerial-maritime); [Kaggle mirror](https://www.kaggle.com/datasets/ammarnassanalhajali/aerial-maritime)
- The page now redirects to universe.roboflow.com/jacob-solawetz/aerial-maritime, which returned 403, so I could not verify the location.

**AFO (Aerial dataset of Floating Objects)**
- Licence is CC BY-NC-SA 3.0 on the HF/FiftyOne card, and CC BY-NC-SA 3.0 IGO on Dataset Ninja. The two sources differ on the variant, but both are non-commercial. Roboflow re-uploads claim CC BY 4.0, which is probably not authoritative. — [Voxel51 AFO card](https://docs.voxel51.com/dataset_zoo/datasets_hf/afo_aerial_floating_objects.html); [Dataset Ninja AFO](https://datasetninja.com/afo)

**MOBDrone (CNR-ISTI, Italy)**
- Over 125K drone-view frames in a marine environment, about 113K person annotations, one UAV at 10–60 m above mean sea level. Hosted on Zenodo. I found no licence text. — [MOBDrone, arXiv 2203.07973](https://arxiv.org/pdf/2203.07973); [CNR page](https://aimh.isti.cnr.it/?p=4025)

**MaCVi 2026 thermal collision-avoidance set**
- Electro-optical/thermal imagery from German, British and Dutch waters, 2023–2025. 704 train, 173 val, 381 test images. Not US. — [MaCVi 2026 overview](https://arxiv.org/pdf/2604.13244), via search snippet

**Other UAV vessel sets found (location not confirmed)**
- Marship-OBB9 / YOLO-UAVShip (2025): 11,268 UAV images, 18,632 OBB instances, 9 ship classes. — [Remote Sensing 17(17):3119](https://www.mdpi.com/2072-4292/17/17/3119)
- VESSELimg (2024): UAV port surveillance vessel set. — [ResearchGate](https://www.researchgate.net/publication/381568503_VESSELimg_A_Large_UAV-Based_Vessel_Image_Dataset_for_Port_Surveillance)
- "Aerial Maritime Vessel Detection and Identification". — [arXiv 2507.07153](https://arxiv.org/pdf/2507.07153)
- Thermal vessel re-ID. — [arXiv 2406.08294](https://arxiv.org/pdf/2406.08294)
- Roboflow "Thermal ships" (CC BY 4.0, unclear provenance). — [Roboflow](https://universe.roboflow.com/ali-pxypy/thermal-ships-moctp)
- Syn2Real aerial ship detection (sim-to-real for ships). — [Applied Sciences 14(11):4558](https://www.mdpi.com/2076-3417/14/11/4558)

**Tri-band (VIS/SWIR/LWIR) Vehicle and Vessel Dataset (Scientific Data, 2025)**
- Zhao Gangnian et al. Each image set records date, location, lighting and weather. — [Scientific Data s41597-025-04945-6](https://www.nature.com/articles/s41597-025-04945-6); [ASREN record](https://asren-aggregator.asren.net/Record/Manara_7342406363f98016bd0f3fa33c6d114a)
- The Nature page redirected to a login, so I could not verify the location or platform. The authors' names suggest China, but that is unverified.

**Airbus Ship Detection (Kaggle, 2018)**
- Satellite imagery, not UAV. I could not fetch the licence; use appears to be governed by the Kaggle competition rules. — [IEEE SPS note](https://signalprocessingsociety.org/our-story/news-updates/2018/10/airbus-ship-detection-challenge); [HF mirror gating text](https://huggingface.co/datasets/datadrivenscience/ship-detection)

### Inferences
- For a US-coast vessel test set, SeaDronesSee is the licence-clean, metadata-rich template, but its domain is a lake in Germany. It is good for checking the altitude/pitch-stratified evaluation protocol, not US geography.
- None of the licence-clean candidates is US-coastal UAV. A small set labelled in-house (for example Camp Pendleton coast drone flights logging DJI SRT/flight-record telemetry) is probably needed for an exact geographic match.
- The following come from prior knowledge, not fetched this session, so verify before relying on them:
  - MODS / MaSTr1325 are USV datasets from the Slovenian coast (Koper).
  - Singapore Maritime Dataset is shore- and ship-mounted, Singapore.
  - HRSC2016 is Google Earth imagery of harbours (non-US mostly).
  - ShipRSImageNet is satellite/Google Earth imagery.
  - ABOships is from a ferry-mounted camera in Turku, Finland.
  - None of these is UAV over US waters.

### Gaps
- Licence text for: Airbus Ship Detection (Kaggle rules), ShipRSImageNet, HRSC2016, MODS/MaSTr1325, Singapore Maritime Dataset, ABOships, MOBDrone (Zenodo), "VesselID". Not retrieved.
- Capture location of the Roboflow Aerial Maritime set (the page returned 403).
- I found no NOAA, USCG or Navy public release of labelled aerial ship imagery. NOAA has S-MODE LWIR airborne ocean data ([data.gov](https://catalog.data.gov/dataset/s-mode-mass-level-1-lwir-version-1)), but it is oceanographic and unlabelled for vessels.
- I found no labelled UAV vessel datasets from NPS Monterey, WHOI, Scripps or UCSD.

## Q2. Which recent sim-to-real aerial detection papers used which real test sets?

### Takeaway
There is no single community yardstick. Papers mostly test on their own small real sets or on DOTA, VisDrone or VEDAI-class benchmarks. The most relevant US-built hybrid is the Army-sponsored **Archangel** set: real UAV images at 15–50 m with per-image altitude, radius and viewing angle, paired with a matched Unity synthetic subset. Its targets are humans, not vehicles.

### Cited findings
- **Archangel (UMD + DEVCOM ARL, sponsored by DTRA/ARL)**
  - Hybrid real + synthetic UAV benchmark "with position and pose metadata".
  - Real subset: altitude and rotation radius 15–50 m in 5 m steps, people standing, kneeling or prone, with camera position and viewing angle per image.
  - Synthetic subset: Unity, 5–80 m, 4 sun angles.
  - No licence found.
  - Source: [arXiv 2209.00128](https://arxiv.org/pdf/2209.00128); [SPIE 12113](https://nanophotonics.spiedigitallibrary.org/conference-proceedings-of-spie/12113/121131T/Archangel-dataset--UAV-based-imagery-with-position-and-pose/10.1117/12.2618636.full)
  - Follow-ups use it as the real test set: [UAV-Sim, arXiv 2310.16255](https://arxiv.org/pdf/2310.16255); [Progressive Transformation Learning, arXiv 2211.01778](https://arxiv.org/pdf/2211.01778)
- **UAV-SynthScene (Remote Sensing 18(2):361, 2026)**
  - Synthetic data from oblique photogrammetry.
  - Detectors trained only on synthetic data reach accuracy comparable to large real annotated sets, measured on a real-world test set (mAP50). The exact test set names could not be retrieved (MDPI returned 403).
  - Source: [MDPI](https://www.mdpi.com/2072-4292/18/2/361); [ResearchGate](https://www.researchgate.net/publication/399969785_Bridging_the_Sim2Real_Gap_in_UAV_Remote_Sensing_A_High-Fidelity_Synthetic_Data_Framework_for_Vehicle_Detection)
- **Aerial vehicle detector adaptation with diffusion-generated data (arXiv 2507.20976)**: evaluates on DOTA plus two new real datasets of its own. — [arXiv 2507.20976](https://arxiv.org/pdf/2507.20976)
- **Game-engine UAV synthetic data (arXiv 2112.12252)**: 4K synthetic sets, transfer tested on real UAV datasets. — [arXiv 2112.12252](https://arxiv.org/pdf/2112.12252)
- **GTA-V UAV sim2real appearance-gap paper (arXiv 2605.02291)**: 456 real-style images, 5 classes. — [arXiv 2605.02291](https://arxiv.org/pdf/2605.02291)
- **Syn2Real ships (Applied Sciences 2024)**: synthetic aerial ship generation and adaptation, the maritime analogue. — [MDPI 14/11/4558](https://www.mdpi.com/2076-3417/14/11/4558)
- **Drone-detection sim2real**: SynDroneVision ([arXiv 2411.05633](https://arxiv.org/pdf/2411.05633)) and SimD3 ([arXiv 2601.14742](https://arxiv.org/html/2601.14742)) test on several unseen real benchmarks, but they detect drones, not ground or sea targets.
- **TUM thesis on simulation-trained vehicle detection**: lists VEDAI and DOTA as the relevant real benchmarks. — [TUM mediaTUM](https://mediatum.ub.tum.de/doc/1721356/erncmqmyemcpiuubk811mgqcc.pdf)

### Inferences
- For CamSim, a two-tier evaluation looks defensible:
  - Community-comparable numbers on a public real set (VEDAI or a DOTA vehicle subset for vehicles; SeaDronesSee for boats).
  - A small, geo-matched Camp Pendleton set with telemetry for the precise sim-vs-real comparison, following Archangel's metadata-stratified design.
- From prior knowledge, unverified this session:
  - VEDAI is Utah AGRC aerial ortho imagery (US, nadir, about 12.5 cm, includes NIR).
  - COWC includes US sites (Utah, Columbus OH) plus Potsdam, Toronto and others.
  - RarePlanes (CosmiQ/AI.Reverie) pairs real and synthetic satellite imagery under CC BY-SA 4.0.
  - DSIAC ATR Algorithm Development Image Database (US Army NVESD, MWIR/visible ground vehicles) is distribution-limited.
  - If true, VEDAI is the closest *US* vehicle yardstick, but it is nadir orthophoto rather than oblique UAV video.

### Gaps
- The exact real test sets in UAV-SynthScene (MDPI 403).
- Army/DIU synthetic-data challenge real test sets (for example DIU xView-type or Army RCV/ATR challenges). Not found in this session's searches.
- A 2024–2026 NeurIPS D&B, CVPR or WACV US-captured UAV thermal vehicle dataset. None surfaced. HIT-UAV and DroneVehicle (prior knowledge) are Chinese.

## Q3. Are there public FMV clips with MISB KLV over the US that could be hand-labelled?

### Takeaway
Yes, but with unclear licences. The classic public STANAG 4609 test clips "Day Flight.mpg" (97 MB) and "Night Flight IR.mpg" (162 MB) are still hosted on samples.ffmpeg.org with no licence file. Esri's FMV tutorial uses a truck clip near Cheyenne, Wyoming, under Esri terms. None of these come with labels.

### Cited findings
- `samples.ffmpeg.org/MPEG2/mpegts-klv/` lists "Day Flight.mpg" (97M, 2012-10-06) and "Night Flight IR.mpg" (162M, 2012-10-06). There is no readme or licence in the directory. — [FFmpeg samples](https://samples.ffmpeg.org/MPEG2/mpegts-klv/)
- Esri's ArcGIS Pro FMV tutorial uses "video data that tracks a semi-trailer truck … along a highway near Cheyenne, Wyoming". The data is distributed with Esri tutorial material under Esri licensing. I could not fetch the tutorial page itself. — [Esri FMV tutorial](https://doc.arcgis.com/en/imagery/workflows/tutorials/fmv-video-player-tutorial.htm), via search snippet
- QGIS FMV plugin (open source) reads MISB ST 0601, ST 0102 and EG 0104. The README suggests building MISB video from DJI video + telemetry when no MISB clip is available. — [QGIS FMV plugin](https://plugins.qgis.org/plugins/QGIS_FMV/version/4.0/); [QGISFMV GitHub](https://github.com/all4gis/qgisfmv)
- Tools for decoding KLV from such clips: pymisb ([PyPI](https://pypi.org/project/pymisb/)) and ImpleoTV KlvInspector ([help](https://impleotv.com/content/klvinspector/help/index.html)).

### Inferences
- The FFmpeg clips are widely used for KLV parser testing. Their provenance and licence are undocumented, so treat them as internal test material only and do not redistribute labels plus frames.
- In-repo note: CamSim's CLAUDE.md warns that ffmpeg ≤ 6.1 strips 5 bytes from stream_id 0xFC KLV packets. Extract KLV from these clips with `scripts/klv_conformance/check.js` (its own TS demuxer), not ffmpeg.
- For a precisely matched Camp Pendleton test set, the most practical path is a self-flown DJI (or similar) collection, converted to MISB via the QGIS FMV route (video + telemetry) and hand-labelled in COCO. This gives a clean licence and real telemetry that matches CamSim's KLV.

### Gaps
- Download URL, licence and exact location of the Esri Cheyenne sample, and of other Esri FMV samples. The page did not render.
- I could not verify the provenance or location of Day Flight / Night Flight IR (often said to be US test-range footage; unverified).
- I did not find a file named "Truck.ts".
- I found no publicly released *labelled* MISB-KLV FMV dataset over the US.
