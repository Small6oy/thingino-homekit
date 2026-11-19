#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""HomeKit Data Stream (HDS) test controller for rhkd.

Pairs (once) with an rhkd *test instance* using aiohomekit, sets up a data stream through the
Data Stream Transport Management service, connects to the TCP port, and runs:
  1. control/hello              -> expects a success response
  2. dataSend/open (no recording)-> expects a response (MissingProtocol until recording exists)
It's an independent client-side implementation (OPACK, HKDF keys, ChaCha20-Poly1305 frames), so a pass
means rhkd's side interoperates.

usage (~/.venvs/aqara/bin/python):
  hds-test.py pair <pairing.json> <device-id> <setup-code>
  hds-test.py run  <pairing.json>
  hds-test.py record <pairing.json> <out.mp4> [seconds]   # select config, Active=1, record, save fMP4
  hds-test.py talk <pairing.json> <opus.ogg> [--volume N] [--mute-after S]   # live view + two-way audio
The test instance runs on the camera with RHKD_SERVICE_NAME=haptest and its own store, so it never
touches the real accessory (see homekit.md "HDS test instance").
"""
import argparse
import asyncio
import base64
import contextlib
import json
import os
import pathlib
import struct
import sys

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

import aiohomekit.controller.ip.connection as ipconn
from aiohomekit import Controller
from aiohomekit.characteristic_cache import CharacteristicCacheFile
from aiohomekit.zeroconf import ZeroconfServiceListener
from zeroconf.asyncio import AsyncServiceBrowser, AsyncZeroconf

ALIAS = "rhkd-test"
DS_SERVICE = "00000129-0000-1000-8000-0026BB765291"
DS_SETUP = "00000131-0000-1000-8000-0026BB765291"

# --- capture the pair-verify HKDF closure (HDS keys use the same shared secret) ----------------------------------
_derive = {}
_orig = ipconn.get_session_keys


def _capture(pairing_data):
    gen = _orig(pairing_data)
    result = None
    try:
        req = gen.send(None)
        while True:
            resp = yield req
            req = gen.send(resp)
    except StopIteration as stop:
        result = stop.value
    _derive["fn"] = result[1]
    return result


ipconn.get_session_keys = _capture


# --- OPACK -------------------------------------------------------------------------------------------------------
def op_enc(v, out):
    if v is None:
        out.append(0x04)
    elif v is True:
        out.append(0x01)
    elif v is False:
        out.append(0x02)
    elif isinstance(v, int):
        if v == -1:
            out.append(0x07)
        elif 0 <= v <= 39:
            out.append(0x08 + v)
        else:
            out.append(0x33)
            out += struct.pack("<q", v)
    elif isinstance(v, str):
        b = v.encode()
        if len(b) <= 32:
            out.append(0x40 + len(b))
        else:
            out += bytes([0x61, len(b)])
        out += b
    elif isinstance(v, (bytes, bytearray)):
        if len(v) <= 32:
            out.append(0x70 + len(v))
        else:
            out += bytes([0x93]) + struct.pack("<I", len(v))
        out += v
    elif isinstance(v, dict):
        out.append(0xE0 + len(v))
        for k, x in v.items():
            op_enc(k, out)
            op_enc(x, out)
    elif isinstance(v, list):
        out.append(0xD0 + len(v))
        for x in v:
            op_enc(x, out)
    else:
        raise TypeError(v)
    return out


class OpDec:
    def __init__(self, b):
        self.b, self.p, self.tracked = b, 0, []

    def take(self, n):
        v = self.b[self.p:self.p + n]
        if len(v) != n:
            raise ValueError("short")
        self.p += n
        return v

    def t(self, v):
        self.tracked.append(v)
        return v

    def dec(self):
        tag = self.take(1)[0]
        if tag == 0x01:
            return True
        if tag == 0x02:
            return False
        if tag == 0x03:
            return StopIteration
        if tag == 0x04:
            return None
        if tag == 0x07:
            return -1
        if 0x08 <= tag <= 0x2E:
            return tag - 0x08
        if 0x30 <= tag <= 0x33:
            n = 1 << (tag - 0x30)
            return self.t(int.from_bytes(self.take(n), "little", signed=True))
        if tag == 0x36:
            return self.t(struct.unpack("<d", self.take(8))[0])
        if 0x40 <= tag <= 0x60:
            return self.t(self.take(tag - 0x40).decode()) if tag > 0x40 else ""
        if 0x61 <= tag <= 0x64:
            n = int.from_bytes(self.take(1 << (tag - 0x61)), "little")
            return self.t(self.take(n).decode())
        if 0x70 <= tag <= 0x90:
            return self.t(bytes(self.take(tag - 0x70))) if tag > 0x70 else b""
        if 0x91 <= tag <= 0x94:
            n = int.from_bytes(self.take(1 << (tag - 0x91)), "little")
            return self.t(bytes(self.take(n)))
        if 0xA0 <= tag <= 0xC0:
            return self.tracked[tag - 0xA0]
        if 0xD0 <= tag <= 0xDF:
            out = []
            while tag == 0xDF or len(out) < tag - 0xD0:
                x = self.dec()
                if x is StopIteration:
                    break
                out.append(x)
            return out
        if 0xE0 <= tag <= 0xEF:
            out = {}
            while tag == 0xEF or len(out) < tag - 0xE0:
                k = self.dec()
                if k is StopIteration:
                    break
                out[k] = self.dec()
            return out
        raise ValueError(f"tag {tag:#x}")


# --- HDS connection ------------------------------------------------------------------------------------------------
class HDS:
    def __init__(self, reader, writer, c2a, a2c):
        self.r, self.w = reader, writer
        self.c2a, self.a2c = ChaCha20Poly1305(c2a), ChaCha20Poly1305(a2c)
        self.tx = self.rx = 0

    @staticmethod
    def nonce(n):
        return b"\0\0\0\0" + struct.pack("<Q", n)

    async def send(self, header, message):
        h = header if isinstance(header, (bytes, bytearray)) else bytes(op_enc(header, bytearray()))
        payload = bytes([len(h)]) + h + bytes(op_enc(message, bytearray()))
        fh = bytes([1]) + len(payload).to_bytes(3, "big")
        ct = self.c2a.encrypt(self.nonce(self.tx), payload, fh)
        self.tx += 1
        self.w.write(fh + ct)
        await self.w.drain()

    async def recv(self, timeout=5):
        fh = await asyncio.wait_for(self.r.readexactly(4), timeout)
        n = int.from_bytes(fh[1:4], "big")
        body = await asyncio.wait_for(self.r.readexactly(n + 16), timeout)
        payload = self.a2c.decrypt(self.nonce(self.rx), body, fh)
        self.rx += 1
        hl = payload[0]
        hdr = OpDec(payload[1:1 + hl]).dec()
        msg = OpDec(payload[1 + hl:]).dec() if len(payload) > 1 + hl else None
        return hdr, msg


# --- controller plumbing -------------------------------------------------------------------------------------------
@contextlib.asynccontextmanager
async def controller_for(path):
    zc = AsyncZeroconf()
    ctl = Controller(async_zeroconf_instance=zc,
                     char_cache=CharacteristicCacheFile(pathlib.Path(path).parent / "charmap-hds.json"))
    async with zc:
        browser = AsyncServiceBrowser(zc.zeroconf, ["_hap._tcp.local.", "_hap._udp.local."], listener=ZeroconfServiceListener())
        async with ctl:
            if pathlib.Path(path).exists():
                ctl.load_data(path)
            yield ctl
        await browser.async_cancel()


async def cmd_pair(path, device_id, code):
    async with controller_for(path) as ctl:
        finish = await (await ctl.async_find(device_id)).async_start_pairing(ALIAS)
        pairing = await finish(code)
        pathlib.Path(path).write_text(json.dumps({ALIAS: pairing.pairing_data}, indent=2))
        print("paired; keys in", path)


async def cmd_run(path):
    ok = True
    async with controller_for(path) as ctl:
        pairing = ctl.aliases[ALIAS]
        accs = await pairing.list_accessories_and_characteristics()
        setup_iid = None
        for s in accs[0]["services"]:
            if s["type"].upper() in (DS_SERVICE, "129"):
                for c in s["characteristics"]:
                    if c["type"].upper() in (DS_SETUP, "131"):
                        setup_iid = c["iid"]
        if not setup_iid:
            print("FAIL: no Data Stream Transport Management service")
            return False
        print(f"SetupDataStreamTransport iid {setup_iid}")

        csalt = os.urandom(32)
        tlv = bytes([1, 1, 0, 2, 1, 0, 3, 32]) + csalt
        resp = await pairing.connection.put_json("/characteristics", {"characteristics": [
            {"aid": 1, "iid": setup_iid, "value": base64.b64encode(tlv).decode(), "r": True}]})
        val = base64.b64decode(resp["characteristics"][0]["value"])
        items, i = {}, 0
        while i < len(val):
            items[val[i]] = val[i + 2:i + 2 + val[i + 1]]
            i += 2 + val[i + 1]
        status = items.get(1, b"\xff")[0]
        port = int.from_bytes(items[2][2:4], "little") if 2 in items else 0
        asalt = items.get(3, b"")
        print(f"setup response: status {status}, port {port}, accessory salt {len(asalt)} bytes")
        if status != 0 or len(asalt) != 32:
            print("FAIL: bad setup response")
            return False
        reread = await pairing.get_characteristics([(1, setup_iid)])
        rv = base64.b64decode(reread[(1, setup_iid)]["value"])
        print("re-read omits the salt:", b"\x03\x20" not in rv)

        derive = _derive["fn"]
        salt = csalt + asalt
        a2c = derive(salt, b"HDS-Read-Encryption-Key")
        c2a = derive(salt, b"HDS-Write-Encryption-Key")
        host = pairing.pairing_data["AccessoryIP"]
        reader, writer = await asyncio.open_connection(host, port)
        hds = HDS(reader, writer, c2a, a2c)

        await hds.send({"protocol": "control", "request": "hello", "id": 1}, {})
        hdr, msg = await hds.recv()
        print("hello ->", hdr, msg)
        ok &= hdr.get("response") == "hello" and hdr.get("status") == 0 and hdr.get("id") == 1

        await hds.send({"protocol": "dataSend", "request": "open", "id": 2},
                       {"target": "controller", "type": "ipcamera.recording", "streamId": 1})
        hdr, msg = await hds.recv()
        print("dataSend/open ->", hdr, msg)
        ok &= hdr.get("response") == "open" and hdr.get("id") == 2 and "status" in hdr

        # Hand-written header with a back-reference: {"protocol":"dataSend","request":<ref 1>,"id":3}.
        # Tracked strings: 0 "protocol", 1 "dataSend", 2 "request", so 0xA1 means "dataSend".
        raw = bytearray([0xE3])
        op_enc("protocol", raw); op_enc("dataSend", raw); op_enc("request", raw); raw.append(0xA1)
        op_enc("id", raw); op_enc(3, raw)
        await hds.send(raw, {})
        hdr, msg = await hds.recv()
        print("back-reference request ->", hdr, msg)
        ok &= hdr.get("response") == "dataSend" and hdr.get("id") == 3

        writer.close()
        print("PASS" if ok else "FAIL")
    return ok


def tlv(*items):
    out = bytearray()
    for t, v in items:
        if isinstance(v, int):
            v = bytes([v])
        out += bytes([t, len(v)]) + v
    return bytes(out)


# The configuration Home selected on the v4 (2026-11-18 14:02): 1080p30 Main L4 2000 kbit/s, I-frame 4 s,
# 4 s prebuffer/fragments, AAC-LC mono 16 kHz 16 kbit/s.
SELECTED = tlv(
    (1, tlv((1, struct.pack("<I", 4000)), (2, struct.pack("<Q", 1)),
            (3, tlv((1, 0), (2, tlv((1, struct.pack("<I", 4000)))))))),
    (2, tlv((1, 0), (2, tlv((1, 1), (2, 2), (3, struct.pack("<I", 2000)), (4, struct.pack("<I", 4000)))),
            (3, tlv((1, struct.pack("<H", 1920)), (2, struct.pack("<H", 1080)), (3, 30))))),
    (3, tlv((1, 0), (2, tlv((1, 1), (2, 0), (3, 1), (4, struct.pack("<I", 16)))))),
)


async def timed_put(conn, chars):
    pid = int.from_bytes(os.urandom(8), "little") >> 1
    r = await conn.put_json("/prepare", {"ttl": 5000, "pid": pid})
    if r and r.get("status", 0) != 0:
        raise RuntimeError(f"prepare failed: {r}")
    return await conn.put_json("/characteristics", {"characteristics": chars, "pid": pid})


async def cmd_record(path, out, seconds):
    async with controller_for(path) as ctl:
        pairing = ctl.aliases[ALIAS]
        accs = await pairing.list_accessories_and_characteristics()
        iid = {}
        for s in accs[0]["services"]:
            for c in s["characteristics"]:
                iid[(s["type"].upper()[:8], c["type"].upper()[:8])] = c["iid"]
        rec = lambda t: iid[("00000204", t)]
        setup_iid = iid[("00000129", "00000131")]
        print("selected:", await pairing.connection.put_json("/characteristics", {"characteristics": [
            {"aid": 1, "iid": rec("00000209"), "value": base64.b64encode(SELECTED).decode()}]}) or "ok")
        print("audio active:", await timed_put(pairing.connection, [{"aid": 1, "iid": rec("00000226"), "value": 1}]) or "ok")
        print("active:", await timed_put(pairing.connection, [{"aid": 1, "iid": rec("000000B0"), "value": 1}]) or "ok")
        await asyncio.sleep(6)  # let the prebuffer fill

        csalt = os.urandom(32)
        resp = await pairing.connection.put_json("/characteristics", {"characteristics": [
            {"aid": 1, "iid": setup_iid, "value": base64.b64encode(bytes([1, 1, 0, 2, 1, 0, 3, 32]) + csalt).decode(),
             "r": True}]})
        val = base64.b64decode(resp["characteristics"][0]["value"])
        items, i = {}, 0
        while i < len(val):
            items[val[i]] = val[i + 2:i + 2 + val[i + 1]]
            i += 2 + val[i + 1]
        port = int.from_bytes(items[2][2:4], "little")
        derive = _derive["fn"]
        salt = csalt + items[3]
        reader, writer = await asyncio.open_connection(pairing.pairing_data["AccessoryIP"], port)
        hds = HDS(reader, writer, derive(salt, b"HDS-Write-Encryption-Key"), derive(salt, b"HDS-Read-Encryption-Key"))
        await hds.send({"protocol": "control", "request": "hello", "id": 1}, {})
        print("hello:", (await hds.recv())[0])
        await hds.send({"protocol": "dataSend", "request": "open", "id": 2},
                       {"target": "controller", "type": "ipcamera.recording", "streamId": 7, "reason": "motion"})
        hdr, msg = await hds.recv()
        print("open:", hdr, msg)
        if hdr.get("status") != 0:
            return False

        data = bytearray()
        loop = asyncio.get_running_loop()
        deadline = loop.time() + seconds
        cur = bytearray()
        packets = 0
        while loop.time() < deadline:
            try:
                hdr, msg = await hds.recv(timeout=max(0.1, deadline - loop.time()))
            except asyncio.TimeoutError:
                break
            if hdr.get("event") == "close":
                print("camera closed the stream:", msg)
                break
            if hdr.get("event") != "data":
                print("other:", hdr, msg)
                continue
            for p in msg["packets"]:
                md = p["metadata"]
                cur += p["data"]
                if md.get("dataChunkSequenceNumber") == 1:
                    print(f"  {md['dataType']} #{md['dataSequenceNumber']} total {md.get('dataTotalSize')} bytes")
                if md.get("isLastDataChunk"):
                    if md.get("dataTotalSize") not in (None, len(cur)) and md.get("dataChunkSequenceNumber") == 1:
                        print("  size mismatch!")
                    data += cur
                    cur = bytearray()
                    packets += 1
        await hds.send({"protocol": "dataSend", "event": "close"}, {"streamId": 7, "reason": 0})
        await asyncio.sleep(0.5)
        writer.close()
        pathlib.Path(out).write_bytes(data)
        print(f"saved {len(data)} bytes, {packets} packets to {out}")
        await timed_put(pairing.connection, [{"aid": 1, "iid": rec("000000B0"), "value": 0}])
        return packets >= 2


# --- two-way audio test: live-view session + SRTP Opus to the camera's audio port ------------------------------
def srtp_session_keys(mk, ms):
    """RFC 3711 4.3.1 (kdr 0): session encryption key, auth key and salt for SRTP."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    def derive(label, n):
        iv = bytearray(ms) + b"\0\0"
        iv[7] ^= label
        enc = Cipher(algorithms.AES(mk), modes.CTR(bytes(iv))).encryptor()
        return enc.update(b"\0" * n) + enc.finalize()

    return derive(0, 16), derive(1, 20), derive(2, 14)


