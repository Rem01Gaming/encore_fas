# Encore FAS

A kernel module for Frame Aware Scheduling (FAS) on Android.

## What is Frame Aware Scheduling?

Frame Aware Scheduling improves game performance by monitoring the game's framerate in real time and using that information to optimize performance and efficiency.

If the game runs choppy, FAS automatically boosts the CPU and GPU to compensate for the low FPS. Otherwise, it relaxes the boost to save power.

## Why kernel module?

While other successful implementations of FAS on Android (such as `fas-rs`) rely on eBPF to attach uprobes to the game process, Encore FAS implements detection directly inside a loadable kernel module. This design offers several advantages:

* **Broad Legacy Compatibility:** Android devices running older kernels (Linux 4.14–5.4) often lack full eBPF support or have restricted BPF subsystem configurations. A kernel module provides a reliable implementation path across legacy and modern Android GKI kernels alike.
* **Precise Microsecond Timing:** In-kernel uprobe handling enables high-precision timestamps (via `ktime_get_ns()`) directly inside the kernel context, minimizing scheduling latency and jitter before sending event telemetries to userspace.
* **Reduced Userspace Overhead:** Fast-path frame timing, event filtering, state machine evaluation, and watchdog timers run entirely within kernel context, notifying the userspace daemon only when actionable scheduling adjustments or state changes occur.

## Architecture

The kernel module attaches a uprobe to `Surface::queueBuffer` within the target game process. Because the game calls this function once per frame, the module can precisely measure the interval between consecutive calls and compare it against a list of valid target frame rates supplied by a userspace daemon.

### Reported Metrics

The module tracks four primary types of events and reports them to the userspace daemon:

1. **Hitches:** A frame arriving at least one vsync slot late.
2. **Deficits:** Sustained periods where the game renders slower than the active target rate.
3. **Pauses:** Interruptions in rendering (e.g., during loading screens).
4. **Rate Switches:** Shifts in the game's frame rate target.

> **Note:** The kernel module is purely a telemetry system and never directly alters CPU or GPU frequencies, all scheduling decisions are deferred to the userspace daemon.

## Requirements

- **Architecture:** `arm64`
- **Kernel Version:** Linux 4.14 up to the latest Android GKI kernel
- **Kernel Config:** `CONFIG_UPROBES` enabled

## User API

- **Device Node:** `/dev/encore_fas`
- **Header Path:** `kernel/include/uapi/encore_fas_uapi.h`
- **ABI Versioning:** Layout version is defined by `FAS_ABI_VERSION`. Verify compatibility using `FAS_IOC_GET_VERSION`.

### `ioctl` Commands

| Command | Argument Type | Description |
| :--- | :--- | :--- |
| `FAS_IOC_GET_VERSION` | `struct fas_version` | Retrieves driver version, ABI layout version, and counter frequency. |
| `FAS_IOC_REGISTER` | `struct fas_register_args` | Attaches a probe listener to a process; returns a listener ID. |
| `FAS_IOC_REMOVE` | `struct fas_remove_args` | Detaches a listener. |
| `FAS_IOC_SET_CONFIG` | `struct fas_config_args` | Updates target frame rates for an active listener. |
| `FAS_IOC_GET_STATE` | `struct fas_state` | Fetches current state metrics for a listener. |
| `FAS_IOC_LIST` | `struct fas_listener_list` | Enumerates all active listeners. |

`FAS_IOC_REGISTER` requires the target process ID (PID), file path, ELF symbol offset for `Surface::queueBuffer` inside `libgui.so`, and an initial configuration list. The `fas_ctl` CLI utility can resolve symbol offsets automatically.

#### Error Codes

| Error | Description |
| :--- | :--- |
| `EPERM` | Insufficient permissions (caller must be `root`). |
| `ESRCH` | Process ID not found. |
| `EEXIST` | A listener is already attached to the target process. |
| `ENOSPC` | Maximum listener capacity reached (limit: 16). |
| `ENOENT` | Invalid file path or listener ID. |
| `EINVAL` | Invalid target frame rate list, symbol offset, or binary file. |

### Configuration & Target Lists

The target list specifies acceptable target frame rates configured by the daemon.

