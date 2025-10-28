# AMD AINIC Network Plugin (ANP)

This repository contains the AINIC network plugin which extends AMD's [RCCL](https://github.com/ROCmSoftwarePlatform/rccl) library for networking capabilities.

---

## Table of Contents
1. [Overview](#overview)
2. [System Requirements](#system-requirements)
3. [Cloning and Building RCCL](#cloning-and-building-rccl)
4. [Cloning This Plugin Project](#cloning-this-plugin-project)
5. [Build Instructions](#build-instructions)
6. [Install Instructions](#install-instructions)
7. [Cleanup Instructions](#cleanup-instructions)
8. [Enabling Telemetry](#enabling-telemetry)
    - [Configuration JSON](#configuration-json)
9. [Device Status JSON](#device-status-json)
    - [Device-Level Information](#device-level-information)
    - [Channel-Level Information](#channel-level-information)
    - [Queue Pair (QP) Information](#queue-pair-qp-information)

---

## Overview
ANP is a plugin library designed to enhance the [RCCL](https://github.com/ROCmSoftwarePlatform/rccl) collective communication library with extended network transport support. The Makefile here compiles this plugin against a locally built RCCL and a ROCm environment.

---

## System Requirements
1. **Operating System**: Linux-based distributions (e.g., Ubuntu, RHEL, CentOS).
2. **ROCm**:
   - Installed ROCm (tested with ROCm 6.x or later).
   - Default ROCm path is `/opt/rocm`; use `ROCM_PATH` to override.
3. **Dependencies**:
   - A working build of [RCCL](https://github.com/ROCmSoftwarePlatform/rccl).
   - `hipcc` from the ROCm toolchain.
   - System libraries for network communication (e.g., `libibverbs`).

---

## Cloning and Building RCCL
If you do not already have a built version of RCCL, follow the steps here:
https://github.com/ROCm/rccl

---

## Cloning This Plugin Project
In a separate directory of your choice:
```bash
git clone https://github.com/ROCm/amd-anp.git
cd amd-anp
```

---

## Build Instructions
1. **Ensure RCCL is built**: The `rccl/build` directory must contain `librccl.so`, `hipify/`, `include/`, etc.
2. **Set `RCCL_HOME`**:
   - Provide the path to the RCCL source tree
     ```bash
      export RCCL_HOME=/home/user/rccl-src/
      ```

4. **Optional: Set `RCCL_BUILD`**:
   - If the RCCL artifacts are not in $RCCL_HOME/build/release, then point to the build artifacts using RCCL_BUILD env var For example:
     ```bash
     export RCCL_BUILD=/home/user/rccl/build
     ```

5. **Optional: `ROCM_PATH`**:
   - If ROCm is in a custom directory (not `/opt/rocm`), specify:
     ```bash
     make RCCL_HOME=$RCCL_HOME ROCM_PATH=/path/to/rocm
     ```

6. **Build Without Telemetry (Default):**

    To build the plugin without the telemetry features enabled (the default behavior), simply run the `make` command:

    ```bash
    make RCCL_HOME=$RCCL_HOME
    ```
    If successful, you will see `librccl-anp.so` in the `build/` folder of this plugin project.

    **Example:**
    ```bash
    make RCCL_HOME=/home/user/rccl-src/
    ```
8.  **Build With Telemetry Enabled:**

    To build the plugin with telemetry features enabled use the build command with flag ANP_TELEMETRY_ENABLED=1.

    ```bash
    make ANP_TELEMETRY_ENABLED=1 RCCL_HOME=$RCCL_HOME
    ```
    If successful, you will see `librccl-anp.so` in the `build/` folder of this plugin project.

    **Example:**
    ```bash
    make ANP_TELEMETRY_ENABLED=1 RCCL_HOME=/home/user/rccl-src/

---

## Install Instructions
To install the plugin into your ROCm library path, run:
```bash
sudo make RCCL_HOME=$RCCL_HOME ROCM_PATH=/path/to/rocm install
```
This copies `librccl-anp.so` to `<ROCM_PATH>/lib`.
`<ROCM_PATH>` defaults to `/opt/rocm` unless overridden by `ROCM_PATH`.

To load this specific AINIC plugin library when running RCCL, set the env `-x NCCL_NET_PLUGIN=librccl-anp.so`.
Setting this env, RCCL will load this specific AINIC plugin library instead of the default plugin library `librccl-net.so`.

---

## Cleanup Instructions

### `clean` Target

The `clean` target is used to remove the build directory and its contents. This is useful for starting a fresh build or for cleaning up intermediate build artifacts.

**Usage:**
```bash
make clean
```

### `uninstall` Target

The uninstall target is used to remove the compiled plugin library `librccl-anp.so` from the installation path `<ROCM_PATH>/lib`

**Usage:**
```bash
make uninstall
```

---

## Enabling Telemetry
AMD ANP plugin provides telemetry capabilities for monitoring device status and performance. The telemetry data is captured and stored in JSON format, giving insights into communication efficiency and queue pair operations. This feature is part of the supported telemetry suite and helps in performance analysis and debugging.

To enable telemetry, the plugin must be compiled with ANP_TELEMETRY_ENABLED=1.
It then reads its configuration from a JSON file, whose location is specified by an environment variable RCCL_ANP_CONFIG_FILE.
In the absence of the environment variable RCCL_ANP_CONFIG_FILE or the JSON file being unreadable, plugin uses defaults for the configuration.

```
export RCCL_ANP_CONFIG_FILE=/path/to/config.json
```

### Configuration JSON
```json
{
  "log_level": "ERROR",
  "output_dir": "/tmp",
  "max_buckets": 5,
  "bucket_interval_ns": 30000
}
```
- `log_level`: Specifies the log level for telemetry logs.
- `output_dir`: Specifies the output directory for files generated by plugin.
- `max_buckets`: Maximum number of histogram buckets for latency tracking.
- `bucket_interval_ns`: Time interval (in nanoseconds) for latency bucket division.


## Device Status JSON

AMD ANP plugin generates a JSON file containing detailed device status and performance metrics. The JSON structure provides a hierarchical view of device status, including:
- **Device metadata** (host, process details, RoCE device, etc.).
- **Channel-level stats**, including queue pairs.
- **Queue pair stats**, covering WQE send/receive/completion data.
- **Latency histogram** for WQE completions.
- **Aggregated statistics**, including WQE size distribution and overall counts.

This generated data serves as part of the supported telemetry features. It can be used to monitor network performance, analyze latencies, and optimize communication between devices.

### Structure Overview
The JSON contains a list of `devices`, where each device has:
- **Status Information**: Metadata about the device and the running process.
- **Channels**: Each device contains multiple channels.
- **Queue Pairs (QP)**: Each channel has multiple queue pairs that handle communication.
- **Statistics**: Various performance metrics are recorded at different levels.

### Device-Level Information

Each device contains the following information under the `status` key:

| Key             | Description |
|----------------|-------------|
| `host_name`    | Host machine name running the process |
| `process_name` | Name of the running process |
| `process_id`   | Process ID |
| `start_time`   | Start time of the process |
| `end_time`     | End time of the process |
| `device_id`    | Unique identifier of the device |
| `eth_device`   | Ethernet device name (if applicable) |
| `roce_device`  | RoCE (RDMA over Converged Ethernet) device name |
| `num_channels` | Number of channels in the device |

Example:
```json
"status": {
    "host_name": "test1",
    "process_name": "all_reduce_perf",
    "process_id": "155048",
    "start_time": "2025-03-12 05:25:58",
    "end_time": "2025-03-12 05:26:15",
    "device_id": "0",
    "eth_device": "",
    "roce_device": "roce_ai3",
    "num_channels": "16"
}
```
---

### Channel-Level Information

Each device has multiple `channels`, where each channel contains:
- **Queue Pairs (`queue_pairs`)**: Communication endpoints for message exchanges.
- **Statistics (`stats`)**: Aggregated stats across queue pairs in the channel.

| Key                | Description |
|--------------------|-------------|
| `id`              | Unique channel identifier |
| `num_queue_pairs` | Number of queue pairs in the channel |
| `queue_pairs`     | List of queue pairs in this channel |
| `stats`           | Aggregated statistics for the channel |

#### Channel Statistics
| Key                 | Description |
|---------------------|-------------|
| `num_wqe_sent`     | Number of WQEs (Work Queue Entries) sent |
| `num_wqe_rcvd`     | Number of WQEs received |
| `num_cts_sent`     | Number of CTS messages sent |
| `num_data_qp`      | Number of data queue pairs |
| `num_cts_qp`       | Number of CTS queue pairs |

Example:
```json
"stats": {
    "num_wqe_sent": "4752",
    "num_wqe_rcvd": "4752",
    "num_cts_sent": "59232",
    "num_data_qp": "1",
    "num_cts_qp": "1"
}
```

---

### Queue Pair (QP) Information

Each `queue_pairs` entry contains:
- **Queue Pair ID**
- **Status**
- **Statistics**

| Key            | Description |
|---------------|-------------|
| `id`          | Unique queue pair identifier |
| `status`      | Contains additional queue pair-specific info |
| `stats`       | Performance metrics for this queue pair |

#### Queue Pair Statistics

| Key                        | Description |
|----------------------------|-------------|
| `num_wqe_sent`            | Number of WQEs sent |
| `num_wqe_rcvd`            | Number of WQEs received |
| `num_wqe_completed`       | Number of WQEs completed |
| `num_slot_miss`           | Number of slot misses |
| `wqe_completion_ns_min`   | Minimum WQE completion latency (ns) |
| `wqe_completion_ns_max`   | Maximum WQE completion latency (ns) |
| `wqe_completion_metrics`  | Histogram of WQE completion latencies |

Example:
```json
"stats": {
    "num_wqe_sent": "4752",
    "num_wqe_rcvd": "4752",
    "num_wqe_completed": "4752",
    "num_slot_miss": "0",
    "wqe_completion_ns_min": "8362",
    "wqe_completion_ns_max": "101821",
    "wqe_completion_metrics": [
        {
            "latency_in_ns": "32767",
            "num_wqe": "4173"
        },
        {
            "latency_in_ns": "65535",
            "num_wqe": "527"
        }
    ]
}
```
---
