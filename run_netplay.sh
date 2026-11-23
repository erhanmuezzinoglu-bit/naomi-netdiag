#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ROLE="${1:-}"
case "$ROLE" in
    host|join) ;;
    *)
        echo "Kullanim: $0 host|join" >&2
        exit 2
        ;;
esac

FLYCAST_SRC="${FLYCAST_SRC:-$HOME/Downloads/flycast}"
FLYCAST_BIN="${FLYCAST_BIN:-$FLYCAST_SRC/build/flycast}"
PACKER="${PACKER:-$SCRIPT_DIR/pack_netdimm_hle.py}"
NAOMI_ENV="${NAOMI_ENV:-/opt/toolchains/naomi/env.sh}"
ROM_BIN="$SCRIPT_DIR/netplay_${ROLE}.bin"
OUT_DIR="$SCRIPT_DIR/netplay_${ROLE}_hle"
GAME_ZIP="$OUT_DIR/mj1.zip"

if [[ -f "$NAOMI_ENV" ]]; then
    # shellcheck disable=SC1090
    source "$NAOMI_ENV"
fi

for tool in make python3 genisoimage chdman; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "HATA: Gerekli komut bulunamadi: $tool" >&2
        exit 1
    fi
done

if [[ ! -x "$FLYCAST_BIN" ]]; then
    echo "HATA: Flycast bulunamadi: $FLYCAST_BIN" >&2
    exit 1
fi
if [[ ! -f "$PACKER" ]]; then
    echo "HATA: Paketleyici bulunamadi: $PACKER" >&2
    exit 1
fi

printf '============================================================\n'
printf '[1/3] Naomi NetPlay %s ROM derleniyor...\n' "$ROLE"
printf '============================================================\n'
make -B -C "$SCRIPT_DIR" "APP=$ROLE"

if [[ ! -s "$ROM_BIN" ]]; then
    echo "HATA: ROM olusturulamadi: $ROM_BIN" >&2
    exit 1
fi

printf '\n============================================================\n'
printf '[2/3] NetDIMM HLE paketi hazirlaniyor...\n'
printf '============================================================\n'
mkdir -p "$OUT_DIR"
rm -f -- "$GAME_ZIP" "$OUT_DIR/mj1/cdp-10002b.chd"
python3 "$PACKER" "$ROM_BIN" --flycast-src "$FLYCAST_SRC" --out "$OUT_DIR"

if [[ ! -s "$GAME_ZIP" || ! -s "$OUT_DIR/mj1/cdp-10002b.chd" ]]; then
    echo "HATA: NetDIMM HLE paketi tamamlanamadi." >&2
    exit 1
fi

printf '\n============================================================\n'
printf '[3/3] Flycast %s istemcisi baslatiliyor...\n' "$ROLE"
printf 'Oyun arsivi: %s\n' "$GAME_ZIP"
printf 'Relay: configured IPv4 UDP port 19001\n'
printf '============================================================\n'
exec env LIBGL_ALWAYS_SOFTWARE=1 "$FLYCAST_BIN" \
    -config Network:Enable=0 \
    -config Debug:ConsoleOutput=1 \
    -config Debug:Analyze=1 \
    "$GAME_ZIP"
