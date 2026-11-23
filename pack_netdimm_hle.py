#!/usr/bin/env python3
"""Build a synthetic mj1 GD-ROM wrapper for Flycast's built-in NetDIMM HLE.

This does NOT modify Flycast. It uses the existing Flycast gdcartridge.cpp DES
implementation to encrypt a padded libnaomi .bin into an ISO9660 ROM/ROM.BIN,
then wraps that ISO as the CHD expected by Flycast's built-in mj1 metadata.

Requirements on Debian:
  sudo apt install g++ genisoimage mame-tools

Usage:
  python3 pack_netdimm_hle.py ./netdimm.bin \
      --flycast-src "$HOME/Downloads/flycast" \
      --out ./netdimm_hle

Output:
  ./netdimm_hle/mj1.zip
  ./netdimm_hle/mj1/cdp-10002b.chd
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

PIC_NAME = "317-0352-jpn.pic"
GDROM_NAME = "cdp-10002b"
ROM_SIZE_OPTIONS = (128, 256, 512)  # MiB; Flycast NetDimm HLE supports these sizes.
DEFAULT_KEY = "0123456789ABCDEF"


def require_tool(*names: str) -> str:
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    raise RuntimeError("Gerekli araç bulunamadı: " + " veya ".join(names))


def run_checked(command: list[str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def _extract_des_from_text(source: str, source_name: str) -> str:
    """Extract Flycast's DES tables and routines without depending on rev64()."""
    import re

    start_match = re.search(
        r"(?m)^[ \t]*[^;\n]*\bDES_LEFTSWAP\s*\[[^\]]*\]\s*=\s*\{",
        source,
    )
    if start_match is None:
        raise RuntimeError(f"{source_name}: DES_LEFTSWAP tablosu yok")

    # rev64() is only a byte-reversal helper around the 64-bit key. It is not
    # part of the DES implementation and is supplied locally in the generated
    # helper below. Older/newer Flycast trees may not define it in this file.
    encrypt_match = re.search(
        r"\bdes_encrypt_decrypt\s*\(",
        source[start_match.start():],
    )
    if encrypt_match is None:
        raise RuntimeError(f"{source_name}: des_encrypt_decrypt() şablonu bulunamadı")

    signature_start = start_match.start() + encrypt_match.start()
    body_open = source.find("{", signature_start)
    if body_open < 0:
        raise RuntimeError(f"{source_name}: des_encrypt_decrypt() gövdesi bulunamadı")

    # Find the matching closing brace, skipping comments and string/char literals.
    depth = 0
    i = body_open
    state = "code"
    while i < len(source):
        ch = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                state = "line_comment"
                i += 2
                continue
            if ch == "/" and nxt == "*":
                state = "block_comment"
                i += 2
                continue
            if ch == '"':
                state = "string"
                i += 1
                continue
            if ch == "'":
                state = "char"
                i += 1
                continue
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = i + 1
                    break
        elif state == "line_comment":
            if ch == "\n":
                state = "code"
        elif state == "block_comment":
            if ch == "*" and nxt == "/":
                state = "code"
                i += 2
                continue
        elif state == "string":
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                state = "code"
        elif state == "char":
            if ch == "\\":
                i += 2
                continue
            if ch == "'":
                state = "code"
        i += 1
    else:
        raise RuntimeError(f"{source_name}: des_encrypt_decrypt() kapanış parantezi bulunamadı")

    snippet = source[start_match.start():end]
    required = (
        "DES_LEFTSWAP",
        "DES_RIGHTSWAP",
        "DES_SBOX1",
        "DES_SBOX8",
        "DES_MASK_TABLE",
        "DES_ROTATE_TABLE",
        "permutate(",
        "des_generate_subkeys",
        "des_encrypt_decrypt",
    )
    missing = [token for token in required if token not in snippet]
    if missing:
        raise RuntimeError(
            f"{source_name}: DES kodunda eksik parçalar: " + ", ".join(missing)
        )

    # Newer Flycast revisions moved the DES tables and routines into
    # GDCartridge as static class members. The extracted helper is a single
    # standalone translation unit, so strip only the class qualification:
    #   const u32 GDCartridge::DES_LEFTSWAP[] -> const u32 DES_LEFTSWAP[]
    #   u64 GDCartridge::des_encrypt_decrypt(...) -> u64 des_encrypt_decrypt(...)
    # This preserves the original routine bodies and constants verbatim.
    snippet = re.sub(r"\bGDCartridge::", "", snippet)
    return snippet

