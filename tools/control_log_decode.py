#!/usr/bin/env python3
"""Раскодировщик RAM-лога нагрева (формат — App/Control/control_log.h).

    python3 tools/control_log_decode.py log.bin [--csv out.csv] [--plot out.png] [--offset N]

log.bin — дамп из STM32CubeProgrammer: адрес 0x2001A000, размер 0x5E00 (можно больше —
хвост игнорируется; если дамп начинается не с заголовка, укажи --offset).
Печатает сводку, пишет CSV (для подбора модели) и график.
"""
import argparse
import csv
import struct
import sys

HDR = struct.Struct('<IHHHHHBBHHHHIH10H14s')   # 64 байта
REC = struct.Struct('<HhhhhhhhhH')               # 20 байт
MAGIC = 0x474F4C43
STATES = {0: 'ARMED (ждёт старта нагрева)', 1: 'RECORDING (пишется/не дописан)', 2: 'FULL (заполнен)'}
FLAGS = [('stage1', 0), ('in_band', 1), ('near', 2), ('ff_faded', 3), ('sat_lo', 4),
         ('sat_hi', 5), ('i_clamp', 6), ('overshoot', 7), ('presleep', 8), ('first', 9)]
CFG = ['ff_pct_per_100c', 'ambient_c', 'integral_band_c', 'approach_offset_c', 'overshoot_gain',
       'ff_fade_dtdt', 'dtdt_filter_ms', 'steady_band_c', 'poll_ms', 'pwm_period_ms']


