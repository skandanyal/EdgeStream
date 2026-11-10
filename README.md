# EdgeStream

**EdgeStream** is an asynchronous, high-throughput edge video analytics engine implemented in modern **C++20**. Engineered for memory-constrained edge hardware, it decouples camera-rate I/O ingestion from GPU tensor inference, spatial-temporal multi-object tracking, and polygon zone compliance monitoring.

All pipeline telemetry—throughput, queue occupancy, frame drops, and thread exit latencies—is logged dynamically at microsecond precision to an embedded SQLite3 database.

---

## Architecture Overview

```mermaid
flowchart TD
    subgraph Ingestion ["Ingestion Subsystem (CPU Worker)"]
        A["Video Stream (RTSP / USB / MP4)"] --> B["H.264 / Codec Decoder"]
        B --> C["Producer Thread"]
    end

    C -->|"Zero-Copy Move\nDrop-Oldest on Full"| D[("Bounded RingBuffer<cv::Mat>\nPre-allocated Memory")]

    subgraph Analytics ["Analytics Subsystem (Consumer / GPU Worker)"]
        D --> E["YOLOv8 ONNX Runtime + CuDNN 9"]
        E --> F["Vectorized Tensor Post-Processing"]
        F --> G["Greedy IoU Multi-Object Tracker"]
        G --> H["Polygon Zone Compliance Engine"]
        H --> I["HighGUI Scaled Display (1280x720)"]
    end

    Analytics -->|"Microsecond Telemetry"| J[("Embedded SQLite3 Database")]
```

---

## Deep Dive: Core Engineering Mechanics

### 1. The Shock Absorber: Lossy Bounded Ring Buffer

When ingesting live hardware feeds, the physical camera clock (33.3 ms/frame) will not pause for downstream compute. Standard queues either exhaust system memory (unbounded) or stall the video source (blocking). EdgeStream implements an atomic **drop-oldest eviction policy**:

```mermaid
flowchart LR
    A["Incoming Frame"] --> B{"Buffer Full?\n(count == capacity)"}
    B -- Yes --> C["Advance Head Pointer\n(head = (head + 1) % cap)"]
    C --> D["Increment Dropped Counter\n(atomic uint64_t)"]
    D --> E["Overwrite Oldest Slot\nstd::move(frame)"]
    B -- No --> E
    E --> F["Notify Consumer via CV"]
```

- **Memory Guarantee:** Contiguous vector storage pre-allocated once in the constructor. Zero dynamic allocations in the hot ingestion loop.
- **Freshness Guarantee:** The consumer never processes stale buffered video; it always evaluates live reality.

---

### 2. High-Performance Tensor Parsing & Data Association

YOLOv8 outputs a continuous 3D tensor of shape `[1, 84, 8400]`, where 84 represents 4 bounding box coordinates and 80 COCO class probabilities across 8,400 candidate anchor boxes.

```mermaid
flowchart TD
    A["Raw Output Tensor [1, 84, 8400]"] -->|"SIMD Cache Transpose"| B["Contiguous Matrix [8400, 84]"]
    B --> C["Confidence Pre-Filter (score > threshold)"]
    C --> D["Coordinate Recovery (Center-Width to Left-Top)"]
    D --> E["Boundary Clamping (std::max / std::min)"]
    E --> F["Non-Maximum Suppression (IoU overlap filter)"]
    F --> G["Greedy IoU Tracker Association"]
```

- **Tracker State Machine:** Bypasses computationally expensive deep appearance re-identification models (e.g., DeepSORT) in favor of a sub-millisecond geometric IoU cost matrix ($\mathcal{O}(T \times D)$). Tracks maintain sticky identities across short-term occlusions and prune dead tracks after a configurable tolerance window.

---

### 3. Spatial-Temporal Zone Compliance

Evaluates whether tracked targets loiter inside restricted convex or concave geofences:

