#!/usr/bin/env python3
"""Validate ath10k containers and generate the kext's embedded payload header.

Inputs are the pinned linux-firmware files fetched by scripts/fetch-firmware.sh
(no network or device access here). Formats: Linux ath10k hw.h and core.c
(ath10k_core_fetch_board_data_api_n, ath10k_core_fetch_firmware_api_n).

  prepare-firmware.py --set kext   QCA6174 only: what the kext embeds
  prepare-firmware.py --set test   QCA6174 + QCA9377: for the host unit tests

Output: <out>/Ath10kPayload.hpp and <out>/manifest.json.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

BOARD_MAGIC = b"QCA-ATH10K-BOARD\0"
FW_MAGIC = b"QCA-ATH10K\0"
# Acer Predator Helios 300 G3-571: Lite-On QCA6174 module, subsystem 11ad:0807.
# This is the exact name Linux ath10k builds on that laptop and matches in
# QCA6174/hw3.0/board-2.bin (kexts/wifi/evidence/linux-ath10k-load-trace.txt).
ACER_BOARD_KEY = "bus=pci,vendor=168c,device=003e,subsystem-vendor=11ad,subsystem-device=0807"
# Upstream Ath10kMac's tested Dell profile (kept; same firmware, 8 KB of board data).
BOARD_KEY = "bus=pci,vendor=168c,device=003e,subsystem-vendor=1028,subsystem-device=0310"
BOARD_SIZE = 8124
QCA9377_BOARD_KEY = "bus=pci,vendor=168c,device=0042,subsystem-vendor=1028,subsystem-device=1810"

class BoardNotFound(ValueError):
    """Only a well-formed API2 container without a match allows API1 fallback."""



def ies(data, magic=None):
    pos = 0
    if magic is not None:
        if not data.startswith(magic):
            raise ValueError("incorrect container magic")
        pos = (len(magic) + 3) & ~3
        if len(data) < pos:
            raise ValueError("truncated magic padding")
    while pos < len(data):
        if len(data) - pos < 8:
            raise ValueError("truncated IE header")
        tag, size = struct.unpack_from("<II", data, pos)
        pos += 8
        padded = (size + 3) & ~3
        if padded > len(data) - pos:
            raise ValueError("IE length exceeds container")
        yield tag, data[pos:pos + size]
        pos += padded


def select_board(data, key=BOARD_KEY):
    matches = []
    for tag, block in ies(data, BOARD_MAGIC):
        if tag != 0:  # normal board, not extended-board IE
            continue
        names = []
        for subtag, value in ies(block):
            if subtag == 0:
                names.append(value)
            elif subtag == 1:
                if key.encode("ascii") in names:
                    matches.append(value)
                names = []
    if not matches:
        raise BoardNotFound("no exact board match")
    if len(matches) != 1:
        raise ValueError(f"expected one exact board match; found {len(matches)}")
    if len(matches[0]) != BOARD_SIZE:
        raise ValueError(f"unsupported board size: {len(matches[0])}")
    return matches[0]


def parse_firmware(data):
    parts = {}
    for tag, value in ies(data, FW_MAGIC):
        if tag in parts:
            raise ValueError(f"duplicate firmware IE {tag}")
        parts[tag] = value
    for tag in (0, 3, 4, 5, 6):
        if not parts.get(tag):
            raise ValueError(f"missing/empty firmware IE {tag}")
    for tag in (5, 6):
        if len(parts[tag]) != 4:
            raise ValueError(f"bad operation-version size for IE {tag}")
    wmi, htt = (struct.unpack("<I", parts[tag])[0] for tag in (5, 6))
    if (wmi, htt) != (4, 3):
        raise ValueError("expected TLV WMI=4 / HTT=3")
    if 7 in parts:
        raise ValueError("code-swap firmware is not supported in this milestone")
    version = parts[0].rstrip(b"\0").decode("ascii")
    if not version or any(ord(c) < 32 or ord(c) > 126 for c in version):
        raise ValueError("invalid firmware version string")
    return parts, version


def sha(data):
    return hashlib.sha256(data).hexdigest()


def ignore_otp_result(parts):
    # Linux v6.12 core.h: ATH10K_FW_FEATURE_IGNORE_OTP_RESULT = 7.
    features = parts.get(2, b"")
    return bool(features and (features[0] & 0x80))


def supports_skip_clock_init(parts):
    # Linux v6.12 core.h: ATH10K_FW_FEATURE_SUPPORTS_SKIP_CLOCK_INIT = 9.
    features = parts.get(2, b"")
    return len(features) > 1 and bool(features[1] & 0x02)


def parse_segments(image):
    """Uncompressed SGMT image, with data + one BEGINADDR + terminal DONE.

    Linux bmi.h/hw.c define this envelope. EXEC/BDDATA are deliberately
    rejected: this milestone downloads code without executing WLAN firmware.
    Offsets refer to the original image (metadata is not sent as raw RAM).
    """
    if len(image) < 8 or len(image) > 2 * 1024 * 1024:
        raise ValueError("unsupported SGMT image size")
    if struct.unpack_from("<II", image) != (0x544d4753, 0):
        raise ValueError("expected uncompressed SGMT firmware image")
    pos, entry, segments = 8, None, []
    while pos < len(image):
        if len(image) - pos < 8:
            raise ValueError("truncated SGMT metadata")
        address, size = struct.unpack_from("<II", image, pos)
        pos += 8
        if size == 0xffffffff:
            if pos != len(image) or entry is None or not segments:
                raise ValueError("invalid SGMT end / missing entry point")
            if not any(s['address'] <= entry < s['address'] + s['length'] for s in segments):
                raise ValueError("SGMT entry point is outside data segments")
            return segments, entry
        if size == 0xfffffffd:
            if entry is not None or (address & 3):
                raise ValueError("duplicate/unaligned SGMT entry point")
            entry = address
            continue
        if size & 0x80000000:
            raise ValueError("unsupported SGMT EXEC/BDDATA/metadata")
        if entry is not None or size == 0 or size > len(image) - pos:
            raise ValueError("invalid SGMT data length/order")
        # Rounded-up tail reads must also fit the target's 32-bit address space.
        if address & 3 or address + ((size + 3) & ~3) > 0x100000000:
            raise ValueError("invalid SGMT target address range")
        if len(segments) >= 64:
            raise ValueError("too many SGMT segments")
        if any(address < s['address'] + s['length'] and s['address'] < address + size for s in segments):
            raise ValueError("overlapping SGMT segments cannot be verified independently")
        # Protect the boot configuration and board area used by this milestone.
        if address < 0x404000 and address + size > 0x400800:
            raise ValueError("SGMT data overlaps Host Interest / board RAM")
        segments.append({'address': address, 'offset': pos, 'length': size})
        pos += size
    raise ValueError("SGMT DONE marker missing")


def linux_crc32(data):
    """crc32_le(0, data, len) as ath10k prints it (no pre/post inversion)."""
    import zlib
    return (~zlib.crc32(data, 0xffffffff)) & 0xffffffff


# Pinned inputs: linux-firmware a2dcb924 (tag 20261019). The board SHA-256 is the
# selected 8124-byte entry; linux_crc32 is what Linux debugfs fw_checksums
# reports as "board" for the same entry on the owner's laptop.
QCA6174 = {
    "namespace": "Ath10kPayload", "dir": "ath10k/QCA6174/hw3.0", "profile": "QCA6174/hw3.2",
    "device": 0x003e, "chip": 10, "target": 0x05030000,
    "firmware_container_sha256": "04d3bad5efa3f9fbe3ba53fd3e25fa9b0585ed227eea8111303b4e08861f979d",
    "board_container_sha256": "66e83dde1c9af535df1fcd17c72971a96a263357300e921b358d35a353227d60",
    "allow_fallback": False,
    "boards": [
        {"name": "Acer", "subvendor": 0x11ad, "subdevice": 0x0807, "key": ACER_BOARD_KEY,
         "sha256": "4c9b92a20a3f11f025a2ea60b605de1344d9b24e575ad5195ab10fdcdf44bf3c",
         "linux_crc32": 0x58f39f4d},
        {"name": "Dell", "subvendor": 0x1028, "subdevice": 0x0310, "key": BOARD_KEY,
         "sha256": "7b01edabe0fc9071abe4d20c7f336fc5e4dc6e2ed4803df54487eef155b16862"},
    ],
}
QCA9377 = {
    "namespace": "Ath10kPayload9377", "dir": "ath10k/QCA9377/hw1.0", "profile": "QCA9377/hw1.1",
    "device": 0x0042, "chip": 1, "target": 0x05020001,
    "firmware_container_sha256": "8f8b002fccfe81d42238f27dd1f56d189604f180bd4772c7c8e75ae1fef16f01",
    "board_container_sha256": "0fdcc7838f478da81704de88f7b33e28862110c6d5decf7818543f8e37e6cd98",
    "allow_fallback": True,
    "fallback_board_sha256": "127d35d82edb46278f30c448cbca664d755ff0d5fed57b649959cdbc4208c768",
    "boards": [
        {"name": "Dell", "subvendor": 0x1028, "subdevice": 0x1810, "key": QCA9377_BOARD_KEY,
         "sha256": "b2713b77c725b0ff81af75c85c3aeba97885d0f40174f715b1e39d5a9d50f4e7"},
    ],
}
SETS = {"kext": (QCA6174,), "test": (QCA6174, QCA9377)}


def c_bytes(name, data):
    lines = [f"static constexpr UInt8 {name}[] = {{"]
    for pos in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{x:02x}" for x in data[pos:pos + 16]) + ",")
    return lines + ["};"]


def prepare_group(fwroot, group, verify_pins=True):
    """Validate one firmware directory and its boards; return (header lines, profile lines, manifest)."""
    folder = Path(fwroot) / group["dir"]
    fw_container = (folder / "firmware-6.bin").read_bytes()
    board_container = (folder / "board-2.bin").read_bytes()
    if verify_pins:
        for what, data in (("firmware", fw_container), ("board", board_container)):
            if sha(data) != group[f"{what}_container_sha256"]:
                raise ValueError(f"{group['profile']} {what} container does not match the pinned SHA-256")
    parts, version = parse_firmware(fw_container)
    segments, entry = parse_segments(parts[3])
    ns = group["namespace"]
    header = [f"namespace {ns} {{",
              f"static constexpr char kFirmwareVersion[] = {json.dumps(version)};",
              f"static constexpr UInt32 kFirmwareSize = {len(parts[3])};",
              f"static constexpr char kFirmwareSHA256[] = {json.dumps(sha(parts[3]))};",
              f"static constexpr UInt32 kOTPSize = {len(parts[4])};",
              f"static constexpr char kOTPSHA256[] = {json.dumps(sha(parts[4]))};",
              f"static constexpr bool kOTPIgnoreResult = {str(ignore_otp_result(parts)).lower()};",
              f"static constexpr bool kSupportsSkipClockInit = {str(supports_skip_clock_init(parts)).lower()};"]
    header += c_bytes("kOTP", parts[4])
    header += ["static_assert(sizeof(kOTP) == kOTPSize, \"OTP payload size\");"]
    header += c_bytes("kFirmware", parts[3])
    header += ["static_assert(sizeof(kFirmware) == kFirmwareSize, \"WLAN payload size\");",
               "static constexpr Ath10kPayload::FirmwareSegment kFirmwareSegments[] = {"]
    header += [f"    {{0x{s['address']:08x}u, {s['offset']}u, {s['length']}u}}," for s in segments]
    header += ["};", f"static constexpr UInt32 kFirmwareSegmentCount = {len(segments)};",
               f"static constexpr UInt32 kFirmwareSegmentDataBytes = {sum(s['length'] for s in segments)};",
               f"static constexpr UInt32 kFirmwareEntryAddress = 0x{entry:08x}u;"]
    manifest = {"profile": group["profile"], "firmware_version": version,
                "firmware_container_sha256": sha(fw_container),
                "board_container_sha256": sha(board_container),
                "firmware_bytes": len(parts[3]), "firmware_sha256": sha(parts[3]),
                "firmware_linux_crc32": f"{linux_crc32(parts[3]):08x}",
                "otp_bytes": len(parts[4]), "otp_sha256": sha(parts[4]),
                "otp_linux_crc32": f"{linux_crc32(parts[4]):08x}",
                "otp_ignore_result_feature": ignore_otp_result(parts),
                "supports_skip_clock_init_feature": supports_skip_clock_init(parts),
                "firmware_segments": segments, "firmware_entry_address": entry,
                "firmware_segment_data_bytes": sum(s['length'] for s in segments),
                "boards": []}
    profiles = []
    for b in group["boards"]:
        selection = "board-api2-exact"
        try:
            board = select_board(board_container, b["key"])
        except BoardNotFound:
            if not group["allow_fallback"]:
                raise
            board = (folder / "board.bin").read_bytes()
            if len(board) != BOARD_SIZE:
                raise ValueError("invalid API1 fallback board size")
            if verify_pins and sha(board) != group["fallback_board_sha256"]:
                raise ValueError(f"{group['profile']} fallback board does not match the pinned SHA-256")
            selection = "board-api1-fallback"
        if verify_pins and selection == "board-api2-exact" and sha(board) != b["sha256"]:
            raise ValueError(f"{b['key']}: board data does not match the pinned SHA-256")
        if verify_pins and "linux_crc32" in b and linux_crc32(board) != b["linux_crc32"]:
            raise ValueError(f"{b['key']}: board crc32 differs from what Linux loads on this card")
        bn = f"kBoard{b['name']}"
        header += [f"static constexpr char {bn}Key[] = {json.dumps(b['key'])};",
                   f"static constexpr char {bn}SHA256[] = {json.dumps(sha(board))};",
                   f"static constexpr char {bn}Selection[] = {json.dumps(selection)};"]
        header += c_bytes(bn, board)
        header += [f"static_assert(sizeof({bn}) == {BOARD_SIZE}, \"board size\");"]
        profiles.append(
            f"    {{0x{group['device']:04x}, 0x{b['subvendor']:04x}, 0x{b['subdevice']:04x}, {group['chip']}, "
            f"0x{group['target']:08x}u, true, {ns}::{bn}Key, {ns}::{bn}Selection, {ns}::{bn}SHA256, "
            f"{ns}::{bn}, sizeof({ns}::{bn}), {ns}::kFirmwareVersion, {ns}::kFirmwareSize, "
            f"{ns}::kFirmwareSHA256, {ns}::kFirmware, {ns}::kOTPSize, {ns}::kOTPSHA256, {ns}::kOTP, "
            f"{ns}::kOTPIgnoreResult, {ns}::kSupportsSkipClockInit, {ns}::kFirmwareSegments, "
            f"{ns}::kFirmwareSegmentCount, {ns}::kFirmwareSegmentDataBytes, {ns}::kFirmwareEntryAddress}},")
        manifest["boards"].append({"key": b["key"], "selection": selection, "bytes": len(board),
                                   "sha256": sha(board), "linux_crc32": f"{linux_crc32(board):08x}"})
        print(f"Prepared {group['profile']} {b['name']}: {selection}; firmware {version}; OTP {len(parts[4])} bytes")
    # The first board of the QCA6174 group is this repo's target (Acer); tests use
    # these aliases for "the default profile".
    first = f"kBoard{group['boards'][0]['name']}"
    header += [f"static constexpr const UInt8 *kBoard = {first};",
               f"static constexpr const char *kBoardKey = {first}Key;", "}", ""]
    return header, profiles, manifest


def prepare(fwroot, out, which="kext", verify_pins=True):
    header = ["// Generated by scripts/prepare-firmware.py from pinned linux-firmware files. Do not edit.",
              "// The firmware bytes below are Qualcomm's (firmware/LICENSE.QualcommAtheros_ath10k).",
              "#pragma once", '#include "Ath10kPayloadProfile.hpp"']
    entries, manifests = [], []
    for group in SETS[which]:
        lines, profiles, manifest = prepare_group(fwroot, group, verify_pins)
        header += lines
        entries += profiles
        manifests.append(manifest)
    header += ["namespace Ath10kPayload {", "static constexpr Profile kProfiles[] = {", *entries, "};",
               "static constexpr Profile kUnavailable = {};",
               "// Exact allowlist: PCI vendor/device, subsystem vendor/device and chip revision.",
               "const Profile &selectProfile(UInt16 vendor, UInt16 device, UInt16 subvendor, UInt16 subdevice, UInt8 chip) {",
               "    if (vendor == 0x168c) for (const auto &p : kProfiles)",
               "        if (p.device == device && p.subvendor == subvendor && p.subdevice == subdevice && p.chip == chip) return p;",
               "    return kUnavailable;", "}",
               "bool subsystemListed(UInt16 vendor, UInt16 device, UInt16 subvendor, UInt16 subdevice) {",
               "    if (vendor == 0x168c) for (const auto &p : kProfiles)",
               "        if (p.device == device && p.subvendor == subvendor && p.subdevice == subdevice) return true;",
               "    return false;", "}", "}", ""]
    out = Path(out)
    files = {out / "Ath10kPayload.hpp": "\n".join(header).encode(),
             out / "manifest.json": (json.dumps({"set": which, "groups": manifests}, indent=2) + "\n").encode()}
    out.mkdir(parents=True, exist_ok=True)
    for path, content in files.items():
        if path.exists() and path.read_bytes() == content:
            continue
        tmp = path.with_name(path.name + ".tmp")
        tmp.write_bytes(content)
        tmp.replace(path)
    return files


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--firmware", type=Path, default=root / "build/firmware",
                        help="directory filled by scripts/fetch-firmware.sh")
    parser.add_argument("--set", choices=sorted(SETS), default="kext")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    try:
        prepare(args.firmware, args.out or root / "build/gen" / args.set, args.set)
    except (OSError, ValueError) as error:
        print(f"Payload preparation FAILED: {error}", file=sys.stderr)
        sys.exit(1)
