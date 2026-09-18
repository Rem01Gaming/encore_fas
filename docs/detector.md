# Encore FAS Frame Interval Detector

This document describes the detector algorithm of the Encore FAS kernel module.
The detector reads the time of each frame and reports events to a user-space daemon.
The file kernel/encore_fas_det.h is the source of truth.

## 1. Scope

1. The input is the time of each call to `Surface::queueBuffer` in one game process.
2. Time is a tick count of the arm64 virtual counter (`CNTVCT_EL0`). All durations in this document are in ticks. Event timestamps use `CLOCK_MONOTONIC` in nanoseconds.
3. The daemon supplies 1 to 8 legal frame rates. The daemon can also supply the display vsync period and a lock flag.
4. The detector does not learn a baseline from the frame stream. The target list alone defines healthy output.
5. The detector reports events only. The daemon decides the CPU and GPU boost policy.
6. All calculations use integer arithmetic.
7. One detector instance (a listener) serves one game process. The detector sees one frame stream per listener. Frames from several surfaces mix into that stream.
8. The vsync period stays constant until the next configuration.

## 2. Notation

### 2.1 Terms

| Term | Meaning |
| ----- | ----- |
| Interval | The time between two consecutive frames. |
| Target | A legal frame rate from the target list. |
| Active target | The target that the detector uses now. |
| Period | The ideal interval of a target. |
| Slot | One period of time in which the game must queue one frame. |
| Hitch | An interval that is at least one slot longer than the reference interval, after the margin. |
| Missed slots | The number of slots that a hitch loses. |
| Deficit | A sustained mean interval that is longer than the active period by more than the tolerance. |
| Window | A time span over which the detector calculates the mean interval. |
| Tolerance | The deficit that the detector accepts. It is 5% of the period. |
| Margin | The extra time that an interval can use before it is a hitch. |
| Listener | The detector state for one game process. |

### 2.2 Symbols

| Symbol | Meaning | Unit |
| ----- | ----- | ----- |
| $F$ | Counter frequency | Hz |
| $t_i$ | Time of frame $i$ | ticks |
| $d_i$ | Interval, $t_i - t_{i-1}$ | ticks |
| $f_k$ | Rate of target $k$ | fps |
| $P_k$ | Period of target $k$ | ticks |
| $B_k$ | Half width of the band of target $k$ | ticks |
| $A$ | Index of the active target | none |
| $P$ | Period of the active target, $P_A$ | ticks |
| $V$ | Vsync period | ticks |
| $c$ | Moving average of the interval | ticks |
| $a$ | Moving mean absolute deviation of the interval | ticks |
| $R$ | Reference interval | ticks |
| $H$ | Margin | ticks |
| $m_i$ | Missed slots of interval $i$ | count |
| $x_i$ | Normalized excess of interval $i$ | Q16 |
| $S$ | CUSUM value | Q16 |
| $\epsilon$ | Tolerance, 0.05 | ratio |
| $h$ | CUSUM alarm limit, 1.0 | ratio |
| $W$ | Window length | ticks |
| $n$ | Number of intervals in a window | count |
| $\mu_w$ | Mean interval of a window | ticks |
| $\rho_w$ | $\mu_w / P$ | Q16 |
| $T_{soft}$, $T_{hard}$, $T_{pause}$ | Watchdog delays | ticks |

Q16 is a fixed-point number with 16 fractional bits. The value $1.0$ is $65536$.

## 3. Configuration

### 3.1 Inputs

| Input | Meaning |
| ----- | ----- |
| `fps[]`, `count` | The legal frame rates. |
| `vsync_ns` | The display vsync period in nanoseconds. Zero selects the default. |
| `FAS_CFG_LOCK_DOWN` | Forbids a switch to a slower target. |

### 3.2 Validation

The module rejects a configuration with `-EINVAL` if one of these conditions is true:

1. `count` is 0 or more than 8.
2. A rate is 0 or more than 1000.
3. `flags` has a bit other than `FAS_CFG_LOCK_DOWN`.
4. `vsync_ns` is not 0 and its period in ticks is less than $F / 1000$ or more than $F / 10$.
5. Two adjacent periods (after sorting) are less than 12% apart:

