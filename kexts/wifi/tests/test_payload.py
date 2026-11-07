import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare", ROOT / "scripts/prepare-firmware.py")
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


def ie(tag, data):
    return struct.pack("<II", tag, len(data)) + data + b"x" * (-len(data) % 4)


def container(magic, data):
    return magic + b"m" * (-len(magic) % 4) + data


def board_entry(key, value):
    return ie(0, ie(0, key.encode()) + ie(1, value))


FW = ROOT / "build/firmware"
QCA6174_DIR = FW / "ath10k/QCA6174/hw3.0"
QCA9377_DIR = FW / "ath10k/QCA9377/hw1.0"


class PayloadTests(unittest.TestCase):
    def test_real_inputs(self):
        board = p.select_board((QCA6174_DIR / "board-2.bin").read_bytes())
        self.assertEqual(len(board), 8124)
        parts, version = p.parse_firmware((QCA6174_DIR / "firmware-6.bin").read_bytes())
        self.assertEqual(version, "WLAN.RM.4.4.1-00309-")
        self.assertEqual(len(parts[3]), 681821)
        self.assertEqual(len(parts[4]), 24429)
        self.assertTrue(p.ignore_otp_result(parts))
        self.assertFalse(p.supports_skip_clock_init(parts))
        segments, entry = p.parse_segments(parts[3])
        self.assertEqual(entry, 0xa0400)
        self.assertEqual(len(segments), 6)
        self.assertEqual(sum(s['length'] for s in segments), 681749)

    def test_acer_board_is_what_linux_loads(self):
        # Linux ath10k on the Helios 300 (evidence/linux-debugfs-fw_checksums.txt):
        # firmware-N.bin 0793bcf2, athwlan 9986d09a, otp 3871d397, board 58f39f4d.
        fw = (QCA6174_DIR / "firmware-6.bin").read_bytes()
        self.assertEqual(p.linux_crc32(fw), 0x0793bcf2)
        parts, _ = p.parse_firmware(fw)
        self.assertEqual(p.linux_crc32(parts[3]), 0x9986d09a)
        self.assertEqual(p.linux_crc32(parts[4]), 0x3871d397)
        container = (QCA6174_DIR / "board-2.bin").read_bytes()
        self.assertEqual(p.linux_crc32(container), 0xd2863f91)
        board = p.select_board(container, p.ACER_BOARD_KEY)
        self.assertEqual(p.linux_crc32(board), 0x58f39f4d)
        self.assertNotEqual(board, p.select_board(container, p.BOARD_KEY))
        # Linux falls back from ",variant=..." to the plain name; the plain name must
        # not match a variant entry by prefix.
        self.assertEqual(p.ACER_BOARD_KEY,
            "bus=pci,vendor=168c,device=003e,subsystem-vendor=11ad,subsystem-device=0807")

    def test_generated_kext_set(self):
        import json
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            files = p.prepare(FW, temp, "kext")
            header = files[Path(temp) / "Ath10kPayload.hpp"].decode()
            manifest = json.loads(files[Path(temp) / "manifest.json"])
        self.assertEqual([g["profile"] for g in manifest["groups"]], ["QCA6174/hw3.2"])
        boards = manifest["groups"][0]["boards"]
        self.assertEqual(boards[0]["key"], p.ACER_BOARD_KEY)
        self.assertEqual(boards[0]["linux_crc32"], "58f39f4d")
        self.assertEqual(boards[0]["selection"], "board-api2-exact")
        self.assertNotIn("Ath10kPayload9377", header)  # no QCA9377 firmware in the kext
        self.assertEqual(header.count("static constexpr UInt8 kFirmware[]"), 1)  # shared, not per board
        self.assertIn("{0x003e, 0x11ad, 0x0807, 10, 0x05030000u, true", header)

    def test_pins_reject_other_inputs(self):
        import shutil
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            for group in (p.QCA6174, p.QCA9377):
                (tmp / group["dir"]).mkdir(parents=True)
                for name in ("board-2.bin", "board.bin", "firmware-6.bin"):
                    src = FW / group["dir"] / name
                    if src.exists():
                        shutil.copyfile(src, tmp / group["dir"] / name)
            p.prepare_group(tmp, p.QCA6174)
            d = tmp / p.QCA6174["dir"]
            data = bytearray((d / "board-2.bin").read_bytes()); data[-1] ^= 1
            (d / "board-2.bin").write_bytes(bytes(data))
            with self.assertRaises(ValueError):
                p.prepare_group(tmp, p.QCA6174)
            shutil.copyfile(QCA9377_DIR / "firmware-6.bin", d / "firmware-6.bin")
            with self.assertRaises(ValueError):
                p.prepare_group(tmp, p.QCA6174, verify_pins=True)

    def test_qca9377_profile(self):
        lines, profiles, manifest = p.prepare_group(FW, p.QCA9377)
        self.assertEqual(manifest['firmware_version'], 'WLAN.TF.2.1-00021-QCARMSWP-1')
        self.assertEqual(manifest['otp_bytes'], 24193)
        self.assertEqual(manifest['firmware_bytes'], 727125)
        self.assertEqual(manifest['firmware_segment_data_bytes'], 727053)
        self.assertEqual(manifest['boards'][0]['selection'], 'board-api2-exact')
        self.assertEqual(manifest['boards'][0]['sha256'],
            'b2713b77c725b0ff81af75c85c3aeba97885d0f40174f715b1e39d5a9d50f4e7')

    def test_exact_board_rejects_corrupt_container_and_api1_fallback(self):
        import shutil
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            d = tmp / p.QCA9377["dir"]; d.mkdir(parents=True)
            for name in ('board-2.bin', 'board.bin', 'firmware-6.bin'):
                shutil.copyfile(QCA9377_DIR / name, d / name)
            bad = [b'corrupt', container(p.BOARD_MAGIC, board_entry(p.QCA9377_BOARD_KEY, b'a'*8124)*2),
                   container(p.BOARD_MAGIC, board_entry(p.QCA9377_BOARD_KEY, b'a'*8000))]
            for data in bad:
                (d/'board-2.bin').write_bytes(data)
                with self.assertRaises(ValueError):
                    p.prepare_group(tmp, p.QCA9377, verify_pins=False)
            foreign = container(p.BOARD_MAGIC, board_entry(
                'bus=pci,vendor=168c,device=0042,subsystem-vendor=17aa,subsystem-device=0901', b'a'*8124))
            (d/'board-2.bin').write_bytes(foreign)
            manifest = p.prepare_group(tmp, p.QCA9377, verify_pins=False)[2]
            self.assertEqual(manifest['boards'][0]['selection'], 'board-api1-fallback')
            # QCA6174 never falls back to board.bin: a missing exact key is an error.
            d6 = tmp / p.QCA6174["dir"]; d6.mkdir(parents=True)
            shutil.copyfile(QCA6174_DIR / 'firmware-6.bin', d6 / 'firmware-6.bin')
            (d6 / 'board-2.bin').write_bytes(container(p.BOARD_MAGIC, board_entry(p.BOARD_KEY, b'a'*8124)))
            with self.assertRaises(p.BoardNotFound):
                p.prepare_group(tmp, p.QCA6174, verify_pins=False)

    def test_segmented_image_validation(self):
        header = struct.pack('<II', 0x544d4753, 0)
        segment = struct.pack('<II', 0x500000, 5) + b'abcde'
        entry = struct.pack('<II', 0x500000, 0xfffffffd)
        done = struct.pack('<II', 0, 0xffffffff)
        self.assertEqual(p.parse_segments(header + segment + entry + done)[0][0]['length'], 5)
        invalid = [b'', header + segment, header + segment + done,
                   header + segment + entry + done + b'x',
                   header + segment + entry + entry + done,
                   header + struct.pack('<II', 0x500000, 0xfffffffc),
                   header + struct.pack('<II', 0x500000, 1000) + b'abc',
                   header + segment + segment + entry + done,
                   header + struct.pack('<II', 0xfffffffc, 5) + b'abcde' + entry + done,
                   header + struct.pack('<II', 0x400800, 5) + b'abcde' + entry + done,
                   header + segment + struct.pack('<II', 0x600000, 0xfffffffd) + done]
        for data in invalid:
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                p.parse_segments(data)

    def test_otp_feature_bit(self):
        self.assertFalse(p.ignore_otp_result({}))
        self.assertFalse(p.ignore_otp_result({2: b""}))
        self.assertFalse(p.ignore_otp_result({2: b"\x40\x80"}))
        self.assertTrue(p.ignore_otp_result({2: b"\x80"}))

    def test_skip_clock_feature_bit(self):
        for value in (b"", b"\x02", b"\xc0\x10"):
            self.assertFalse(p.supports_skip_clock_init({2: value}))
        self.assertFalse(p.supports_skip_clock_init({}))
        self.assertTrue(p.supports_skip_clock_init({2: b"\xc0\x12"}))

    def test_exact_not_prefix_match(self):
        data = container(p.BOARD_MAGIC,
            board_entry(p.BOARD_KEY + ",variant=other", b"b" * 8124) +
            board_entry(p.BOARD_KEY, b"a" * 8124))
        self.assertEqual(p.select_board(data), b"a" * 8124)

    def test_missing_duplicate_wrong_size(self):
        entries = [board_entry("other", b"a" * 8124),
                   board_entry(p.BOARD_KEY, b"a" * 8124) * 2,
                   board_entry(p.BOARD_KEY, b"a" * 8123)]
        for entry in entries:
            with self.subTest(size=len(entry)), self.assertRaises(ValueError):
                p.select_board(container(p.BOARD_MAGIC, entry))

    def test_truncated_and_hostile_lengths(self):
        for data in (b"", p.BOARD_MAGIC, container(p.BOARD_MAGIC, b"a"),
                     container(p.BOARD_MAGIC, struct.pack("<II", 0, 0xffffffff)),
                     container(p.BOARD_MAGIC, ie(0, struct.pack("<II", 1, 8000)))):
            with self.subTest(data=data[:25]), self.assertRaises(ValueError):
                p.select_board(data)

    def test_firmware_rejects_missing_duplicate_codeswap_wrong_ops(self):
        good = ie(0, b"version") + ie(3, b"firmware") + ie(4, b"otp") + ie(5, struct.pack("<I", 4)) + ie(6, struct.pack("<I", 3))
        for body in (good + ie(3, b"second"), good + ie(7, b"swap"),
                     ie(0, b"version"), good.replace(struct.pack("<I", 4), struct.pack("<I", 9))):
            with self.subTest(size=len(body)), self.assertRaises(ValueError):
                p.parse_firmware(container(p.FW_MAGIC, body))


if __name__ == "__main__":
    unittest.main()