def extract_des_code(flycast_cpp: Path) -> str:
    """Extract Flycast's DES implementation, tolerating source layout changes."""
    candidates = [flycast_cpp]

    # Some Flycast revisions may move the DES helpers out of gdcartridge.cpp.
    # Search the Naomi source subtree as a fallback, without touching it.
    naomi_dir = flycast_cpp.parent
    candidates.extend(
        path
        for pattern in ("*.cpp", "*.h", "*.hpp")
        for path in sorted(naomi_dir.rglob(pattern))
        if path != flycast_cpp
    )

    failures = []
    for candidate in candidates:
        try:
            source = candidate.read_text(encoding="utf-8")
        except (OSError, UnicodeError):
            continue
        if "DES_LEFTSWAP" not in source or "des_encrypt_decrypt" not in source:
            continue
        try:
            snippet = _extract_des_from_text(source, str(candidate))
            print(f"DES implementation: {candidate}", flush=True)
            return snippet
        except RuntimeError as exc:
            failures.append(str(exc))

    details = " | ".join(failures[:4])
    if not details:
        details = "Naomi kaynak dizininde DES_LEFTSWAP ve des_encrypt_decrypt sembolleri birlikte bulunamadı"
    raise RuntimeError("Flycast DES kodu ayıklanamadı: " + details)


