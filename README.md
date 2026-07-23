 # esp32-freertos-latency-lab

Measuring and defending real-time determinism on an ESP32 under FreeRTOS.

Most embedded portfolio projects stop at "it works." This one asks a harder
question: **when it works, how do you know — and by how much?** The system under
test is a periodic sense-compute-respond pipeline. The point of the repository is
the instrumentation wrapped around it, the deliberate load applied to it, and the
before/after numbers for every fix.

> **Status: week 1 of 4 — in progress.**
> The trigger source and interrupt-to-task path are working and characterized in
> simulation. Nothing has run on physical hardware yet. All figures below are
> labelled with where they came from.

---

## The idea

Three parts, built in this order:

1. **The work.** A hardware event arrives 1000 times per second. Firmware must
   respond, every time, on time. Being late once is a failure even when the
   answer is right.
2. **The witness.** Separate instrumentation records how long every stage took,
   for every event, without perturbing what it measures.
3. **The adversary.** Competing workloads — bus contention, blocking drivers,
   flash writes, network traffic — switched on and off at runtime while the
   witness keeps recording.

The deliverable is the evidence, not the pipeline.

---

## Current architecture

```
LEDC peripheral ──> GPIO 25 ──jumper──> GPIO 26
                                          │
                                    edge interrupt
                                          │
                                    trig_isr (IRAM)
                                          │
                              vTaskNotifyGiveFromISR
                                          │
                              workTask (prio 20, core 1)
                                          │
                                     GPIO 27 pulse
```

**Trigger.** The LEDC PWM peripheral generates a 1 kHz square wave at 50% duty on
GPIO 25, jumpered to GPIO 26. LEDC is a hardware peripheral, so the waveform holds
its timing regardless of what the CPU is doing — that independence is what makes it
usable as a measurement reference. It stands in for a sensor's data-ready pin and
will be replaced by one without changing anything downstream.

**Interrupt path.** `trig_isr` is placed in IRAM. This is required, not an
optimization: when flash is being written or erased the instruction cache is
disabled, and a flash-resident handler would be masked out for milliseconds or
panic outright. The handler stays minimal — timestamp, notify, yield.

**Task.** `workTask` blocks on a task notification, consuming zero CPU while
waiting. `portYIELD_FROM_ISR` forces the reschedule on ISR exit rather than at the
next scheduler tick, which is the difference between microsecond and millisecond
response.

**Response pin.** GPIO 27 is driven high for the duration of the work via direct
register writes (`GPIO.out_w1ts` / `out_w1tc`) rather than `digitalWrite`, which is
too slow to sit inside the measurement. Trigger-edge to response-rise is latency;
the width of the response pulse is execution time.

---

## Measurement methodology

Two independent measurement paths, deliberately:

| Path | Measures | Sees | Available on |
| --- | --- | --- | --- |
| Logic analyzer / VCD | pin edge to pin edge | full path incl. hardware dispatch | simulation only |
| On-chip timestamps | ISR entry to task wake | software path only | simulation + hardware |

Neither is trusted alone. Their difference isolates the interrupt dispatch cost,
and agreement between two independent methods is the strongest available evidence
that the instrumentation is sound.

Every capture discards a warm-up window before recording. Startup runs with cold
caches and half-created tasks, and those samples would otherwise set a worst-case
figure unrelated to steady-state behaviour.

---

## Results

### Simulation — Wokwi, 5127 events

| Metric | min | mean | median | p99 | max | σ |
| --- | --- | --- | --- | --- | --- | --- |
| Trigger period (µs) | 999.253 | 1000.000 | 1000.000 | 1000.001 | 1000.001 | 0.010 |
| Latency (µs) | 31.941 | 31.942 | 31.942 | 31.942 | 32.188 | 0.008 |

Unanswered triggers: 0 of 5127.

**Reading these honestly.** The trigger generator is clean — σ of 10 ns is the
VCD's own resolution, so the reference clock is as good as the format can express.