$$
100 P_{k+1} < 112 P_k
$$

A rejected configuration changes no state.

### 3.3 Derived Settings

The detector sorts the rates from fastest to slowest. Target 0 is the fastest. For each target:

$$
P_k = \left\lfloor \frac{F + \lfloor f_k / 2 \rfloor}{f_k} \right\rfloor
\qquad
B_k = \left\lfloor \frac{5 P_k}{100} \right\rfloor
$$

The detector also sets these values:

| Value | Definition |
| ----- | ----- |
| $V$ | The supplied vsync period. If none, $P_0$. |
| $W_{min}$ | $F / 4$ (250 ms). |
| $T_{pause}$ | $\max(F, 10 P_{n-1})$, where $P_{n-1}$ is the slowest period. |
| Active target | Target 0, until acquisition selects one. |
| $W$ | $\max(W_{min}, 8P)$. It follows the active target. |

The band of target $k$ is $[P_k - B_k, P_k + B_k]$. The bands of two targets never overlap, because $P_{k+1} / P_k \ge 1.12$ and the bands overlap only if the ratio is at most $1.105$.

### 3.4 Meaning of the List

Operation at any listed rate is healthy. A game that must keep one rate needs a list with one entry. A list with several entries lets the detector follow the game between tiers.

The lock flag stops a switch to a slower target. With the lock, a game that drops to a slower listed rate stays degraded.

### 3.5 Reset

A new configuration and the first registration reset the detector:

1. Set all state to zero.
2. Set `acquiring` to 1.
3. Set `pending` to none.
4. Set the watchdog stage to `IDLE`.

A new configuration keeps the event sequence number `seq`.

## 4. State

| Variable | Meaning |
| ----- | ----- |
| `last` | Time of the last frame. |
| `have_last` | The detector has seen one frame. |
| `acquiring` | The detector has not selected its first active target. |
| `win_start`, `win_n` | Start time and interval count of the current window. |
| `c_q4` | $16c$. |
| `a_q4` | $16a$. |
| `S` | The CUSUM value in Q16. |
| `degraded` | The game is slower than the active target. |
| `paused` | The watchdog reported a pause. |
| `wd_stage` | `ARMED`, `SOFT_SENT`, `HARD_SENT` or `IDLE`. |
| `ok_windows` | Consecutive clean windows. |
| `pending`, `pending_n` | Candidate target for a rate switch, and the number of consecutive windows that match it. |
| `seq` | Sequence number of the last event. |

## 5. Structure

### 5.1 Pipeline

```
frame time t_i
    |
    v
interval d_i = t_i - t_(i-1)
    |
    +--> gap check (d_i >= T_pause) ------> resync, RESUMED
    |
    +--> hitch detector (each interval) --> SMALL_JANK, BIG_JANK
    |
    +--> deficit detector (CUSUM) --------> DEGRADED
    |
    +--> window estimator (each window) --> RECOVERED, RATE_SWITCH

watchdog timer (restarted at each frame) --> BOOST_SOFT, BOOST_HARD, PAUSED
```

### 5.2 State Machine

```
                 configuration
                       |
                       v
                +-------------+
                |  ACQUIRING  |
                +-------------+
                       | first window closes (RATE_SWITCH)
                       v
  +-------------+  CUSUM alarm (DEGRADED)   +-------------+
  |   HEALTHY   |-------------------------->|  DEGRADED   |
  |             |<--------------------------|             |
  +-------------+  2 clean windows          +-------------+
        |          (RECOVERED)                     |
        |          or rate switch                  |
        |                                          |
        +----- no frame for T_pause (PAUSED) ------+
                          |
                          v
                     +---------+
                     | PAUSED  |--- next frame (RESUMED) ---> HEALTHY
                     +---------+
```

The `PAUSED` state clears the `DEGRADED` state. If the game is still slow after the pause, the CUSUM raises a new alarm.