def make_encryptor(flycast_cpp: Path, work: Path) -> Path:
    snippet = extract_des_code(flycast_cpp)
    import re

    # Old Flycast versions expose DES as template<bool decrypt>, while newer
    # versions use a decrypt-only member routine. In the latter case, feeding
    # the round keys in reverse order makes that same routine perform DES
    # encryption (DES encrypt/decrypt differ only by round-key order).
    templated_des = re.search(
        r"template\s*<\s*bool\s+decrypt(?:\s*=\s*(?:true|false))?\s*>[\s\S]{0,300}\bdes_encrypt_decrypt\s*\(",
        snippet,
    ) is not None

    # For non-template revisions, inspect the routine itself to see which end
    # of the 16-round schedule it consumes. The common GDCartridge implementation
    # used by newer builds starts at round 15 (subkey 30) and decrypts. Reversing
    # the round-key pairs therefore turns it into the required encryptor.
    reverse_round_keys = False
    if not templated_des:
        fn_match = re.search(r"\bdes_encrypt_decrypt\s*\([^)]*\)\s*\{", snippet)
        if fn_match is None:
            raise RuntimeError("Flycast DES encrypt/decrypt fonksiyon gövdesi çözümlenemedi")
        fn_body = snippet[fn_match.end():]
        starts_reverse = re.search(r"\bsubkey\s*=\s*30\b|\bdes_subkeys\s*\[\s*30\s*\]", fn_body)
        starts_forward = re.search(r"\bsubkey\s*=\s*0\b|\bdes_subkeys\s*\[\s*0\s*\]", fn_body)
        if starts_reverse:
            reverse_round_keys = True
        elif starts_forward:
            reverse_round_keys = False
        else:
            raise RuntimeError(
                "Flycast DES anahtar sırası otomatik belirlenemedi. "
                "gdcartridge.cpp içindeki des_encrypt_decrypt() gövdesini incelemek gerekiyor."
            )

    cpp = work / "flycast_des_encrypt.cpp"
    cpp.write_text(
        """// Standalone encryptor generated from Flycast gdcartridge.cpp DES routines.
// Flycast source remains unchanged; this helper only prepares the test disc.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

"""
        + snippet
        + r"""
// The Flycast DES routine expects its 64-bit key bytes in reversed order.
// Keep this tiny helper local so extraction does not depend on Flycast's
// separate rev64() utility being present in this source revision.
static inline u64 reverse_key_bytes(u64 src)
{
    u64 ret = 0;
    for (unsigned i = 0; i < 8; ++i) {
        ret = (ret << 8) | (src & 0xff);
        src >>= 8;
    }
    return ret;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: encrypt INPUT OUTPUT KEYHEX\n");
        return 2;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(argv[3], &end, 16);
    if (end == argv[3] || *end != '\0') {
        std::fprintf(stderr, "invalid key: %s\n", argv[3]);
        return 2;
    }

    FILE *in = std::fopen(argv[1], "rb");
    if (!in) { std::perror("open input"); return 1; }
    FILE *out = std::fopen(argv[2], "wb");
    if (!out) { std::perror("open output"); std::fclose(in); return 1; }

    u32 subkeys[32];
    des_generate_subkeys(reverse_key_bytes(static_cast<u64>(parsed)), subkeys);

    // DES decryption with reversed 2-word round-key pairs is encryption.
    // The legacy templated Flycast implementation has an explicit encrypt mode.
    if (DES_REVERSE_ROUND_KEYS) {
        u32 reversed[32];
        for (unsigned round = 0; round < 16; ++round) {
            reversed[round * 2] = subkeys[(15 - round) * 2];
            reversed[round * 2 + 1] = subkeys[(15 - round) * 2 + 1];
        }
        std::memcpy(subkeys, reversed, sizeof(subkeys));
    }

    u64 block;
    std::size_t n;
    while ((n = std::fread(&block, 1, sizeof(block), in)) == sizeof(block)) {
        block = DES_ENCRYPT_CALL(block, subkeys);
        if (std::fwrite(&block, 1, sizeof(block), out) != sizeof(block)) {
            std::perror("write output");
            std::fclose(in); std::fclose(out); return 1;
        }
    }
    if (n != 0 || std::ferror(in)) {
        std::fprintf(stderr, "input size must be a multiple of 8 bytes\n");
        std::fclose(in); std::fclose(out); return 1;
    }
    if (std::fclose(in) != 0 || std::fclose(out) != 0) {
        std::perror("close files"); return 1;
    }
    return 0;
}
""",
        encoding="utf-8",
    )
    generated = cpp.read_text(encoding="utf-8")
    generated = generated.replace("DES_REVERSE_ROUND_KEYS", "true" if reverse_round_keys else "false")
    generated = generated.replace(
        "DES_ENCRYPT_CALL",
        "des_encrypt_decrypt<false>" if templated_des else "des_encrypt_decrypt",
    )
    cpp.write_text(generated, encoding="utf-8")
    exe = work / "flycast_des_encrypt"
    compiler = require_tool("g++")
    run_checked([compiler, "-std=c++17", "-O3", "-Wall", "-Wextra", str(cpp), "-o", str(exe)])
    return exe


