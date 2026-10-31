#!/usr/bin/env python3
"""
Наш кодек против эталонной библиотеки Meshtastic (pip install meshtastic).

Туда: кадры, которые собирает наш C-код, разбираются официальным
mesh_pb2.ToRadio — поле в поле. Обратно: FromRadio, собранные официальной
библиотекой, разбирает наш декодер. Номер поля, перепутанный хоть в одном
месте, здесь всплывает сразу, а на живом узле — молчанием.

SPDX-License-Identifier: AGPL-3.0-or-later
"""
import subprocess
import sys

from meshtastic.protobuf import mesh_pb2, portnums_pb2

BIN = sys.argv[1] if len(sys.argv) > 1 else './build/interop'
fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print('  ✗', what)


def frame_body(hexline):
    raw = bytes.fromhex(hexline)
    check(raw[:2] == b'\x94\xc3', 'заголовок кадра 94 C3')
    n = raw[2] << 8 | raw[3]
    check(n == len(raw) - 4, f'длина в заголовке {n} = {len(raw) - 4}')
    return raw[4:]


# --- туда ---
lines = subprocess.run([BIN, 'enc'], capture_output=True, text=True, check=True).stdout.split()
tr = mesh_pb2.ToRadio()
tr.ParseFromString(frame_body(lines[0]))
check(tr.WhichOneof('payload_variant') == 'want_config_id' and tr.want_config_id == 0x1234567, 'want_config_id')

tr = mesh_pb2.ToRadio()
tr.ParseFromString(frame_body(lines[1]))
check(tr.WhichOneof('payload_variant') == 'heartbeat' and tr.heartbeat.nonce == 777, 'heartbeat')

tr = mesh_pb2.ToRadio()
tr.ParseFromString(frame_body(lines[2]))
p = tr.packet
check(tr.WhichOneof('payload_variant') == 'packet', 'packet')
check(p.to == 0x12345678 and p.channel == 1 and p.id == 0xABCDEF01, f'to/channel/id {p.to:x} {p.channel} {p.id:x}')
check(p.hop_limit == 3 and p.want_ack, 'hop_limit/want_ack')
check(p.decoded.portnum == portnums_pb2.PortNum.PRIVATE_APP, 'portnum PRIVATE_APP')
check(p.decoded.payload.decode() == 'привет, mesh', 'payload')

tr = mesh_pb2.ToRadio()
tr.ParseFromString(frame_body(lines[3]))
p = tr.packet
check(p.to == 0xFFFFFFFF and not p.want_ack and p.decoded.portnum == portnums_pb2.PortNum.TEXT_MESSAGE_APP, 'broadcast text')
check(p.decoded.payload == bytes(range(233)), 'полный кадр 233 байта')


# --- обратно ---
def ours(msg):
    out = subprocess.run([BIN, 'dec', msg.SerializeToString().hex()], capture_output=True, text=True).stdout
    kv = {}
    for line in out.splitlines():
        for tok in line.split():
            if '=' in tok:
                k, v = tok.split('=', 1)
                kv.setdefault(k, v)
    return out, kv


fr = mesh_pb2.FromRadio(id=5)
pk = fr.packet
setattr(pk, 'from', 0xA1B2C3D4)
pk.to = 0xFFFFFFFF
pk.channel = 2
pk.id = 0x0BADF00D
pk.rx_time = 1761900000
pk.rx_snr = 6.25
pk.rx_rssi = -97
pk.hop_limit = 2
pk.hop_start = 3
pk.want_ack = True
pk.decoded.portnum = portnums_pb2.PortNum.PRIVATE_APP
pk.decoded.payload = b'\x53\x11hello'
pk.decoded.request_id = 99
out, kv = ours(fr)
check(kv.get('ok') == '1' and kv.get('kind') == '1', 'packet: ok/kind ' + out[:40])
check(kv.get('from') == str(0xA1B2C3D4) and kv.get('to') == str(0xFFFFFFFF), 'packet: from/to')
check(kv.get('channel') == '2' and kv.get('pid') == str(0x0BADF00D), 'packet: channel/id')
check(kv.get('rx_snr') == '6.25' and kv.get('rx_rssi') == '-97', 'packet: snr/rssi (отрицательный int32)')
check(kv.get('hop_limit') == '2' and kv.get('hop_start') == '3' and kv.get('want_ack') == '1', 'packet: hops/ack')
check(kv.get('rx_time') == '1761900000', 'packet: rx_time')
check(kv.get('portnum') == '256' and kv.get('payload') == '5311' + b'hello'.hex(), 'packet: portnum/payload')
check(kv.get('request_id') == '99', 'packet: request_id')

enc = mesh_pb2.FromRadio(id=6)
setattr(enc.packet, 'from', 1)
enc.packet.to = 2
enc.packet.encrypted = b'\x01\x02\x03'
out, kv = ours(enc)
check(kv.get('encrypted') == '1' and kv.get('decoded') == '0', 'чужой канал: encrypted без decoded')

mi = mesh_pb2.FromRadio(id=7)
mi.my_info.my_node_num = 0xDEADBEEF
mi.my_info.reboot_count = 4
out, kv = ours(mi)
check(kv.get('kind') == '2' and kv.get('my_node_num') == str(0xDEADBEEF), 'my_info')

ni = mesh_pb2.FromRadio(id=8)
ni.node_info.num = 0x10203040
ni.node_info.user.id = '!10203040'
ni.node_info.user.long_name = 'Узел на крыше'
ni.node_info.user.short_name = 'krsh'
ni.node_info.snr = -3.5
ni.node_info.last_heard = 1761900001
ni.node_info.hops_away = 2
out, kv = ours(ni)
check(kv.get('kind') == '3' and kv.get('num') == str(0x10203040), 'node_info: num')
check('long=Узел' in out and kv.get('short') == 'krsh' and kv.get('nid') == '!10203040', 'node_info: имена')
check(kv.get('snr') == '-3.50' and kv.get('last_heard') == '1761900001' and kv.get('hops') == '2', 'node_info: snr/hops')

cc = mesh_pb2.FromRadio(id=9, config_complete_id=0x1234567)
out, kv = ours(cc)
check(kv.get('kind') == '4' and kv.get('config_complete_id') == str(0x1234567), 'config_complete_id')

qs = mesh_pb2.FromRadio(id=10)
qs.queueStatus.res = -1
qs.queueStatus.free = 11
qs.queueStatus.maxlen = 16
qs.queueStatus.mesh_packet_id = 77
out, kv = ours(qs)
check(kv.get('kind') == '6' and kv.get('free') == '11' and kv.get('maxlen') == '16', 'queueStatus')

# FromRadio с полем, которого мы не знаем (config) — не ломает разбор
cfg = mesh_pb2.FromRadio(id=11)
cfg.config.lora.hop_limit = 3
out, kv = ours(cfg)
check(kv.get('ok') == '1' and kv.get('kind') == '0', 'незнакомое поле пропускается')

print(('ок' if not fails else 'ОШИБКИ') + f': сверка с meshtastic, провалено {fails}')
sys.exit(1 if fails else 0)
