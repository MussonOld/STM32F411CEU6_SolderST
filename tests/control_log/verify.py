#!/usr/bin/env python3
"""Независимая проверка дампа sim.c раскодировщиком tools/control_log_decode.py."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'tools'))
import control_log_decode as d  # noqa: E402

hdr, recs = d.parse(open(sys.argv[1], 'rb').read())
rs = d.rows(hdr, recs)
fail = []

def check(ok, msg):
    if not ok:
        fail.append(msg)

check(len(rs) == 1200 and hdr['state'] == 2, 'буфер не заполнен: %d записей, state=%d' % (len(rs), hdr['state']))
check((hdr['kp'], hdr['ki'], hdr['kd']) == (500, 90, 300), 'коэффициенты в заголовке')
check(rs[0]['first'] == 1 and sum(r['first'] for r in rs) == 1, 'флаг FIRST должен быть ровно в первой записи')
check(rs[0]['sp_c'] == 200.0, 'в логе должен остаться разгон на 200 °C (перезапуск по скачку уставки), sp0=%s' % rs[0]['sp_c'])
check(rs[0]['temp_c'] > 40, 'разгон начинается с ~50 °C, temp0=%s' % rs[0]['temp_c'])
dts = [b['t_s'] - a['t_s'] for a, b in zip(rs, rs[1:])]
check(all(0.045 <= x <= 0.06 for x in dts), 'шаг PID вне 45..60 мс: %.3f..%.3f' % (min(dts), max(dts)))
check(abs(rs[-1]['t_s'] - 59.95) < 0.3, 'длительность %.2f с, ожидалось ~60' % rs[-1]['t_s'])
check(abs(rs[-1]['temp_c'] - 200) < 3, 'температура в конце %.1f, ожидалось ~200' % rs[-1]['temp_c'])
# clamp(p+i+d+ff) должен совпадать с записанной мощностью в пределах округления
bad = 0
for r in rs:
    s = r['p_pct'] + r['i_pct'] + r['d_pct'] + r['ff_pct']
    if abs(min(max(s, 0.0), 100.0) - r['power_pct']) > 0.75:   # 4 слагаемых по 0.1 % + округление выхода до 1 %
        bad += 1
check(bad == 0, 'мощность не сходится со слагаемыми в %d записях' % bad)
check(any(r['stage1'] for r in rs) and any(r['sat_hi'] for r in rs), 'нет стадии 1 / насыщения на разгоне')
check(any(r['in_band'] for r in rs) and rs[-1]['near'] == 1, 'нет захода в полосу/near у уставки')
check(all(r['sp_work_c'] <= r['sp_c'] for r in rs), 'рабочая цель выше уставки')

d.summary(hdr, rs)
if len(sys.argv) > 2:
    d.plot(rs, sys.argv[2]); print('plot ->', sys.argv[2])
if fail:
    print('\nVERIFY FAILED:'); [print(' -', m) for m in fail]; sys.exit(1)
print('\nverify: OK')