## 6. Frame Procedure

The detector runs this procedure for each frame with time $t$. The procedure returns the delay of the watchdog timer. A return value of 0 means that the timer stays as it is.

1. If `have_last` is 0, set `have_last` and `acquiring` to 1. Set `last` and `win_start` to $t$. Set `win_n` to 0. Return 0.
2. Calculate $d = t - \texttt{last}$. If $d \le 0$, discard the frame and return 0. Two CPUs can read the counter in the opposite order of their lock order.
3. Set `last` to $t$.
4. If `paused` is 1 or $d \ge T_{pause}$, run the resync procedure (Section 11). Return 0 if `acquiring` is 1. Otherwise return $T_{soft}$.
5. If `acquiring` is 1:
   1. Increment `win_n`.
   2. If $t - \texttt{win\_start} < W_{min}$, return 0.
   3. Run the acquisition procedure (Section 7). Return $T_{soft}$.
6. Set the event flag `FAS_EVF_WATCHDOG` if `wd_stage` is not `ARMED`. Set `wd_stage` to `ARMED`.
7. Calculate $R$ and $H$ (Section 8).
8. If $d \ge R + H$, run the hitch procedure (Section 9). Otherwise update $c$ and $a$ (Section 8.3).
9. If `degraded` is 0, add $d$ to the CUSUM (Section 10). If the CUSUM alarms, emit `DEGRADED`. Set `degraded` to 1 and $S$ to 0.
10. Increment `win_n`. If $t - \texttt{win\_start} \ge W$, run the window procedure (Section 12).
11. Return $T_{soft}$, calculated from the state after steps 7 to 10.

A frame can produce at most 4 events. The detector discards events after the fourth.

## 7. Acquisition

Acquisition selects the first active target from one window of frames.

1. The first frame gives no interval and only starts the window (Section 6, step 1).
2. Each later frame adds one interval. Let $n$ be the number of intervals.
3. When $t - \texttt{win\_start} \ge W_{min}$, calculate $\mu = \lfloor (t - \texttt{win\_start}) / n \rfloor$.
4. If $\mu$ is in the band of target $k$, select target $k$.
5. Otherwise, select the target with the largest period that is not more than $\mu$. If all periods are more than $\mu$, select target 0.
6. Set `acquiring` to 0 and `wd_stage` to `ARMED`.
7. Run the switch procedure (Section 12.3). This emits `RATE_SWITCH`.
8. Set `win_start` to `last` and `win_n` to 0.

Step 5 assumes that a game between two listed rates aims at the faster rate and fails to reach it. The CUSUM then reports `DEGRADED`.

The watchdog emits no event while `acquiring` is 1.

## 8. Reference Interval and Margin

### 8.1 Reference Interval

$$
R = \min\bigl(\max(P, c),\ 2P\bigr)
$$

$R$ follows the recent interval of the game. It is never less than $P$ and never more than $2P$. The deficit detector always uses $P$. A rise of $R$ therefore cannot hide a deficit.

### 8.2 Margin

$$
H = \min\bigl(\max(V / 2,\ N a),\ R\bigr) \qquad N = 6
$$

1. The floor $V / 2$ is halfway between an interval on time and an interval one vsync late.
2. The term $N a$ follows the noise of the intervals.
3. The cap $R$ keeps the hitch threshold below $2R$.

### 8.3 Update

The detector updates $c$ and $a$ only with intervals that are not hitches. It uses $c$ before the update to calculate the deviation.

$$
dev = \min(\lvert d - c \rvert,\ V)
$$

$$
a \leftarrow a + \frac{dev - a}{16}
\qquad
c \leftarrow c + \frac{d - c}{8}
$$

The cap $V$ limits the effect of the short catch-up intervals after a burst hitch.

Initial values: the switch and resync procedures set $c = P$. The setup procedure sets $a = 0$. A switch does not change $a$.

## 9. Hitch Detector

An interval is a hitch when $d \ge R + H$. The number of missed slots is:

$$
m = \left\lfloor \frac{d - H}{R} \right\rfloor \qquad (m \ge 1)
$$

| Missed slots | Event | Rule |
| ----- | ----- | ----- |
| $1 \le m \le 2$ | `SMALL_JANK` | Suppressed while `degraded` is 1. |
| $m \ge 3$ | `BIG_JANK` | Always emitted. |

The event carries $d$, $\min(m, 65535)$ and the flag from Section 6, step 6.

### 9.1 Thresholds

The values below assume low noise, that is $H = V / 2$. The big hitch threshold is $3R + H$.

| Target | $V$ | $H$ | Hitch at $d \ge$ | Big hitch at $d \ge$ |
| ----- | ----- | ----- | ----- | ----- |
| 30 fps | 16.67 ms | 8.33 ms | 41.7 ms | 108.3 ms |
| 60 fps | 16.67 ms | 8.33 ms | 25.0 ms | 58.3 ms |
| 60 fps | 8.33 ms | 4.17 ms | 20.8 ms | 54.2 ms |
| 120 fps | 8.33 ms | 4.17 ms | 12.5 ms | 29.2 ms |
| 144 fps | 6.94 ms | 3.47 ms | 10.4 ms | 24.3 ms |

## 10. Deficit Detector

The deficit detector is a one-sided CUSUM chart. It runs only while `degraded` is 0.

$$
x_i = \frac{\min(d_i,\ 1.5P) - P}{P} \quad \text{(Q16)}
$$

$$
S_i = \max(0,\ S_{i-1} + x_i - \epsilon)
$$

The detector raises an alarm when $S_i \ge h$. On alarm, it emits `DEGRADED`, sets `degraded` to 1 and sets $S$ to 0. The window estimator ends the `DEGRADED` state (Section 12.2).

| Parameter | Effect |
| ----- | ----- |
| $\epsilon = 0.05$ | Excess below the tolerance does not accumulate. |
| $h = 1.0$ | The alarm needs a total excess of one slot above the tolerance. |
| Upper clip $1.5P$ | One interval adds at most $0.5 - \epsilon = 0.45$ to $S$. One or two hitches cannot alarm. Three can. |
| Lower bound $-1.0$ | It follows from $d > 0$. One interval lowers $S$ by at most $1.0 + \epsilon$. |

### 10.1 Pressure

The pressure of a listener is $\min(S, 1.0)$ in Q16. The value 65536 means that the next alarm is due. Pressure is 0 while `degraded` is 1.

### 10.2 Properties

For a sustained excess $\delta = (d - P) / P$, the number of frames to an alarm is:

$$
N_{det} = \left\lceil \frac{h}{\min(\delta, 0.5) - \epsilon} \right\rceil
$$

The detection time is $N_{det} P (1 + \delta)$. A constant excess of at most $\epsilon$ never alarms.

A burst hitch is a late frame that is followed by short intervals. Its excess and its catch-up cancel. The CUSUM ignores a burst hitch. A shift hitch delays all later frames. It loses slots for good and counts toward the CUSUM.

Timestamp jitter cannot cause a drift of $S$. Let the timestamp be $t_i = n_i + e_i$, where $n_i$ is the ideal time and $e_i$ is independent jitter with mean 0. The interval is $d_i = P + e_i - e_{i-1}$. The sum of $L$ intervals is $LP + e_n - e_{n-L}$. It depends on two jitter values only.

## 11. Pause and Resume

An interval of $T_{pause}$ or more is a pause, not a hitch. The detector runs the resync procedure for the first frame after a pause:

1. If `paused` is 1, emit `RESUMED`.
2. Discard the interval.
3. Set `paused`, `degraded`, `ok_windows`, `S` and `pending_n` to 0. Set `pending` to none.
4. Set `win_start` to $t$ and `win_n` to 0.
5. If `acquiring` is 0, set `wd_stage` to `ARMED` and set $c = P$.

The detector emits no hitch event for a pause. If a late timer misses the pause, the detector still resyncs. It then emits no `RESUMED` event.

## 12. Window Estimator

### 12.1 Definition