```mermaid
flowchart TD
    A["Tracked Target"] --> B["Compute Bottom-Center Footpoint"]
    B --> C{"cv::pointPolygonTest(polygon, footpoint) >= 0"}
    C -- Inside --> D{"First time in zone?"}
    D -- Yes --> E["Record Steady Clock Entry Timestamp"]
    D -- No --> F["Compute Dwell Time = now - entry_timestamp"]
    F --> G{"Dwell Time >= Threshold?"}
    G -- Yes --> H["Trigger Visual Violation Alert (Box & Polygon Turn RED)"]
    G -- No --> I["Safe Monitoring (Green)"]
    C -- Outside --> J["Erase Track from Active Dwell Map"]
```

---

## Hardware Profiling Platform

All empirical benchmarks were recorded on the following physical edge workstation:

- **Processor (CPU):** AMD Ryzen 5 6600H (6 Cores / 12 Threads @ 3.3 GHz base, 4.5 GHz boost)
- **System Memory:** 8 GB DDR5 RAM
- **Graphics (GPU):** NVIDIA GeForce RTX 3050 Laptop GPU (4 GB GDDR6 VRAM, Ampere Architecture)
- **Operating System:** Linux (Ubuntu LTS, Kernel 7.0.0-42-generic)
- **Acceleration Stack:** CUDA 13.3 + CuDNN 9.0 + Microsoft ONNX Runtime C++ API
- **Operating Conditions:** AC Power connected (charging), OS set to **Performance Mode**.
- **Video Workloads:** Full HD (1920×1080) @ 30.0 FPS. RingBuffer capacity bounded to **50 frames**.

---

## Empirical Benchmarks & Telemetry

The following metrics were captured directly by the embedded SQLite telemetry recorder across controlled runs on Full HD (1080p @ 30 FPS) video streams:

### 1. Dense Multi-Target Workload (`traffic2_30fps.mp4` — Full HD 1080p @ 30 FPS)

| Backend & Model   | Throughput (Avg) | Throughput (Peak) | Avg Queue Depth       | Dropped Frames  | Total Elapsed Time | Pipeline Profile                                                                                                                                        |
| :---------------- | :--------------- | :---------------- | :-------------------- | :-------------- | :----------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **CPU (YOLOv8n)** | **12.18 FPS**    | 30.00 FPS         | **46.80 / 50 frames** | **700 dropped** | 51.44 s            | **Severe CPU Ingestion Bottleneck:** Ingestion outpaces inference; buffer saturates at 94% capacity, dropping 700 frames to shed latency.               |
| **GPU (YOLOv8n)** | **30.08 FPS**    | **41.77 FPS**     | **3.69 / 50 frames**  | **34 dropped**  | 43.42 s            | **Locked 30 FPS Real-Time:** Inference easily outpaces frame arrival; queue drained to ~3.7 frames with minimal frame drops.                            |
| **CPU (YOLOv8s)** | **6.20 FPS**     | 30.00 FPS         | **45.58 / 50 frames** | **987 dropped** | 55.15 s            | **Extreme CPU Compute Bottleneck:** 11.2M parameter model chokes CPU; buffer continuously sheds 987 stale frames to prevent memory explosion.           |
| **GPU (YOLOv8s)** | **29.48 FPS**    | **33.77 FPS**     | **14.58 / 50 frames** | **16 dropped**  | 44.46 s            | **Production Balance:** Delivers real-time ~30 FPS on dense multi-target traffic; buffer smoothly absorbs deeper compute waves at 14.6 avg queue depth. |

---

### 2. Sparse Multi-Target Workload (`2cats_30fps.mp4` — Full HD 1080p @ 30 FPS)

