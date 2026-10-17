# Government / defence-funded aerial and UAV EO/IR vehicle datasets (US) for a sim-to-real test set

Scope: datasets released by US government or defence programmes (DSIAC/NVESD, DARPA, IARPA-adjacent, AFRL SDMS, Kitware/DARPA VIRAT, NPS, Army). Research ran 2026-10-15 with about 20 tool calls. Several primary pages could not be fetched (PSU VIVID site HTTP 500; UCF VIRAT page TLS error). Those items are marked.

## Which datasets are over the US and taken from an aircraft or UAV (not a ground tower)?

### Takeaway
Every dataset found is over the US, but only some are airborne. **Airborne:** AFRL CSUAV (Columbus, OH: EO plus MWIR), AFRL CLIF 2007 and WPAFB 2009 (Ohio, EO WAMI), DARPA VIVID (Eglin AFB, FL and Fort Pickett, VA: EO plus IR, from a military aircraft), VIRAT Aerial (DARPA EO/IR aircraft video, reportedly Fort A.P. Hill, VA) and the MEVA UAV subset (Muscatatuck, IN; DJI drones). **Ground-based:** the DSIAC/SENSIAC ATR Algorithm Development Image Database is ground-based (cameras on slightly elevated positions overlooking a valley), not aerial. It does not meet the "aircraft/UAV" requirement.

### Cited Findings

