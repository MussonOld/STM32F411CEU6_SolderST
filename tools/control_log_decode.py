#!/usr/bin/env python3
"""Разбор лога нагрева из RAM (см. Control.md, "Лог нагрева").

  python tools/control_log_decode.py dump.bin            -> dump.csv + dump.png
Вход — .bin из STM32CubeProgrammer (адрес 0x2001A000, размер >= 24 КБ).
Формат — control.c, control_log_hdr_t / control_log_rec_t.
"""
import struct, sys, csv, pathlib

HDR = struct.Struct("<IHHIII12x")   # magic, version, rec_size, capacity, count, start_tick, reserved[3]
REC = struct.Struct("<HhhhhhhhhBB")  # t_ms, temp, work_sp, true_sp, p, i, d, ff, dtdt, duty, flags
assert HDR.size == 32 and REC.size == 20

def decode(raw):
    magic, ver, rec_size, cap, count, start = HDR.unpack_from(raw, 0)
    if magic != 0x474F4C53 or ver != 1 or rec_size != REC.size:
        raise SystemExit(f"не лог: magic={magic:#x} ver={ver} rec_size={rec_size}")
    count = min(count, cap, (len(raw) - HDR.size) // REC.size)
    rows = []
    for k in range(count):
        t, temp, wsp, tsp, p, i, d, ff, dt, duty, fl = REC.unpack_from(raw, HDR.size + k * REC.size)
        rows.append(dict(t_s=t / 1000, temp=temp / 16, work_sp=wsp / 16, setpoint=tsp / 16,
                         p=p / 10, i=i / 10, d=d / 10, ff=ff / 10, dTdt=dt / 10,
                         duty=duty, approach=fl & 1))
    return rows

def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = pathlib.Path(sys.argv[1])
    rows = decode(path.read_bytes())
    if not rows:
        raise SystemExit("лог пуст (count=0)")
    out = path.with_suffix(".csv")
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader(); w.writerows(rows)
    sp = rows[0]["setpoint"]; peak = max(r["temp"] for r in rows)
    print(f"{len(rows)} записей, {rows[-1]['t_s']:.1f} с, уставка {sp:.1f}, пик {peak:.1f} (перелёт {peak - sp:+.1f}) -> {out}")
    try:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    t = [r["t_s"] for r in rows]
    fig, ax = plt.subplots(3, 1, figsize=(10, 8), sharex=True)
    ax[0].plot(t, [r["temp"] for r in rows], label="temp"); ax[0].plot(t, [r["work_sp"] for r in rows], "--", label="work sp")
    ax[0].axhline(sp, color="gray", lw=.5); ax[0].legend(); ax[0].set_ylabel("°C")
    for k in ("p", "i", "d", "ff"):
        ax[1].plot(t, [r[k] for r in rows], label=k)
    ax[1].legend(); ax[1].set_ylabel("%")
    ax[2].plot(t, [r["duty"] for r in rows], label="duty %"); ax[2].plot(t, [r["dTdt"] for r in rows], label="dT/dt °C/s")
    ax[2].legend(); ax[2].set_xlabel("с")
    fig.tight_layout(); fig.savefig(path.with_suffix(".png"), dpi=110)
if __name__ == "__main__":
    main()