def srtp_protect(keys, pkt, roc=0):
    import hmac
    import hashlib
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    ke, ka, ks = keys
    seq = int.from_bytes(pkt[2:4], "big")
    ssrc = int.from_bytes(pkt[8:12], "big")
    index = (roc << 16) | seq
    iv = bytearray(ks) + b"\0\0"
    for i, b in enumerate(ssrc.to_bytes(4, "big")):
        iv[4 + i] ^= b
    for i, b in enumerate(index.to_bytes(6, "big")):
        iv[8 + i] ^= b
    enc = Cipher(algorithms.AES(ke), modes.CTR(bytes(iv))).encryptor()
    out = pkt[:12] + enc.update(pkt[12:]) + enc.finalize()
    tag = hmac.new(ka, out + roc.to_bytes(4, "big"), hashlib.sha1).digest()[:10]
    return out + tag


def ogg_packets(path):
    """Opus packets from an Ogg file (skips OpusHead and OpusTags)."""
    data, pos, cur, out = pathlib.Path(path).read_bytes(), 0, b"", []
    while pos + 27 <= len(data) and data[pos:pos + 4] == b"OggS":
        nseg = data[pos + 26]
        segs = data[pos + 27:pos + 27 + nseg]
        body = pos + 27 + nseg
        for s in segs:
            cur += data[body:body + s]
            body += s
            if s < 255:
                out.append(cur)
                cur = b""
        pos = body
    return [p for p in out if not p.startswith((b"OpusHead", b"OpusTags"))]