| Backend & Model   | Throughput (Avg) | Throughput (Peak) | Avg Queue Depth       | Dropped Frames  | Total Elapsed Time | Pipeline Profile                                                                                                    |
| :---------------- | :--------------- | :---------------- | :-------------------- | :-------------- | :----------------- | :------------------------------------------------------------------------------------------------------------------ |
| **CPU (YOLOv8n)** | **12.24 FPS**    | 30.00 FPS         | **45.53 / 50 frames** | **482 dropped** | 38.00 s            | **CPU Ingestion Lag:** Lightweight model on CPU remains capped at ~12.2 FPS; buffer saturates at 91% capacity.      |
| **GPU (YOLOv8n)** | **30.55 FPS**    | **44.16 FPS**     | **4.27 / 50 frames**  | **15 dropped**  | 30.92 s            | **Ultra-Low Latency:** Queue drained down to ~4 frames; frame drops reduced by **97%** compared to CPU.             |
| **CPU (YOLOv8s)** | **6.09 FPS**     | 30.00 FPS         | **44.05 / 50 frames** | **695 dropped** | 41.86 s            | **Severe CPU Compute Bottleneck:** Small model inference takes ~164 ms per frame, dropping 695 frames to avoid OOM. |
| **GPU (YOLOv8s)** | **30.11 FPS**    | **36.07 FPS**     | **9.67 / 50 frames**  | **15 dropped**  | 31.04 s            | **Real-Time Edge Standard:** Rock-solid 30.11 FPS delivery; 15 dropped frames total with clean queue drainage.      |

---

### Key Empirical Findings:

1. **Hardware Acceleration Factor ($2.5\times$ to $4.8\times$ Throughput Gain):**
   - On **YOLOv8n**, switching from CPU to GPU jumped throughput from **12.2 FPS $\rightarrow$ 30.1–30.6 FPS** ($2.5\times$ speedup).
   - On **YOLOv8s**, switching from CPU to GPU jumped throughput from **6.1–6.2 FPS $\rightarrow$ 29.5–30.1 FPS** ($4.8\times$ speedup), transforming an unviable 6 FPS slideshow into true real-time 30 FPS video analytics.

2. **RingBuffer Drop-Oldest Policy Saves the Host from OOM:**
   - On CPU, the camera clock (30 FPS) severely outpaced compute (6.2–12.2 FPS). Without our bounded lossy ring buffer, hundreds of megabytes of uncompressed 1080p frames would have accumulated in system RAM. The ring buffer capped occupancy at 45–47 frames and dropped **482 to 987 stale frames**, ensuring the system never crashed and always processed current real-time frames.

3. **Queue Drainage Dynamics on GPU (Up to 98% Reduction in Frame Loss):**
   - On GPU, average queue occupancy collapsed from **~46 frames down to 3.7–4.3 frames for Nano** and **9.7–14.6 frames for Small**, demonstrating that the consumer transitioned from a downstream bottleneck into an active drain. Total frame loss plummeted by **96% to 98%** (from 987 down to 16 in dense traffic).

4. **Model Capacity & Detection Fidelity (Nano vs Small):**
   - While **YOLOv8n** (3.2M parameters) executes with lower compute overhead, it noticeably struggles with object detection recall and localization stability, causing bounding boxes to flicker or drop on occluded, moving, or distant targets. In contrast, **YOLOv8s** (11.2M parameters — a $3.5\times$ capacity increase) performs significantly better at reliably identifying targets, holding tighter bounding boxes around objects for longer durations, and maintaining persistent track continuity across consecutive frames while still comfortably sustaining our target 30 FPS real-time throughput on GPU.

---

## Proof of Concept & Visual Demonstrations

### Dynamic Video Tracking & Zone Compliance Demos

Both demonstrations below showcase 8-second execution captures of the pipeline running the higher-fidelity **YOLOv8s** (11.2M parameters) model accelerated on **NVIDIA GPU** (RTX 3050 Laptop GPU, ONNX Runtime with CUDA 13 + CuDNN 9 backend) at real-time 30 FPS.

#### Scenario A: Sparse Target & Polygon Loitering Alert

_Demonstrates persistent track ID retention and spatial-temporal dwell monitoring. A target entering the designated polygon triggers entry timestamp logging. Once the continuous loitering duration exceeds the **3.0-second minimum dwell threshold**, the tracking bounding box and geofence boundary dynamically escalate into a violation alert by turning **RED**._

![Sparse Tracking Demo](assets/cats_recording.gif)

#### Scenario B: Dense Multi-Vehicle Tracking & Ingestion Drain

_Demonstrates continuous multi-object track ID persistence and greedy IoU data association across 20–36 concurrent vehicles in dense traffic. Under real-time GPU inference (~30 FPS), the consumer worker actively drains the 50-slot bounded ring buffer with zero stalls and minimal frame loss._