A window starts at a frame with time $t_s$. It ends at the first frame with time $t_e$ such that $t_e - t_s \ge W$. The next window starts at that frame.

$$
\mu_w = \left\lfloor \frac{t_e - t_s}{n} \right\rfloor
\qquad
\rho_w = \left\lfloor \frac{(t_e - t_s) \cdot 65536}{n P} \right\rfloor
$$

The window includes all intervals, including hitches. The sum of the intervals is $t_e - t_s$, so a burst hitch and its catch-up cancel in $\mu_w$. The error of $\mu_w$ that comes from jitter is $\sqrt{2}\,\sigma_t / n$, where $\sigma_t$ is the standard deviation of the timestamp jitter.

When a window ends, the detector runs the recovery check, then the rate tracking, then it starts the next window.

### 12.2 Recovery

The detector runs this check only while `degraded` is 1.

1. If $\rho_w \le 1 + \epsilon$, increment `ok_windows`. Otherwise set `ok_windows` to 0.
2. When `ok_windows` reaches 2, set `degraded`, `ok_windows` and $S$ to 0. Emit `RECOVERED` with $\mu_w$.

The exit threshold equals the entry tolerance. A deficit of more than $\epsilon$ raises and holds the `DEGRADED` state. A deficit of less than $\epsilon$ does neither.

### 12.3 Rate Tracking

1. Find the target $k$ whose band contains $\mu_w$. Test the targets from fastest to slowest.
2. If no band contains $\mu_w$, or $k = A$, set `pending` to none and `pending_n` to 0. Stop.
3. If $k \ne$ `pending`, set `pending` to $k$ and `pending_n` to 0.
4. Increment `pending_n`. The counter stops at 255.
5. If $P_k > P$ (a slower target) and the lock flag is set, stop.
6. Set $need = 4$ for a slower target and $need = 2$ for a faster target.
7. If `pending_n` $\ge need$, run the switch procedure.

**Switch procedure.** To switch to target $k$:

1. Set the active target to $k$. Set $P$, the reciprocal of $P$ and $W$ from the new target.
2. Set $c = P$.
3. Set `pending` to none. Set `pending_n`, `degraded`, `ok_windows` and $S$ to 0.
4. Emit `RATE_SWITCH`. The `fps` field holds the new rate.

A slower target needs more windows, because a slower rate can hide a real failure.

## 13. Watchdog

The watchdog reports a late frame before the frame arrives. The frame procedure restarts the timer at each frame.

### 13.1 Delays

$$
T_{soft} = R + H
\qquad
T_{hard} = 3R + H
\qquad
T_{pause} = \max(F,\ 10 P_{n-1})
$$

All delays count from the last frame. $R$ and $H$ come from the state at the time of the call.

### 13.2 Timer Procedure

The timer callback calculates $e = \max(0, now - \texttt{last})$ and acts on `wd_stage`:

| Stage | Condition | Action |
| ----- | ----- | ----- |
| `ARMED` | $e < T_{soft}$ | Do nothing. Return 0. |
| `ARMED` | $e \ge T_{soft}$ | Set `SOFT_SENT`. Emit `BOOST_SOFT`. Return $\max(T_{hard} - e, 1)$. |
| `SOFT_SENT` | $e < T_{hard}$ | Return $T_{hard} - e$. |
| `SOFT_SENT` | $e \ge T_{hard}$ | Set `HARD_SENT`. Emit `BOOST_HARD`. Return $\max(T_{pause} - e, 1)$. |
| `HARD_SENT` | $e < T_{pause}$ | Return $T_{pause} - e$. |
| `HARD_SENT` | $e \ge T_{pause}$ | Set `IDLE` and `paused`. Emit `PAUSED`. Go to idle poll. |
| `IDLE` | Any | Go to idle poll. |

**Idle poll.** The callback marks the call as idle and returns $8 W_{min}$ (2 s). The caller checks that the game process is alive. If the process is dead, the caller removes the listener. Otherwise it arms the timer again.

The registration and each new configuration arm the timer with the idle poll delay.