async def cmd_talk(path, ogg, volume, mute_after, suspend_at=0, service=0, size="640x360", kbps=299,
                   reconfigure_at=0, reconfigure_kbps=0, echo=None):
    import socket
    import time
    pkts = ogg_packets(ogg)
    async with controller_for(path) as ctl:
        pairing = ctl.aliases[ALIAS]
        conn = pairing.connection
        accs = await pairing.list_accessories_and_characteristics()
        iid, rtp_services = {}, []
        for s in accs[0]["services"]:
            if s["type"].upper()[:8] == "00000110":
                rtp_services.append({c["type"].upper()[:8]: c["iid"] for c in s["characteristics"]})
            for c in s["characteristics"]:
                iid.setdefault((s["type"].upper()[:8], c["type"].upper()[:8]), c["iid"])
        if ("00000113", "00000119") not in iid:
            print("no Speaker service (two-way audio) on this accessory")
            return False
        spk_vol, spk_mute = iid[("00000113", "00000119")], iid[("00000113", "0000011A")]
        print(f"{len(rtp_services)} RTP stream service(s); using #{service}")
        if echo is not None:
            sw = iid.get(("00000049", "00000025"))
            print(f"echo cancellation switch -> {echo}:", await conn.put_json("/characteristics", {"characteristics": [
                {"aid": 1, "iid": sw, "value": echo == "on"}]}) or "ok" if sw else "no switch")
        setup_iid, sel_iid = rtp_services[service]["00000118"], rtp_services[service]["00000117"]
        width, height = (int(v) for v in size.split("x"))
        print("speaker volume ->", volume, await conn.put_json("/characteristics", {"characteristics": [
            {"aid": 1, "iid": spk_vol, "value": volume}, {"aid": 1, "iid": spk_mute, "value": False}]}) or "ok")

        cam_ip = pairing.pairing_data["AccessoryIP"]
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect((cam_ip, 9))
        my_ip = probe.getsockname()[0]
        probe.close()
        vs, aus = (socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(2))
        for s in (vs, aus):
            s.bind((my_ip, 0))
            s.setblocking(False)
        vkey, vsalt, akey, asalt, sid = (os.urandom(n) for n in (16, 14, 16, 14, 16))
        req = tlv((1, sid),
                  (3, tlv((1, 0), (2, my_ip.encode()), (3, struct.pack("<H", vs.getsockname()[1])),
                          (4, struct.pack("<H", aus.getsockname()[1])))),
                  (4, tlv((1, 0), (2, vkey), (3, vsalt))), (5, tlv((1, 0), (2, akey), (3, asalt))))
        await conn.put_json("/characteristics", {"characteristics": [
            {"aid": 1, "iid": setup_iid, "value": base64.b64encode(req).decode()}]})
        got = await conn.get_json(f"/characteristics?id=1.{setup_iid}")
        val = base64.b64decode(got["characteristics"][0]["value"])
        items, i = {}, 0
        while i < len(val):
            items[val[i]] = val[i + 2:i + 2 + val[i + 1]]
            i += 2 + val[i + 1]
        addr, j = {}, 0
        while j < len(items[3]):
            addr[items[3][j]] = items[3][j + 2:j + 2 + items[3][j + 1]]
            j += 2 + items[3][j + 1]
        cam_aport = int.from_bytes(addr[4], "little")
        print(f"setup: status {items[2][0]}, camera audio port {cam_aport}")

        my_assrc = int.from_bytes(os.urandom(4), "big")
        sel = tlv(
            (1, tlv((1, sid), (2, 1))),
            (2, tlv((1, 0), (2, tlv((1, 0), (2, 0), (3, 0))),
                    (3, tlv((1, struct.pack("<H", width)), (2, struct.pack("<H", height)), (3, 30))),
                    (4, tlv((1, 99), (2, struct.pack("<I", 1)), (3, struct.pack("<H", kbps)),
                            (4, struct.pack("<f", 0.5)), (5, struct.pack("<H", 1378)))))),
            (3, tlv((1, 3), (2, tlv((1, 1), (2, 0), (3, 1), (4, 20))),
                    (3, tlv((1, 110), (2, struct.pack("<I", my_assrc)), (3, struct.pack("<H", 24)),
                            (4, struct.pack("<f", 5.0)), (6, 13))), (4, 0))))
        print("start:", await conn.put_json("/characteristics", {"characteristics": [
            {"aid": 1, "iid": sel_iid, "value": base64.b64encode(sel).decode()}]}) or "ok")

        keys = srtp_session_keys(akey, asalt)
        seq, ts, rx_v, rx_a = int.from_bytes(os.urandom(2), "big"), 0, 0, 0
        vbytes, reconfigured, vb_mark = 0, False, (0.0, 0)
        t0, muted = time.monotonic(), False
        phase, phase_v = "before", {"before": 0, "suspended": 0, "resumed": 0}
        for n, op in enumerate(pkts):
            el = time.monotonic() - t0
            if suspend_at and phase == "before" and el >= suspend_at:
                phase = "suspended"
                print("suspend:", await conn.put_json("/characteristics", {"characteristics": [
                    {"aid": 1, "iid": sel_iid, "value": base64.b64encode(tlv((1, tlv((1, sid), (2, 2))))).decode()}]}) or "ok")
                await asyncio.sleep(0.3)  # packets already in flight
                for s in (vs, aus):
                    with contextlib.suppress(BlockingIOError):
                        while True:
                            s.recv(2048)
            elif phase == "suspended" and el >= suspend_at + 2:
                phase = "resumed"
                print("resume:", await conn.put_json("/characteristics", {"characteristics": [
                    {"aid": 1, "iid": sel_iid, "value": base64.b64encode(tlv((1, tlv((1, sid), (2, 3))))).decode()}]}) or "ok")
            if reconfigure_at and not reconfigured and el >= reconfigure_at:
                reconfigured = True
                rb = vbytes - vb_mark[1]
                print(f"video before reconfigure: {rb * 8 / max(el - vb_mark[0], 0.1) / 1000:.0f} kbit/s")
                vb_mark = (el, vbytes)
                rc = tlv((1, tlv((1, sid), (2, 4))),
                         (2, tlv((3, tlv((1, struct.pack("<H", width)), (2, struct.pack("<H", height)), (3, 30))),
                                 (4, tlv((3, struct.pack("<H", reconfigure_kbps)),)))))
                print(f"reconfigure -> {reconfigure_kbps} kbit/s:", await conn.put_json("/characteristics", {"characteristics": [
                    {"aid": 1, "iid": sel_iid, "value": base64.b64encode(rc).decode()}]}) or "ok")
            if mute_after and not muted and time.monotonic() - t0 >= mute_after:
                muted = True
                print("speaker mute ->", await conn.put_json("/characteristics", {"characteristics": [
                    {"aid": 1, "iid": spk_mute, "value": True}]}) or "ok")
            rtp = bytes([0x80, 110]) + seq.to_bytes(2, "big") + ts.to_bytes(4, "big") + my_assrc.to_bytes(4, "big") + op
            aus.sendto(srtp_protect(keys, rtp), (cam_ip, cam_aport))
            seq, ts = (seq + 1) & 0xFFFF, (ts + 320) & 0xFFFFFFFF
            for s in (vs, aus):
                with contextlib.suppress(BlockingIOError):
                    while True:
                        d = s.recv(2048)
                        if s is vs:
                            rx_v += 1
                            vbytes += len(d)
                            phase_v[phase] += 1
                        else:
                            rx_a += 1
            await asyncio.sleep(max(0, t0 + (n + 1) * 0.02 - time.monotonic()))
        print(f"sent {len(pkts)} Opus packets; received {rx_v} video and {rx_a} audio packets from the camera")
        el = time.monotonic() - t0
        print(f"video {'after reconfigure' if reconfigured else 'overall'}: "
              f"{(vbytes - vb_mark[1]) * 8 / max(el - vb_mark[0], 0.1) / 1000:.0f} kbit/s")
        if suspend_at:
            print("video packets per phase:", phase_v)
        end = tlv((1, tlv((1, sid), (2, 0))))
        print("end:", await conn.put_json("/characteristics", {"characteristics": [
            {"aid": 1, "iid": sel_iid, "value": base64.b64encode(end).decode()}]}) or "ok")
        await conn.put_json("/characteristics", {"characteristics": [{"aid": 1, "iid": spk_mute, "value": False},
                                                                     {"aid": 1, "iid": spk_vol, "value": 100}]})
        return rx_v > 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pair")
    p.add_argument("file")
    p.add_argument("device")
    p.add_argument("code")
    r = sub.add_parser("run")
    r.add_argument("file")
    rc = sub.add_parser("record")
    rc.add_argument("file")
    rc.add_argument("out")
    rc.add_argument("seconds", type=float, nargs="?", default=12)
    tk = sub.add_parser("talk")
    tk.add_argument("file")
    tk.add_argument("ogg", help="Opus in Ogg, 20 ms frames (e.g. ffmpeg ... -c:a libopus -frame_duration 20)")
    tk.add_argument("--volume", type=int, default=1, help="Speaker volume during the test (default 1: quiet)")
    tk.add_argument("--mute-after", type=float, default=0, help="seconds after which Speaker Mute is set")
    tk.add_argument("--suspend-at", type=float, default=0, help="Suspend the session at S seconds, Resume 2 s later")
    tk.add_argument("--service", type=int, default=0, help="which Camera RTP Stream Management service (0, 1)")
    tk.add_argument("--size", default="640x360", help="requested video size, e.g. 1280x720")
    tk.add_argument("--kbps", type=int, default=299, help="requested max video bitrate")
    tk.add_argument("--reconfigure-at", type=float, default=0, help="send Reconfigure at S seconds")
    tk.add_argument("--reconfigure-kbps", type=int, default=0, help="max bitrate in that Reconfigure")
    tk.add_argument("--echo", choices=["on", "off"], help="set the Echo Cancellation switch first")
    a = ap.parse_args()
    if a.cmd == "talk":
        sys.exit(0 if asyncio.run(cmd_talk(a.file, a.ogg, a.volume, a.mute_after, a.suspend_at, a.service,
                                                     a.size, a.kbps, a.reconfigure_at, a.reconfigure_kbps, a.echo)) else 1)
    if a.cmd == "record":
        sys.exit(0 if asyncio.run(cmd_record(a.file, a.out, a.seconds)) else 1)
    if a.cmd == "pair":
        asyncio.run(cmd_pair(a.file, a.device, a.code))
    else:
        sys.exit(0 if asyncio.run(cmd_run(a.file)) else 1)


if __name__ == "__main__":
    main()