![Dense Traffic Demo](assets/traffic_recording.gif)

---

## System Configuration (`config.yaml`)

EdgeStream decouples operational tuning from compilation. All pipeline hyperparameters are dynamically loaded at runtime:

- **Hardware & Backend Selection (`model`):** Selects between `CUDA` (ONNX Runtime via Tensor Cores) and `CPU` (OpenCV fallback), and specifies the model weight path (`yolov8n.onnx` vs `yolov8s.onnx`).
- **Ingestion Buffer Governance (`buffer_configs`):** Sets the queue `capacity` and `num_producers`. Bounding the capacity protects the edge host against memory exhaustion during unexpected network or processing spikes.
- **Inference Filtering (`pipeline_configs`):** Balances detection sensitivity via `confidence_threshold` (pre-filtering background noise) and `nms_threshold` (suppressing duplicate overlapping bounding boxes).
- **Compliance Geofencing (`zone_configs`):** Defines arbitrary $N$-point polygon vertices in pixel coordinates alongside the `min_dwell_time_seconds` required to escalate from safe presence to an alert.
- **Telemetry Persistence (`database`):** Specifies the SQLite database destination. Database handles are managed via RAII and reset automatically on process teardown to prevent stale metric pollution.

---

## Verification & Systems Safety

The automated test suite ([tests/tests.cpp](file:///home/skandan-c-y/CLionProjects/EdgeStream/tests/tests.cpp)) covers the core failure modes of edge pipelines without hardware camera dependencies:

```text
[==========] Running 12 tests from 6 test suites ran. (471 ms total)
[  PASSED  ] 12 tests.
```

### Major Concerns Addressed by Verification Suite:

1. **Concurrency Safety & Deadlock Prevention:** Multi-threaded stress tests push 1,000 frames concurrently across multiple producers to assert zero race conditions. Graceful shutdown tests assert that `stop()` unblocks blocked consumers in under 100 ms.
2. **Buffer Overrun & Memory Protection:** Verifies that overflowing a full queue cleanly evicts the oldest elements, increments the dropped counter, and retains strict FIFO ordering for remaining elements.
3. **Temporal Identity Stability:** Simulates moving targets across synthetic frames to verify that track IDs remain sticky and do not swap during inter-frame displacement.
4. **Track Lifecycle Pruning:** Verifies that temporarily occluded tracks are retained for up to `max_lost_frames`, after which stale memory records are pruned.
5. **Spatial Compliance Mathematics:** Validates raycasting point-in-polygon containment and ensures dwell-time clocks trigger violation alerts only when thresholds are breached.
6. **Telemetry Transactional Integrity:** Validates SQLite schema generation, multi-column metric logging, and aggregate mathematical queries.

---

## Build & Execution

### 1. System Prerequisites

Install the required C++20 build tools and native dependencies (tested on Ubuntu 22.04 / 24.04 LTS):

```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libopencv-dev \
    libsqlite3-dev \
    libyaml-cpp-dev
```

_(Optional for GPU acceleration):_ Ensure NVIDIA drivers and CUDA Toolkit are installed on your host system:

```bash
nvidia-smi
nvcc --version
```

---

### 2. ONNX Runtime Setup

EdgeStream links directly against the Microsoft ONNX Runtime C++ API. Download and unpack the release tarball for your target architecture into the `onnxruntime/` directory (the sanitized `CMakeLists.txt` automatically discovers any unpacked archive inside `onnxruntime/`):

#### Option A: GPU Acceleration (CUDA / Tensor Cores — Recommended)

```bash
mkdir -p onnxruntime && cd onnxruntime
# Example: ONNX Runtime GPU build (adjust version tag if necessary)
wget https://github.com/microsoft/onnxruntime/releases/download/v1.18.0/onnxruntime-linux-x64-gpu-1.18.0.tgz
tar -xvf onnxruntime-linux-x64-gpu-1.18.0.tgz
cd ..
```

#### Option B: CPU Execution

```bash
mkdir -p onnxruntime && cd onnxruntime
wget https://github.com/microsoft/onnxruntime/releases/download/v1.18.0/onnxruntime-linux-x64-1.18.0.tgz
tar -xvf onnxruntime-linux-x64-1.18.0.tgz
cd ..
```

> [!TIP]
> If you already have ONNX Runtime installed system-wide or in a custom directory, you can simply pass `-DORT_ROOT=/path/to/onnxruntime` during the CMake configuration step.

---

### 3. Build Instructions

Configure and compile the engine using CMake:

```bash
# 1. Configure build with Release optimizations (-O3 -march=native)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Note: If ONNX Runtime is at a custom path:
# cmake -B build -DCMAKE_BUILD_TYPE=Release -DORT_ROOT=/opt/onnxruntime

# 2. Build the main application and automated test suite
cmake --build build --config Release -j$(nproc)
```

Binaries will be placed in the `build/` directory:

- `build/main`: Primary multi-threaded ingestion and edge analytics engine.
- `build/run_tests`: Standalone GoogleTest verification suite.

---

### 4. Running the Pipeline

#### A. Running on Video Files or Test Streams

```bash
# Process pre-recorded benchmark video
./build/main assets/traffic.mp4

# Or test with the sparse target video
./build/main assets/2cats.mp4
```

#### B. Running on Live Camera Feeds

```bash
# Connect to default V4L2 USB camera index 0
./build/main 0

# Connect to RTSP IP camera feed
./build/main rtsp://admin:password@192.168.1.100:554/stream1
```

#### C. Running Automated Unit & Concurrency Tests

Run the standalone GoogleTest suite (12 tests covering ring buffer eviction, multi-threaded stress tests, YOLO tensor parsing, IoU tracker association, polygon zone math, and SQLite telemetry):

```bash
./build/run_tests
```

---

### 5. Switching Hardware Backends & Models

All pipeline settings are controlled via `config.yaml` without recompilation:

```yaml
# config.yaml
model:
  backend: "CUDA" # Set to "CUDA" for NVIDIA GPU or "CPU" for OpenCV CPU fallback
  model_path: "models/yolov8s.onnx" # Switch between "models/yolov8n.onnx" and "models/yolov8s.onnx"
  confidence_threshold: 0.50
  nms_threshold: 0.45

buffer_configs:
  capacity: 50 # Bound ring buffer queue depth to prevent host memory exhaustion
  num_producers: 1

zone_configs:
  min_dwell_time_seconds: 3.0 # Trigger loitering violation after target stays 3s in polygon
```

To switch to CPU mode, set `backend: "CPU"` in `config.yaml`. EdgeStream will automatically route inference through OpenCV DNN with CPU optimizations and skip CUDA initialization.

---

## Repository Structure

```text
EdgeStream/
├── assets/                 # Validation imagery and benchmark video streams
├── config.yaml             # Runtime pipeline configuration
├── database/               # Ephemeral SQLite3 operational telemetry storage
├── include/
│   ├── database.hpp        # RAII SQLite3 telemetry interface
│   ├── detector.hpp        # ONNX Runtime CUDA / OpenCV DNN inference interface
│   ├── ringBuffer.hpp      # Zero-allocation, lossy runtime-dynamic ring buffer
│   ├── tracker.hpp         # Multi-object IoU data association interface
│   └── zoneMonitor.hpp     # Raycasting polygon inclusion & loitering engine
├── models/                 # Neural network ONNX weights (YOLOv8n, YOLOv8s)
├── src/
│   ├── database.cpp        # SQLite schema creation & aggregation queries
│   ├── detector.cpp        # YOLOv8 tensor parsing, blob prep, and NMS
│   ├── tracker.cpp         # Frame-to-frame tracking & lifecycle state machine
│   └── zoneMonitor.cpp     # Point-in-polygon math & alert rendering
├── tests/
│   └── tests.cpp           # GoogleTest concurrency, inference, and compliance suite
├── CMakeLists.txt          # Unified build script
└── main.cpp                # Decoupled Producer-Consumer pipeline entry point
```