### 13.3 Duplicate Events

A stall that ends after $T_{soft}$ produces a watchdog event and a hitch event. The hitch event has the flag `FAS_EVF_WATCHDOG` when `wd_stage` is `SOFT_SENT` or `HARD_SENT` at the frame. The daemon must not count such a stall twice.

## 14. Events

### 14.1 Event Table

| Event | Source | Condition | `frametime` value | Suggested daemon action |
| ----- | ----- | ----- | ----- | ----- |
| `BOOST_SOFT` | Watchdog | No frame for $R + H$ | Time since the last frame | Short boost |
| `BOOST_HARD` | Watchdog | No frame for $3R + H$ | Time since the last frame | Strong boost |
| `PAUSED` | Watchdog | No frame for $T_{pause}$ | Time since the last frame | Remove boosts |
| `SMALL_JANK` | Frame | $1 \le m \le 2$ | $d$ | Log, or minor boost |
| `BIG_JANK` | Frame | $m \ge 3$ | $d$ | Strong boost |
| `DEGRADED` | Frame | CUSUM alarm | $d$ | Raise the base performance level |
| `RECOVERED` | Window | 2 clean windows | $\mu_w$ | Lower the base performance level |
| `RESUMED` | Frame | First frame after `PAUSED` | 0 | Restart the model |
| `RATE_SWITCH` | Acquisition, window | Active target changes | 0 | Update the model |

### 14.2 Payload

The module converts ticks to nanoseconds for the payload. It sets the fields when it queues the event.

| Field | Value |
| ----- | ----- |
| `timestamp_ns` | `CLOCK_MONOTONIC` time at queueing. |
| `frametime_ns` | The `frametime` value of the event. |
| `fps` | The active target at emission. |
| `missed` | $\min(m, 65535)$ for hitch events. Otherwise 0. |
| `flags` | Bit 0 is `FAS_EVF_WATCHDOG`. |
| `pressure_q16` | Pressure (Section 10.1) after the call that made the event. |
| `seq` | The listener counter, incremented for each event. |

### 14.3 Delivery

1. All listeners share one queue of 512 events.
2. If the queue is full, the module discards the oldest event and increments a drop counter.
3. A gap in `seq` shows that the daemon lost events. The daemon then calls `FAS_IOC_GET_STATE`. It returns `fps`, the state flags (`ACQUIRING`, `DEGRADED`, `PAUSED`), `pressure_q16`, `seq` and the drop counter.
4. `read()` returns up to 8 events. The buffer must hold at least one 48-byte event. Otherwise `read()` returns `-EINVAL`.

### 14.4 Volume

A healthy game produces no events. The worst case for a stream is:

| Source | Bound |
| ----- | ----- |
| Hitch | 1 per frame. Only `BIG_JANK` while `degraded` is 1. |
| Window | 2 per window (`RECOVERED`, `RATE_SWITCH`). |
| Watchdog | 3 per stall. |

## 15. Integer Arithmetic

1. The module divides 64-bit values with `div_u64()`, `div64_u64()` and `div64_s64()`.
2. The reciprocal $\lfloor 2^{48} / P \rfloor$ is stored for the active target. The CUSUM excess is:

$$
x = \frac{\bigl(\min(d, 1.5P) - P\bigr) \cdot \lfloor 2^{48} / P \rfloor}{2^{32}}
$$

3. The moving averages use signed 64-bit values with an arithmetic right shift:

```
dev_q4 += ((dev << 4) - dev_q4) >> 4
c_q4   += ((d   << 4) - c_q4)   >> 3
```

4. The module requires that a right shift of a negative signed value keeps the sign. The Linux kernel guarantees this.
5. Every interval that reaches these calculations is less than $T_{pause}$. For $F \le 4$ GHz and a 1 fps target, $T_{pause}$ is $4 \times 10^{10}$ ticks. A shift by 16 bits then stays far below $2^{63}$. The CUSUM product has a magnitude of at most $2^{48}$.
6. The tick to nanosecond conversion uses a multiplier and a shift. It limits the input to 60 s of ticks.
7. The module uses `CNTFRQ_EL0` for $F$. If the register is out of range or differs from a measured value by more than 10%, the module uses the measured value rounded up to 10 kHz.

