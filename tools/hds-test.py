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
    a = ap.parse_args()
    if a.cmd == "record":
        sys.exit(0 if asyncio.run(cmd_record(a.file, a.out, a.seconds)) else 1)
    if a.cmd == "pair":
        asyncio.run(cmd_pair(a.file, a.device, a.code))
    else:
        sys.exit(0 if asyncio.run(cmd_run(a.file)) else 1)


if __name__ == "__main__":
    main()