- **Rate Count:** Accepts 1 to 8 target rates (range: 1 to 1000 FPS per entry).
- **Separation:** Adjacent target periods must differ by at least 12%. Duplicate rates or close intervals (e.g., `{60, 63}`) are rejected.
- **Health Criteria:** Operating at any configured target rate is treated as healthy. For games with a fixed lock, provide a single-element list.
- **VSync Synchronization (`vsync_ns`):** Specifies the display vsync period in nanoseconds. If set to `0`, defaults to the period of the fastest configured target. Explicit configuration is recommended when display refresh rate exceeds the maximum target FPS.
- **Lockdown Mode (`FAS_CFG_LOCK_DOWN`):** Disables automatic downswitching. If enabled, a frame rate drop (e.g., 60 FPS down to 30 FPS) will remain flagged as degraded.

### Event Stream

Reading from `/dev/encore_fas` returns 48-byte `struct fas_event` structures (up to 8 events per single `read()` call). The file descriptor supports non-blocking operations (`O_NONBLOCK`) and multiplexing via `poll()`.

| Event Type | Description | Expected Daemon Action |
| :--- | :--- | :--- |
| `BOOST_SOFT` | Missed frame boundary by 1 period + margin. | Initiate short boost. |
| `BOOST_HARD` | Missed frame boundary by 3 periods + margin. | Initiate maximum boost. |
| `SMALL_JANK` | Frame rendered 1–2 slots late. | Log metric or apply minor frequency adjustment. |
| `BIG_JANK` | Frame rendered $\ge 3$ slots late. | Apply significant performance boost. |
| `DEGRADED` | Average frame time exceeds target by $>5\%$ sustained. | Elevate baseline performance state. |
| `RECOVERED` | Frame time returned within tolerance for 2 consecutive evaluation windows. | Restore default baseline performance. |
| `PAUSED` | No rendering activity detected for 1 second. | Drop active performance boosts. |
| `RESUMED` | First frame rendered following a `PAUSED` state. | Re-initialize performance model. |
| `RATE_SWITCH` | Active target rate changed (new FPS indicated in `fps` field). | Re-calibrate scheduling targets. |

#### Operational Notes

- **Watchdog Deduplication:** Stalls typically emit a `BOOST_SOFT` event followed by a jank event (`SMALL_JANK` or `BIG_JANK`). The subsequent jank event will carry the `FAS_EVF_WATCHDOG` flag to indicate it originates from the same stall.
- **Degraded State Filtering:** While a listener remains in a `DEGRADED` state, minor hitches are suppressed, and only major hitches are dispatched.
- **Pressure Metric:** The `pressure_q16` field (Q16 fixed-point) measures proximity to triggering a `DEGRADED` state ($65536 = 100\%$ threshold). Daemon implementations can use this to scale boost responses preemptively.
- **Warmup Phase:** Event generation begins after approximately 250ms of stable frame output, allowing the module to lock onto the initial target rate.

### Process Lifecycle & Event Buffering

- **Sequence Numbers:** Events include a monotonically increasing sequence number (`seq`). Gaps in sequence numbers indicate queue overflow; callers should query `FAS_IOC_GET_STATE` to reconcile state.
- **Queue Capacity:** The shared kernel event ring buffer holds up to 512 events total across all listeners.
- **Daemon Restarts:** Probes remain active in the kernel if the userspace daemon terminates. Re-attach or inspect active instances using `FAS_IOC_LIST` and `FAS_IOC_SET_CONFIG`.
- **Process Termination:** Listeners are automatically torn down when the target process exits (garbage collection runs within ~3 seconds).

## Control Tool (`fas_ctl`)

### Command Usage

```sh
fas_ctl version
fas_ctl attach <pid> [-l lib] [-o hex_offset | -s symbol] [-v vsync_hz] [-L] <fps>...
fas_ctl config <ctx_id> [-v vsync_hz] [-L] <fps>...
fas_ctl state <ctx_id>
fas_ctl detach <ctx_id>
fas_ctl list
fas_ctl listen [-c count]
fas_ctl selftest [-q]
```

### Quickstart Example

Attach to PID `1234` on a 120 Hz panel with targets set for 60 FPS and 120 FPS:

```sh
# Attach listener
fas_ctl attach 1234 -v 120 60 120

# Stream incoming event notifications
fas_ctl listen
```

## License

Encore FAS is open-sourced software licensed under the [GPL-2.0-only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).