## 16. Concurrency

1. Each listener has one raw spinlock. All access to its detector state uses `raw_spin_lock_irqsave()`.
2. A global mutex protects the listener table. Code takes the mutex before a listener lock.
3. **Frame handler.** The handler ignores calls from other thread groups. It takes the lock, runs the frame procedure, queues the events and arms the timer if the delay is not 0. It releases the lock, then wakes the readers.
4. **Timer callback.** The callback takes the lock, runs the timer procedure, queues the events and arms the timer if the delay is not 0. It releases the lock, then wakes the readers. The callback does not use the automatic restart of the timer.
5. **Configuration.** The module builds the new detector on the stack. It then takes the lock, copies the new state, keeps `seq` and arms the idle poll.
6. The timer uses a slack of $1/16$ of its delay.

## 17. Parameters

| Parameter | Default | Unit | Effect of a larger value |
| ----- | ----- | ----- | ----- |
| `FAS_TOL_PCT` ($\epsilon$) | 5 | % of $P$ | Fewer deficit alarms. Wider rate bands. Slower recovery check. |
| `FAS_MIN_RATIO_PCT` | 112 | % | Stricter target list. |
| `FAS_NOISE_MULT` ($N$) | 6 | none | Fewer false hitches. Less hitch sensitivity in noisy streams. |
| `FAS_MISS_BIG` | 3 | slots | Fewer `BIG_JANK` events. Later `BOOST_HARD`. |
| CUSUM clip | 0.5 | $P$ | Faster alarms from large single hitches. |
| CUSUM limit $h$ | 1.0 | $P$ | Slower deficit alarms. |
| Minimum window $W_{min}$ | $F/4$ | ticks | More precise mean. Slower decisions. |
| `FAS_WIN_PERIODS` | 8 | $P$ | Longer windows for slow targets. |
| `FAS_OK_WINDOWS` | 2 | windows | Slower recovery. |
| `FAS_UP_WINDOWS` | 2 | windows | Slower switch to a faster target. |
| `FAS_DOWN_WINDOWS` | 4 | windows | Slower switch to a slower target. |
| Pause floor | $F$ | ticks | Later `PAUSED` event. |
| `FAS_PAUSE_PERIODS` | 10 | $P_{n-1}$ | Later `PAUSED` event for slow targets. |
| `FAS_IDLE_POLL_WINS` | 8 | $W_{min}$ | Slower detection of a dead process. |
| Cadence weight | 1/8 | none | Faster response of $R$. |
| Deviation weight | 1/16 | none | Faster response of $a$. |
| $R$ cap | 2 | $P$ | Higher hitch thresholds in a degraded game. |
| `FAS_OUT_MAX` | 4 | events | Larger output per call. |

## 18. False Hitch Probability

Assume Gaussian timestamp jitter with standard deviation $\sigma_t$. The interval jitter has standard deviation $\sigma_d = \sqrt{2}\,\sigma_t$. The detector estimates it as $\sigma_d \approx 1.2533\,a$.

The probability that noise alone makes one interval a hitch is:

$$
p = Q\!\left(\frac{H}{\sigma_d}\right)
$$

$Q(z)$ is the probability that a standard normal variable is more than $z$. A game at $f$ fps has a mean time of $1 / (f p)$ between false hitches. With $N = 6$, $H / \sigma_d = 4.79$ and $p = 8.3 \times 10^{-7}$.

A false CUSUM alarm at lag $L$ needs $e_n - e_{n-L} \ge (h + L\epsilon) P$. The probability per frame is at most:

$$
\sum_{L \ge 1} Q\!\left(\frac{(h + L\epsilon) P}{\sqrt{2}\,\sigma_t}\right)
$$

The term for $L = 1$ dominates. The rule $\sigma_t \le 0.15 P$ keeps this value very small.