def make_synthetic_pic(key_hex: str) -> bytes:
    key_bytes = bytes.fromhex(key_hex)
    if len(key_bytes) != 8:
        raise ValueError("DES anahtarı tam 16 hex karakter (8 byte) olmalı.")

    pic = bytearray(0x4000)
    # Flycast reads these eight bytes to build its GD-ROM DES key.
    for i in range(7):
        pic[0x780 + i * 2] = key_bytes[i]
    pic[0x7A0] = key_bytes[7]

    # Select the NetPIC-style ISO layout: PVD at LBA 16, then /ROM/ROM.BIN.
    pic[0x6EE] = 1

    # The name is not used in NetPIC mode, but keep the fields printable.
    first = b"HOMEbrew"
    second = b"NETDimm"
    for i in range(7):
        pic[0x7C0 + i * 2] = first[i]
        pic[0x7E0 + i * 2] = second[i]
    return bytes(pic)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_bin", type=Path, help="libnaomi makerom çıktısı .bin")
    parser.add_argument(
        "--flycast-src",
        type=Path,
        default=Path.home() / "Downloads/flycast",
        help="Flycast kaynak ağacının kök dizini",
    )
    parser.add_argument("--out", type=Path, default=Path("./netdimm_hle"))
    parser.add_argument("--key", default=DEFAULT_KEY, help="16 haneli hex DES anahtarı")
    args = parser.parse_args()

    input_bin = args.input_bin.expanduser().resolve()
    flycast_src = args.flycast_src.expanduser().resolve()
    out = args.out.expanduser().resolve()
    gdcartridge_cpp = flycast_src / "core/hw/naomi/gdcartridge.cpp"

    if not input_bin.is_file():
        raise RuntimeError(f"BIN bulunamadı: {input_bin}")
    if not gdcartridge_cpp.is_file():
        raise RuntimeError(f"Flycast kaynak dosyası bulunamadı: {gdcartridge_cpp}")

    key_hex = args.key.upper().removeprefix("0X")
    if len(key_hex) != 16:
        raise RuntimeError("--key 16 hex karakter (8 byte) olmalı.")
    try:
        bytes.fromhex(key_hex)
    except ValueError as exc:
        raise RuntimeError("--key yalnızca hex karakter içermeli.") from exc

    input_size = input_bin.stat().st_size
    chosen_mib = next((n for n in ROM_SIZE_OPTIONS if input_size <= n * 1024 * 1024), None)
    if chosen_mib is None:
        raise RuntimeError("BIN 512 MiB sınırını aşıyor; bu paketleyici desteklemiyor.")
    dimm_bytes = chosen_mib * 1024 * 1024
    if dimm_bytes % 8:
        raise RuntimeError("Seçilen DIMM boyutu 8 byte hizalı değil.")

    geniso = require_tool("genisoimage", "mkisofs")
    chdman = require_tool("chdman")

    out.mkdir(parents=True, exist_ok=True)
    game_dir = out / "mj1"
    game_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="netdimm_hle_") as temp_name:
        work = Path(temp_name)
        encryptor = make_encryptor(gdcartridge_cpp, work)

        plain = work / "ROM.BIN.plain"
        with input_bin.open("rb") as src, plain.open("wb") as dst:
            shutil.copyfileobj(src, dst, length=1024 * 1024)
            dst.truncate(dimm_bytes)  # zero-pad to a DIMM size the HLE accepts

        encrypted = work / "ROM.BIN"
        run_checked([str(encryptor), str(plain), str(encrypted), key_hex])
        if encrypted.stat().st_size != dimm_bytes:
            raise RuntimeError("Şifrelenmiş ROM.BIN boyutu beklenenden farklı.")

        disc_root = work / "disc_root"
        rom_dir = disc_root / "ROM"
        rom_dir.mkdir(parents=True)
        shutil.copyfile(encrypted, rom_dir / "ROM.BIN")

        iso = work / "disc.iso"
        run_checked([
            geniso,
            "-quiet",
            "-iso-level", "1",
            "-V", "NETDIMM",
            "-o", str(iso),
            str(disc_root),
        ])

        cue = work / "disc.cue"
        cue.write_text(
            'FILE "disc.iso" BINARY\n'
            '  TRACK 01 MODE1/2048\n'
            '    INDEX 01 00:00:00\n',
            encoding="ascii",
        )
        chd_path = game_dir / f"{GDROM_NAME}.chd"
        if chd_path.exists():
            chd_path.unlink()
        run_checked([chdman, "createcd", "-i", str(cue), "-o", str(chd_path)])

    pic_path = out / PIC_NAME
    pic_path.write_bytes(make_synthetic_pic(key_hex))
    zip_path = out / "mj1.zip"
    if zip_path.exists():
        zip_path.unlink()
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.write(pic_path, arcname=PIC_NAME)
    pic_path.unlink()

    print("\nHazır.")
    print(f"  MAME tanımlayıcı arşivi : {zip_path}")
    print(f"  GD-ROM CHD              : {chd_path}")
    print(f"  ROM.BIN / DIMM boyutu   : {chosen_mib} MiB")
    print("\nFlycast'e mj1.zip dosyasını açtır. CHD, zip ile aynı dizindeki")
    print("mj1/cdp-10002b.chd yolunda bulunmalı. Flycast kaynak kodu değiştirilmedi.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"HATA: {exc}", file=sys.stderr)
        raise SystemExit(1)