The latency figures are *not* a physical result. A spread of one nanosecond across
five thousand events is not something silicon does; it is the signature of a
deterministic emulator recomputing the same path with the same modelled costs.
Simulation validated the **logic** — the interrupt fires, the notification lands,
the task wakes, and every trigger got a response — but timing must come from
hardware.

This was worth finding early. The project rule is now explicit: **build the shape
in simulation, take every number on hardware.** No figure enters this README
without a source label.

### Hardware

Not yet measured. Pending board bring-up.

---

## Findings log

Short entries, added as they happen. The debugging record is a deliverable.

**LEDC clock source must be pinned.** Left on automatic selection the driver may
choose the internal RC oscillator, which drifts with temperature. Forcing
`LEDC_USE_APB_CLK` ties the reference to the crystal. 80 MHz ÷ (1000 × 1024) =
78.125 divides exactly given LEDC's fractional divider, so the output is 1000.000 Hz
with no inherited rounding error.

**~485 ms of silence before the first edge.** Not a defect — ROM bootloader,
second-stage bootloader, ESP-IDF startup, scheduler start, then `setup()`. Led to
adding an explicit warm-up discard before every capture.

**Designated initializer order.** C99 permits any order; C++20 requires
declaration order. Both LEDC config structs fail to compile in a `.cpp` file if
reordered.

**Probing both ends of a jumper measures nothing.** Two pins joined by a wire are
one electrical node with one voltage. The second analyzer channel belongs on the
response pin, where it captures a different signal.

**Simulation jitter is implausibly low.** σ = 8 ns over 5127 events. Prompted the
simulation/hardware split in methodology above.

---

## Hardware

- ESP32 DevKitC v4 (dual-core Xtensa LX6, 240 MHz)
- Jumper: GPIO 25 → GPIO 26
- GPIO 27 free as the response pin
- Multimeter for DC/frequency cross-checks

Pins 6–11 (flash), 34–39 (input-only) and strapping pins 0, 2, 5, 12, 15 are
avoided. GPIO 26 uses an internal pulldown so a dislodged jumper reads a clean low
and produces zero events rather than plausible-looking noise.

---

## Layout

```
src/main.cpp        firmware
tools/              analysis scripts
  vcd_latency.py    VCD parser: pairs edges, reports latency distribution
data/               committed captures
docs/               diagrams, traces
diagram.json        Wokwi wiring
platformio.ini
```

---

## Build

```bash
pio run                       # build
pio run -t upload             # flash
wokwi-cli .                   # simulate
```

Analyzing a capture:

```bash
python tools/vcd_latency.py wokwi.vcd --trigger D0 --response D1 --csv data/run.csv
```

The parser also reports the trigger period, which is a free check on the reference
clock — if that is not 1000.000 µs with negligible spread, nothing downstream means
anything.

---

## Roadmap

**Week 1 — measurement infrastructure.** Hardware trigger ✅ · interrupt-to-task
path ✅ · VCD analysis ✅ · on-chip timestamping · fixed-bin histogram · committed
baseline.

**Week 2 — pipeline and load harness.** ADC sampling task · filter and PWM output ·
queue-based IPC with drop-on-full telemetry · per-task CPU, stack watermarks, queue
depth · runtime-switchable stressors.

**Week 3 — experiments.** Flash cache stalls under NVS writes · priority inversion
through a shared SPI bus · `printf` in the real-time path · `double` vs `float` on a
single-precision FPU · polling vs DMA for short transfers · Arduino `attachInterrupt`
dispatcher vs directly registered handler. One variable at a time, before and after.

**Week 4 — writeup.** Consolidated results, plots, analyzer traces, unit tests for
the statistics module.

---

## Open questions

- How much of the 32 µs simulated latency is hardware dispatch versus context
  switch? Requires running both measurement paths simultaneously.
- Does Wokwi model deferred context switches at all? Testable by removing
  `portYIELD_FROM_ISR` and checking whether latency jumps toward a tick period.
  If it does not, no scheduling experiment in simulation is meaningful.

---

## License

MIT