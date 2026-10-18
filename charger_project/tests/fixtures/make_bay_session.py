#!/usr/bin/env python3
"""Writes tests/fixtures/bay_session.candump, a deterministic `candump -L` recording.

No randomness, no wall clock: the same bytes on every machine. It is a hand-built
DEMO-protocol recording (ids 0x180 measurement, 0x181 status) of about 25 minutes
at 1 Hz with a constant-current / constant-voltage charge profile and ONE
communication dropout, so the replay path can be tested against exact figures:

  t =    0..  59 s   READY, 400.00 V, 0 A
  t =   60.05 s      status CHARGING -> session 1 opens
  t =   60..600 s    CC: current ramps 1.00 A/s... to 125.00 A, voltage rises 0.08 V/s
  t =  600..899 s    CV taper: current falls 0.40 A/s
  t =  900..911 s    SILENCE (12 s, longer than the 3 s stream timeouts): session 1 is
                     closed by COMMUNICATION_TIMEOUT, held for review, never invoiced
  t =  912.05 s      status CHARGING again -> session 2 opens
  t =  912..1440 s   CC at 120.00 A, then taper from t = 1200 s
  t = 1440.05 s      status OFF -> session 2 closes cleanly (billable)
  t = 1441..1500 s   OFF, 0 A

Measurement frames are on whole seconds, status frames 50 ms later. Base time is
2026-10-18T08:00:00Z.

    python3 tests/fixtures/make_bay_session.py            # (re)write the fixture
    python3 tests/fixtures/make_bay_session.py --expect   # independent integer model of the engine

--expect re-derives, with plain Python integers and the documented rules (trapezoid
between consecutive accepted samples, carried remainder, 200 ms idle ticks while the
bus is silent, timeouts of 3000 ms), what the monitor must report for the two
sessions at the default configuration and a tariff of 0.25 EUR/kWh + 3.60 EUR/h +
0.50 EUR flat. tests/test_candump.cpp states the same numbers.
"""
import calendar
import os
import sys

BASE = calendar.timegm((2026, 10, 18, 8, 0, 0, 0, 0, 0))
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "bay_session.candump")


def volts_cv(t):
    """Voltage in centivolts at whole second t."""
    if t < 60:
        return 40000
    if t < 912:
        return 40000 + (t - 60) * 8
    return 44000 + (t - 912) * 2 if t <= 1440 else 40000


def amps_ca(t):
    """Current in centiamps at whole second t."""
    if t < 60 or t > 1440:
        return 0
    if t < 912:
        if t < 185:
            return (t - 60) * 100  # ramp, 1.00 A per second
        if t <= 600:
            return 12500
        return max(500, 12500 - (t - 600) * 40)
    if t <= 1200:
        return 12000
    return max(1200, 12000 - (t - 1200) * 45)


def frames():
    """(time in microseconds relative to BASE, can id, data bytes)"""
    out = []
    for t in range(0, 1501):
        if 900 <= t <= 911:
            continue  # the dropout
        v, i = volts_cv(t), amps_ca(t)
        out.append((t * 1000000, 0x180, bytes([v & 255, v >> 8, i & 255, i >> 8, 0, 0, 0, 0])))
        if t < 60:
            st = 1
        elif t < 900 or (912 <= t < 1440):
            st = 2
        else:
            st = 0
        if t == 1440:
            st = 0
        out.append((t * 1000000 + 50000, 0x181, bytes([st])))
    return out


def write_fixture():
    lines = []
    for rel_us, can_id, data in frames():
        us = BASE * 1000000 + rel_us
        lines.append("(%d.%06d) can0 %03X#%s" % (us // 1000000, us % 1000000, can_id, data.hex().upper()))
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s (%d frames)" % (OUT, len(lines)))


def model_session(open_us, close_us, sample_times_us, load):
    """Trapezoid between consecutive accepted samples, seeded at the open instant.

    load(t_us) -> (millivolts, milliamps) of the last measurement at or before t_us.
    """
    DIV = 2 * 3600 * 1000000  # (P0 + P1) / 2 [uW] * dt [us] / 3600e6 [us/h] -> uWh
    rem = 0
    energy = 0
    prev_us = open_us
    mv, ma = load(open_us)
    p_prev = mv * ma  # uW
    for t in sample_times_us:
        mv, ma = load(t)
        p = mv * ma
        dt = t - prev_us
        num = (p_prev + p) * dt + rem
        energy += num // DIV
        rem = num % DIV
        prev_us, p_prev = t, p
    return energy, (close_us - open_us) // 1000


def expect():
    def load(t_us):
        t = t_us // 1000000
        return volts_cv(t) * 10, amps_ca(t) * 10  # centi-units -> milli-units

    s1_samples = [t * 1000000 for t in range(61, 900)]
    # Silence after the last frame (899.05): the live loop ticks every 200 ms and the
    # session is closed by the first tick that finds the measurement older than 3000 ms.
    last_meas = 899 * 1000000
    last_frame = 899 * 1000000 + 50000
    tick = last_frame
    while True:
        tick += 200000
        if tick - last_meas > 3000000:
            break
    s1_energy, s1_dur = model_session(60050000, tick, s1_samples, load)
    s2_samples = [t * 1000000 for t in range(913, 1441)]
    s2_energy, s2_dur = model_session(912050000, 1440050000, s2_samples, load)

    def price(energy_uwh, dur_ms):
        wh = energy_uwh // 1000000
        s = dur_ms // 1000
        e = (wh * 250000 + 500) // 1000
        t = (s * 3600000 + 1800) // 3600
        return wh, s, e, t, 500000, e + t + 500000

    for name, en, du, stop in (("session 1", s1_energy, s1_dur, "COMMUNICATION_TIMEOUT"),
                               ("session 2", s2_energy, s2_dur, "STATUS_OFF")):
        wh, s, e, t, flat, total = price(en, du)
        print("%s: stop=%s energy_uwh=%d energy_wh=%d duration_ms=%d charging_s=%d "
              "energy_micro_eur=%d time_micro_eur=%d flat_micro_eur=%d total_micro_eur=%d"
              % (name, stop, en, wh, du, s, e, t, flat, total))
    print("session 1 closes at recorded t = %.2f s after the base" % ((tick - 0) / 1e6))


if __name__ == "__main__":
    if "--expect" in sys.argv:
        expect()
    else:
        write_fixture()