def parse(buf, offset=0):
    if len(buf) < offset + HDR.size:
        sys.exit('файл короче заголовка (64 байта)')
    h = HDR.unpack_from(buf, offset)
    hdr = dict(zip(['magic', 'version', 'rec_size', 'capacity', 'count', 'state', 'channel', 'res0',
                    'kp', 'ki', 'kd', 'pid_scale', 't0_ms', 'restarts'], h[:14]))
    hdr.update(zip(CFG, h[14:24]))
    if hdr['magic'] != MAGIC:
        sys.exit('нет сигнатуры CLOG (magic=0x%08X): неверный адрес/--offset, лог не включён '
                 '(CONTROL_LOG_ENABLE) или RAM прочитан после сброса' % hdr['magic'])
    if hdr['version'] != 1 or hdr['rec_size'] != REC.size:
        sys.exit('неизвестная версия формата: version=%d rec_size=%d' % (hdr['version'], hdr['rec_size']))
    n = min(hdr['count'], hdr['capacity'], (len(buf) - offset - HDR.size) // REC.size)
    if n < hdr['count']:
        print('внимание: в файле только %d из %d записей (дамп короче 0x5E00?)' % (n, hdr['count']), file=sys.stderr)
    recs = [REC.unpack_from(buf, offset + HDR.size + k * REC.size) for k in range(n)]
    return hdr, recs


def rows(hdr, recs):
    out, t_cum, prev = [], 0, None
    for t16, temp, sp, spw, pw, p, i, d, ff, fl in recs:
        if prev is not None:
            t_cum += (t16 - prev) & 0xFFFF     # разворачиваем 16-битный тик (64 с > окна 60 с)
        prev = t16
        r = {'t_s': t_cum / 1000.0, 'temp_c': temp / 10, 'sp_c': sp / 10, 'sp_work_c': spw / 10,
             'power_pct': pw / 10, 'p_pct': p / 10, 'i_pct': i / 10, 'd_pct': d / 10, 'ff_pct': ff / 10,
             'flags': fl}
        for name, bit in FLAGS:
            r[name] = (fl >> bit) & 1
        out.append(r)
    return out


def summary(hdr, rs):
    s = hdr['pid_scale']
    print('состояние : %s, записей %d из %d, перезапусков записи: %d'
          % (STATES.get(hdr['state'], hdr['state']), len(rs), hdr['capacity'], hdr['restarts']))
    print('канал %d, Kp/Ki/Kd = %g / %g / %g (raw %d/%d/%d)'
          % (hdr['channel'], hdr['kp'] / s, hdr['ki'] / s, hdr['kd'] / s, hdr['kp'], hdr['ki'], hdr['kd']))
    print('константы : ' + ', '.join('%s=%d' % (k, hdr[k]) for k in CFG))
    if len(rs) < 2:
        return
    dts = [b['t_s'] - a['t_s'] for a, b in zip(rs, rs[1:])]
    print('длительность %.1f с, шаг PID: среднее %.1f мс (мин %.0f, макс %.0f)'
          % (rs[-1]['t_s'], 1000 * sum(dts) / len(dts), 1000 * min(dts), 1000 * max(dts)))
    sp = rs[-1]['sp_c']
    peak = max(rs, key=lambda r: r['temp_c'])
    print('уставка %.1f °C, старт %.1f °C, пик %.1f °C на %.1f с (перелёт %+.1f °C), конец %.1f °C'
          % (sp, rs[0]['temp_c'], peak['temp_c'], peak['t_s'], peak['temp_c'] - sp, rs[-1]['temp_c']))
    for band in (10, 5, 2):
        hit = next((r for r in rs if r['temp_c'] >= sp - band), None)
        if hit:
            print('  достигнуто sp-%d °C за %.1f с' % (band, hit['t_s']))
    tail = [r for r in rs if r['t_s'] >= rs[-1]['t_s'] - 5]
    print('последние 5 с: мощность в среднем %.1f %%, температура %.1f..%.1f °C'
          % (sum(r['power_pct'] for r in tail) / len(tail), min(r['temp_c'] for r in tail),
             max(r['temp_c'] for r in tail)))
    n = len(rs)
    print('доли шагов: стадия1 %.0f %%, выход в 100 %% %.0f %%, в 0 %% %.0f %%, интеграл упёрт %.0f %%, перелёт %.0f %%'
          % tuple(100 * sum(r[k] for r in rs) / n for k in ('stage1', 'sat_hi', 'sat_lo', 'i_clamp', 'overshoot')))


def plot(rs, path):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    t = [r['t_s'] for r in rs]
    fig, (a, b) = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
    a.plot(t, [r['temp_c'] for r in rs], label='температура', color='C3')
    a.plot(t, [r['sp_c'] for r in rs], '--', label='уставка', color='k')
    a.plot(t, [r['sp_work_c'] for r in rs], ':', label='рабочая цель (стадия 1)', color='C0')
    a.set_ylabel('°C'); a.grid(alpha=.3); a.legend(loc='lower right')
    b.plot(t, [r['power_pct'] for r in rs], label='мощность', color='k', lw=1.6)
    for k, c in (('p_pct', 'C0'), ('i_pct', 'C2'), ('d_pct', 'C1'), ('ff_pct', 'C4')):
        b.plot(t, [r[k] for r in rs], label=k[:-4].upper(), color=c, lw=1)
    b.set_ylabel('% мощности'); b.set_xlabel('с'); b.grid(alpha=.3); b.legend(ncol=5, loc='upper right')
    b.set_ylim(-110, 150)
    fig.tight_layout(); fig.savefig(path, dpi=110)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('bin'); ap.add_argument('--csv'); ap.add_argument('--plot')
    ap.add_argument('--offset', type=lambda x: int(x, 0), default=0)
    a = ap.parse_args()
    hdr, recs = parse(open(a.bin, 'rb').read(), a.offset)
    rs = rows(hdr, recs)
    summary(hdr, rs)
    if a.csv and rs:
        with open(a.csv, 'w', newline='') as f:
            w = csv.DictWriter(f, fieldnames=list(rs[0].keys())); w.writeheader(); w.writerows(rs)
        print('CSV  ->', a.csv)
    if a.plot and rs:
        plot(rs, a.plot); print('plot ->', a.plot)


if __name__ == '__main__':
    main()
