# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the packaged JSON protocol without calling Bilibili or OBS."""
from base64 import b64decode, b64encode
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
from urllib.parse import parse_qs, unquote, urlparse

helper = Path(sys.argv[1])
identity = Path(sys.argv[2])


def challenge(voucher):
    timestamp = b"1760012345"
    key = hashlib.sha256(voucher[9:12].encode() + timestamp).digest()[:16]
    plain = json.dumps({"token": "synthetic-face-token", "type": "realname"}).encode()
    cipher = bytes(byte ^ key[index % len(key)] for index, byte in enumerate(plain))
    return b64encode(b"|".join([timestamp, b"fixture-salt", b"18", b64encode(cipher)])).decode()


with TemporaryDirectory() as folder:
    fixture = Path(folder) / "scene.json"
    fixture.write_text(json.dumps({"sources": [{"id": "scene", "name": "Main",
                                                "settings": {"items": []}}],
                                   "current_scene": "Main"}), encoding="utf-8")
    voucher = "prefix00abcdefghijk"
    requests = [
        ("ping", {}),
        ("account", {"identity": str(identity), "room": "900", "device": "fixture",
                     "cookies": {"DedeUserID": "123", "SESSDATA": "sample", "bili_jct": "sample"}}),
        ("output", {"active": False, "reconnecting": False}),
        ("pk_start", {"mode": "traditional"}),
        ("encoder", {"encoder": "libx264", "scale_mode": "bilinear", "spatial_aq": False}),
        ("face_qr", {"voucher": voucher, "content": challenge(voucher)}),
        ("convert", {"path": str(fixture), "name": "Migrated"}),
    ]
    payload = "".join(json.dumps({"id": ix, "op": op, "args": args}) + "\n"
                      for ix, (op, args) in enumerate(requests))
    process = subprocess.run([sys.executable, "-B", str(helper)], input=payload, text=True,
                             capture_output=True, check=True, timeout=45)
    replies = {record["id"]: record for raw in process.stdout.splitlines()
               if "id" in (record := json.loads(raw))}
    assert replies[0]["value"]["version"] == 1, replies[0]
    assert replies[1]["value"] is True, replies[1]
    assert replies[2]["value"] is True, replies[2]
    assert "OBS" in replies[3]["error"], replies[3]
    assert replies[4]["value"] is True, replies[4]
    result = replies[5]["value"]
    parsed = urlparse(result["url"])
    assert parsed.netloc == "www.bilibili.com"
    assert parse_qs(parsed.query).get("t")
    assert len(b64decode(unquote(parse_qs(parsed.query)["t"][0]))) > 9
    assert json.loads(result["track"])["dm_img_list"] == []
    collection = replies[6]["value"]["collection"]
    assert collection["name"] == "Migrated"
    assert collection["current_scene"] == "Main"
    assert any(source.get("name") == "Main" for source in collection["sources"])
    print("Headless PK admission, face challenge and scene conversion fixtures passed.")