**DSIAC / SENSIAC "ATR Algorithm Development Image Database"**
- Collected by the US Army Night Vision and Electronic Sensors Directorate (NVESD). It contains visible and MWIR imagery, a seven-section user's guide, an image viewer, ground truth, meteorological data, target photographs and calculated target-to-background temperature differentials. Classes are people, foreign military vehicles and civilian vehicles at "a variety of ranges" and aspect angles. Size is about 206 GB as a multipart download, and the page suggests a 1 TB external drive. The page shows "Updated: September 25, 2026" — [DSIAC database page](https://dsiac.dtic.mil/databases/atr-algorithm-development-image-database/)
- The DSIAC page does not state the collection site or the platform — [DSIAC database page](https://dsiac.dtic.mil/databases/atr-algorithm-development-image-database/)
- Geometry, from a 2023 paper that used it:
  - **Ground-based.** The cameras were on "slightly elevated positions overlooking a valley of dry scrubland". It is not aerial.
  - Two forward-looking cameras: an L3 Cincinnati Electronics Night Conqueror MWIR (640×480 InSb, 28 µm pitch) and an Illunis visible camera.
  - Ten targets: foreign military vehicles, a towed artillery piece and dismounted civilians. The 8 vehicle classes used are Pickup, SUV, BTR70, BRDM2, BMP2, T72, ZSU23-4 and 2S3.
  - Each target drove a 100 m diameter circle at ranges from 1000 to 5000 m in 500 m steps.
  - Recording was at 30 Hz for one minute per target, range and time, giving 1,680 frames per sequence.
  - Day for both cameras, plus night for MWIR. MWIR data is 16-bit.
  - Boxes came from a semi-automatic process (manual seeds plus a tracker). The tracker drifts at far range, so the authors recomputed boxes from positional metadata.
  - [Long-Range Thermal Target Detection… (PMC10536622)](https://pmc.ncbi.nlm.nih.gov/articles/PMC10536622/)
- Other papers describe "six military and two civilian vehicles" or seven military targets. Some use only the 1000/1500/2000 m subset — [MDPI Sensors 19(17):3702](https://www.mdpi.com/1424-8220/19/17/3702); [Springer EURASIP 2011](https://link.springer.com/article/10.1186/1687-6180-2011-124)
- Note on the "1–5 km" wording in the brief: in DSIAC's 2020 technical-inquiry report, "MWIR data on ground vehicle systems and personnel for ranges limited between 1 and 5 kilometers" is given as an example of **Eglin Signature Data Center (ESDC)** data, not of the ATR database. The same report says ESDC also holds "mobile and fixed tower systems, ground systems, and airborne systems" data — [DSIAC TI report DSIAC-2190996](https://dsiac.dtic.mil/wp-content/uploads/2022/01/DSIAC-2190996.pdf)

**AFRL SDMS: CSUAV (Columbus Surrogate UAV)**
- Two surrogate UAVs flew a "dog bone course" over The Ohio State University campus in Columbus, OH on 28 Oct 2007, from about 11:00 to 13:21 local time. The planned track intersection was east of Ohio Stadium — [SDMS CSUAV](https://www.sdms.afrl.af.mil/index.php?collection=csuav)
- EO platform: Illunis XMV-11000 monochrome CCD, 4004×2672, about 5 Hz, flying at about 6.5 kft MSL and 90–130 kt east–west. Ground resolution is about 0.61 ft/px — [SDMS CSUAV](https://www.sdms.afrl.af.mil/index.php?collection=csuav)
- IR platform: FLIR ThermoVision SC6000 **MWIR (3–5 µm)**, 640×512, 30 Hz, flying at about 2.5 kft MSL and 80–100 kt north–south. Ground resolution is about 0.88 ft/px — [SDMS CSUAV](https://www.sdms.afrl.af.mil/index.php?collection=csuav)
- 15 runs: 234–329 EO frames and 2087–3723 IR frames per run. Raw files carry headers, parsed by the supplied `readHdr.m`. Example AVIs show filename, local timestamp and frame number. The page mentions no ground truth or labels — [SDMS CSUAV](https://www.sdms.afrl.af.mil/index.php?collection=csuav)

**AFRL SDMS: CLIF 2007 (Columbus Large Image Format)**
- EO imagery of the OSU campus from Oct 2007. The platform approaches the campus and loiters over it.
- The sensor is a large-format monochrome EO array of six cameras (2×3) at about 2 fps.
- The full set has 16,117 image sets (96,702 raw files) plus 16,117 auxiliary text files, 966 GB in total. It also includes frames from J. W. Davis's (OSU) ground sensor network. A 50-set sample DVD is available for immediate download.
- Suggested challenge areas include tracking and ATR.
- The page states neither altitude nor ground truth.
- [SDMS CLIF 2007](https://www.sdms.afrl.af.mil/index.php?collection=clif2007)
- CLIF 2006 is listed on SDMS, but its page was not fetched — [SDMS WPAFB 2009 page nav](https://www.sdms.afrl.af.mil/index.php?collection=wpafb2009)

**AFRL SDMS: WPAFB 2009 WAMI**
- A flyover of Wright-Patterson AFB, OH and the surrounding area, dated 21 Oct 2009.
- The sensor is a six-camera (2×3) monochrome EO matrix with 105 mm lenses, mosaicked into one image.
- 1,537 stitched NITF 2.1 frames, split into thirds: training with image truth, self-test with image truth, and self-eval without truth. Also supplied: JPEG pyramids, raw six-camera data, and a Derby index database holding the truth.
- [SDMS WPAFB 2009](https://www.sdms.afrl.af.mil/index.php?collection=wpafb2009)
- Ground sample distance is about 0.25 m. Pose files ship with the raw images — [ResearchGate: Camera models for WPAFB 2009 WAMI](https://www.researchgate.net/publication/282540607_Camera_models_for_the_wright_patterson_air_force_base_WPAFB_2009_wide-area_motion_imagery_WAMI_data_set); [arXiv 1803.04856](https://arxiv.org/html/1803.04856v1)

**DARPA VIVID**
- SDMS "Video Sample Set #1" holds clips from DARPA VIVID Data Collection 1 at **Eglin AFB, FL, 2–3 Dec 2003**.
- Sensors: EO Daylight TV (DLTV), EO DLTV Spotter and **IR**.
- Content: military and civilian vehicles (for example a truck and a convoy), in AVI clips of about 60 s. Each subject has a wide/spotter view, a tight-zoom DLTV view and an IR view.
- The page states no platform, telemetry or truth.
- [SDMS Video Sample Set #1](https://www.sdms.afrl.af.mil/index.php?collection=video_sample_set_1)
- The DARPA VIVID programme built "an automated video-based ground targeting system for unmanned aerial vehicles" — [Bose, academia.edu abstract](https://independent.academia.edu/MichaelBose6)
- The PSU/CMU VIVID tracking evaluation site (PETS 2005; Collins, Zhou & Teh) hosted ground-truthed sequences. EgTest01 is "collected at Eglin during the DARPA VIVID program": vehicles circle a runway, and some frames were dropped. PkTest02/03 (Fort Pickett, VA) were described as thermal IR in search snippets — [VIVID Tracking Evaluation Web Site](http://vision.cse.psu.edu/data/vividEval/datasets/datasets.html) (**returned HTTP 500 on 2026-10-15**); [CMU publication page](https://www.ri.cmu.edu/publications/an-open-source-tracking-testbed-and-evaluation-web-site); [Collins pets2005](https://www.cs.cmu.edu/~rcollins/Pub/pets2005.html)
- One review lists VIVID as 9 videos at 30 fps — [arXiv 1910.09761](https://arxiv.org/pdf/1910.09761)
- VIVID egtest sequences are mirrored inside the LAMOD moving-object dataset on GitHub — [LAMOD GitHub](https://github.com/LAMODDATASET/LAMOD)

**VIRAT Video Dataset: aerial portion**
- Aerial subsets were added in Version 2.0 on 11 Jan 2012. "Currently, only videos are available."
- A 29 Dec 2016 update: "Annotating the aerial video proved extremely challenging", and the original contract could not complete the annotations. So there are **no aerial labels**.
- The aerial data is "Distribution A, Release Unlimited". It is downloadable from the Kitware VIRAT-Aerial collection: https://data.kitware.com/#collection/611e77a42fa25629b9daceba
- [viratdata.org](https://viratdata.org/)
- Resolution is low: 480×720. A maintainer said in 2014 that no HD UAV video was available — [UG2+ report arXiv 1907.11529](https://arxiv.org/pdf/1907.11529); [Kitware mailing list 2014](https://public.kitware.com/pipermail/viratdata/2014-June/000008.html)
- A secondary description of the related UCF "APHill" aerial videos: provided by DARPA, recorded with an EO/IR sensor from a military aircraft at over 1000 m. "APHill" suggests Fort A.P. Hill, VA — [UCF CRCV VIRAT page, via search snippet](https://www.crcv.ucf.edu/data/VIRAT.php) (the page failed TLS verification on fetch)

**MEVA (IARPA DIVA programme; found via viratdata.org)**
- The KF1 collection was recorded over three weeks at the **Muscatatuck Urban Training Center (MUTC), Indiana**, with more than 100 actors.
- UAV video: 4.6 h (26 GB) from two DJI Inspire 1 v2 drones. The site also has ground IR–EO camera pairs, but no UAV thermal is stated.
- Supplied with it: KRTD camera models, a 3D PLY model of MUTC, actor GPS logs, and activity annotations (NIST ActEV).
- Licence: CC BY 4.0, free on AWS S3 (Public Dataset Program).
- [mevadata.org](https://mevadata.org/)
- MEVA also has 250+ h of ground video and 12.5 h annotated — [viratdata.org](https://viratdata.org/)

**Naval Postgraduate School (NPS)**
- Searches found NPS UAV flight activity at Camp Roberts, CA (ARSENL swarm flights, JIFX field experiments) but no released labelled EO/IR vehicle dataset — [NPS FX past events](https://nps.edu/web/fx/past-events/-/categories/133091385); [DVIDS Camp Roberts UAVs](https://dvidshub.net/image/2079447/uavs-camp-roberts)

**Other SDMS collections, names only (sensors not checked)**
- The list includes 3D Challenge 2003, AdaptSAPS, ADTS, CCD Challenge Problem, CLIF 2006, CLIF 2007, CSUAV, CV Data Domes, Ft. Hood, Gotcha, GTRI, Greene 07, MAMI 2013, MSTAR, RASPNet, SAR GMTI Challenge, VHF Change Detection, Video Sample Set #1/#2 and Wide Angle SAR — [SDMS](https://www.sdms.afrl.af.mil/index.php?collection=wpafb2009)
- MSTAR, Gotcha, Wide Angle SAR, SAR GMTI, CCD and VHF are SAR or RF. **Exclude them** (not EO/IR).

**Excluded, non-US or non-government** (surfaced incidentally)
- HIT-UAV: China, 2,898 thermal images — [PMC10119175](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC10119175/)
- DroneVehicle: China, RGB+IR — [neurohive](https://neurohive.io/en/news/dronevehicle-new-challenge-and-benchmark-for-vehicle-detection-and-counting/)
- SCUT FIR: Guangzhou, vehicle-mounted — [DSIAC TI report](https://dsiac.dtic.mil/wp-content/uploads/2022/01/DSIAC-2190996.pdf)
- FLIR ADAS: US (Santa Barbara), but car-mounted, not aerial, and not government — [DSIAC infrared datasets inquiry](https://dsiac.dtic.mil/technical-inquiries/notable/infrared-imagery-datasets)

### Inferences
- **Best US airborne thermal candidate: CSUAV.** It has real MWIR from a low (about 2.5 kft MSL) aircraft over a well-mapped US campus (OSU, Columbus), with a timestamped header per frame and known flight lines. NAIP and 3DEP both cover Columbus. However, it has no vehicle labels (they would have to be added), and its targets are campus traffic, not military vehicles.
- **WPAFB 2009 and CLIF 2007** are EO-only WAMI at about 0.25 m GSD, so vehicles are only about 10–20 px long. They are useful for tiny-vehicle detection or tracking and for geo-registration (pose files and NITF), but not for IR.
- **VIVID** (Eglin, Fort Pickett) is the only military-vehicle EO+IR airborne set found. It is small, old (2003–2005, 640×480-class AVI) and its metadata is unclear.
- **VIRAT Aerial** is unlabelled and low-resolution, so it is weak as a test set.
- **MEVA UAV** is RGB-only, but it has the cleanest licence (CC BY 4.0), plus camera models and a site model. Muscatatuck is in Indiana, which NAIP and 3DEP cover.
- **DSIAC ATR** has the best labels and military vehicles, but it is a ground-based, long-range, near-horizontal geometry (a tower or hill looking across a valley). That does not match a UAV view.

### Gaps
- DSIAC ATR database collection site: neither the DSIAC page nor the papers fetched name it. Other papers have attributed it to a desert test range, but no source confirmed this here, so it is unverified.
- CLIF 2006 page contents, SDMS "Video Sample Set #2", "MAMI 2013", "GTRI", "Greene 07" and "Ft. Hood": not fetched, so their sensors and locations are unknown.
- VIVID aircraft type, altitude, depression angles and telemetry: not found, because the PSU site was down (HTTP 500).
- RIT DIRSIG-related real collects (MegaCollect 2010 Rochester, SpecTIR, Tait Preserve): not researched within the tool budget, so no sourced findings. These are mostly hyperspectral and LWIR airborne collects over Rochester, NY, and are not known to carry vehicle labels; verify separately.
- IARPA / NGA / DIU (xView is satellite, so out of scope), DEVCOM C5ISR / NVESD, and ARL aerial EO/IR vehicle releases: a search found no public DEVCOM or ARL aerial vehicle dataset.
- NPS: no released dataset found.

## Which have thermal IR?

### Takeaway
Of the US airborne sets, only **CSUAV (MWIR 3–5 µm, FLIR SC6000)**, **DARPA VIVID (IR, waveband unstated)** and **VIRAT Aerial / APHill (EO/IR sensor per a secondary source)** have thermal. The DSIAC ATR database has MWIR (InSb), but it is ground-based. WPAFB 2009, CLIF 2007 and MEVA UAV are visible-only. None found has LWIR from an aircraft.

### Cited Findings
- CSUAV: FLIR ThermoVision SC6000 MWIR 3–5 µm, 640×512 at 30 Hz, from about 2.5 kft MSL — [SDMS CSUAV](https://www.sdms.afrl.af.mil/index.php?collection=csuav)
- VIVID DC1: "EO Daylight TV (DLTV), EO DLTV Spotter, and IR" — [SDMS Video Sample Set #1](https://www.sdms.afrl.af.mil/index.php?collection=video_sample_set_1)
- DSIAC ATR: L3 CE Night Conqueror MWIR (InSb 640×480), day and night; a visible camera in the daytime only — [PMC10536622](https://pmc.ncbi.nlm.nih.gov/articles/PMC10536622/)
- WPAFB 2009 and CLIF 2007: "monochromatic electro-optical" six-camera arrays — [SDMS WPAFB 2009](https://www.sdms.afrl.af.mil/index.php?collection=wpafb2009); [SDMS CLIF 2007](https://www.sdms.afrl.af.mil/index.php?collection=clif2007)
- VIRAT/APHill: EO/IR sensor from a military aircraft at over 1000 m (secondary source) — [UCF CRCV VIRAT page snippet](https://www.crcv.ucf.edu/data/VIRAT.php)
- MEVA UAV: DJI Inspire 1 v2. Thermal is mentioned only for ground IR–EO pairs — [mevadata.org](https://mevadata.org/)

### Inferences
- If MWIR from the air is required, CSUAV is the only clearly documented candidate. CamSim's `mwir_cooled` preset is closely analogous (cooled InSb-class, 640×512).
- VIVID's IR waveband and whether it was white-hot are unknown.

### Gaps
- VIVID IR band and the VIRAT Aerial IR proportion were not verified from primary documentation.

## What are the access/distribution restrictions, and can an open-source project that publishes results use any of them?

### Takeaway
Two have explicit public-release terms: **VIRAT Aerial ("Distribution A, Release Unlimited")** and **MEVA (CC BY 4.0)**. The **DSIAC ATR database is now marked Distribution Statement A** (public release) but is obtained through a DSIAC request form, and DSIAC "reserves the right for final distribution". **SDMS** collections need a site registration; some files download immediately, while full sets need a data request. SDMS pages fetched state no licence or distribution statement. **ESDC** (Eglin) data is CUI or classified and needs a US Government sponsor, so it is not usable for open source.

### Cited Findings
- **DSIAC ATR database:**
  - The page shows "Distribution Statement A. Approved for public release. (DSIAC reserves the right for final distribution.)"
  - Access is through the online "ATR Algorithm Development Image Database Request" form. The contact is Jesse Jimenez, jesse.h.jimenez3.civ@army.mil, 703-704-0089.
  - The page does not mention eligibility (US person, NATO), DD2345, export control or cost.
  - The page was live, showing "Updated: September 25, 2026".
  - [DSIAC database page](https://dsiac.dtic.mil/databases/atr-algorithm-development-image-database/)
- A 2024 survey claims the dataset is available only to researchers in NATO member countries. This conflicts with the Distribution A marking, and other papers call it "publicly available" — [arXiv 2408.06868 (search snippet)](https://arxiv.org/pdf/2408.06868); [PMC10536622](https://pmc.ncbi.nlm.nih.gov/articles/PMC10536622/) ("Publicly available datasets were analyzed in this study")
- Peer-reviewed papers publish results, images and train/test splits on the DSIAC ATR database, for example MDPI open access with a supplementary split file — [PMC10536622](https://pmc.ncbi.nlm.nih.gov/articles/PMC10536622/); [YOLOatr arXiv 2507.11267](https://arxiv.org/pdf/2507.11267)
- **ESDC (Eglin):**
  - Some data is CUI, and "much of the information is classified at confidential or higher".
  - Access needs an ESDC Data Request form. Contractors and industry need their US Government sponsor's endorsement and then submit to 782TS.RNWI.ESDC@us.af.mil.
  - [DSIAC TI report](https://dsiac.dtic.mil/wp-content/uploads/2022/01/DSIAC-2190996.pdf)
- **SDMS:**
  - WPAFB 2009 NITF is "available immediately through the public download page (registration required)". The full set (raw and JPEG) needs a data request — [SDMS WPAFB 2009](https://www.sdms.afrl.af.mil/index.php?collection=wpafb2009)
  - The CLIF 2007 sample can be downloaded immediately. The full 966 GB set is by request — [SDMS CLIF 2007](https://www.sdms.afrl.af.mil/index.php?collection=clif2007)
  - CSUAV and Video Sample Set #1 are listed for download after registration — [SDMS CSUAV](https://www.sdms.afrl.af.mil/index.php?collection=csuav); [SDMS VSS#1](https://www.sdms.afrl.af.mil/index.php?collection=video_sample_set_1)
  - One paper notes that "due to restrictions in the distribution of the data, acquiring a usable sample set for testing bordered on impossibility" (context: WAMI) — [search snippet, WAMI papers](https://www.researchgate.net/publication/300792562_A_benchmark_for_vehicle_detection_on_wide_area_motion_imagery)
- **VIRAT:**
  - Aerial is "Distribution A, Release Unlimited", free on data.kitware.com.
  - The ground data instead requires a "VIRAT Video Dataset Protection Agreement".
  - [viratdata.org](https://viratdata.org/)
- **MEVA:** CC BY 4.0, on AWS S3 — [mevadata.org](https://mevadata.org/)
- **VIVID:** PSU evaluation site was HTTP 500 on 2026-10-15. Clips are on SDMS behind registration. Sequences are mirrored in LAMOD on GitHub (its licence was not checked) — [SDMS VSS#1](https://www.sdms.afrl.af.mil/index.php?collection=video_sample_set_1); [LAMOD](https://github.com/LAMODDATASET/LAMOD)

### Inferences
- For an open-source project that publishes metrics:
  - **MEVA** (CC BY) and **VIRAT Aerial** (Distribution A) are the clean options.
  - **DSIAC ATR** is Distribution A, so publishing results should be fine, as many papers do. However, redistributing the raw imagery in the CamSim repository is probably not appropriate, because DSIAC "reserves the right for final distribution". Users would fetch it themselves via the request form.
  - **SDMS** sets are public-download-after-registration. Because no licence is stated, the conservative approach is: don't redistribute, do publish results, and ask SDMS before including any frames.
- ESDC is unusable for open source.

### Gaps
- The DSIAC request form's fields (affiliation, citizenship, intended use) were not seen. It is unknown whether foreign or anonymous requesters are refused.
- SDMS registration terms and any end-user agreement text were not fetched. It is unconfirmed whether the SDMS site is reachable without a .mil or .gov account, and the pages fetched fine on 2026-10-15.

## Do they carry geolocation, sensor pose and time metadata good enough to rebuild the scene?

### Takeaway
**WPAFB 2009** (pose files plus NITF, about 0.25 m GSD, published camera models) and **MEVA** (KRTD camera models, 3D site model, GPS) are the strongest for scene rebuilds. **CSUAV** has per-frame headers with timestamps and documented altitudes and flight lines, but per-frame lat/lon/attitude content is unconfirmed. **DSIAC ATR** gives target positional metadata, range and met data, but it is a ground view. **VIVID** and **VIRAT Aerial** have no documented pose metadata.

### Cited Findings
- WPAFB 2009 raw images come with pose files, and the NITF images are projected — [arXiv 1803.04856](https://arxiv.org/html/1803.04856v1); [Camera models for WPAFB 2009 WAMI](https://www.researchgate.net/publication/282540607_Camera_models_for_the_wright_patterson_air_force_base_WPAFB_2009_wide-area_motion_imagery_WAMI_data_set)
- A paper studies the "Impact of Georegistration Accuracy on WAMI Object Detection and Tracking" on this data — [ResearchGate](https://www.researchgate.net/publication/367898976_Impact_of_Georegistration_Accuracy_on_Wide_Area_Motion_Imagery_Object_Detection_and_Tracking)
- CLIF 2007 ships 16,117 auxiliary text files, one per image set. Their content (INS/GPS?) is not stated — [SDMS CLIF 2007](https://www.sdms.afrl.af.mil/index.php?collection=clif2007)
- CSUAV: raw headers are read by `readHdr.m`, AVIs show local timestamps, the date and time window is known (28 Oct 2007, 11:00–13:21), and altitudes (6.5 / 2.5 kft MSL) and headings (E–W / N–S) are given — [SDMS CSUAV](https://www.sdms.afrl.af.mil/index.php?collection=csuav)
- MEVA: KRTD camera models, a site map, a 3D PLY model of MUTC and actor GPS logs — [mevadata.org](https://mevadata.org/)
- DSIAC ATR: ground truth, meteorological data, target photographs and temperature differentials; positional metadata good enough to recompute boxes — [DSIAC page](https://dsiac.dtic.mil/databases/atr-algorithm-development-image-database/); [PMC10536622](https://pmc.ncbi.nlm.nih.gov/articles/PMC10536622/)
- VIRAT Aerial: "only videos are available" — [viratdata.org](https://viratdata.org/)

### Inferences
- To build a CamSim scene package and replay matched views:
  - WPAFB 2009 is the most tractable.
  - CSUAV is next, but needs its header format checked for GPS/INS fields and its time of day is known.
  - With VIVID and VIRAT Aerial, poses would have to be estimated by registering frames to NAIP.
- Imagery time mismatch: NAIP is 2020s and these collects are 2003–2009, so buildings and roads around OSU and WPAFB will have changed.

### Gaps
- Whether CSUAV and CLIF headers contain GPS/INS attitude: unconfirmed.
- WPAFB pose file accuracy and format details were not retrieved.
- Whether the DSIAC ATR database releases the sensor's own location or survey coordinates: not stated in the sources found.
