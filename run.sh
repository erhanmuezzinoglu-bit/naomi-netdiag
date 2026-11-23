#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

FLYCAST_SRC="${FLYCAST_SRC:-$HOME/Downloads/flycast}"
FLYCAST_BIN="${FLYCAST_BIN:-$FLYCAST_SRC/build/flycast}"
ROM_BIN="${ROM_BIN:-$SCRIPT_DIR/netdimm.bin}"
PACKER="${PACKER:-$SCRIPT_DIR/pack_netdimm_hle.py}"
OUT_DIR="${OUT_DIR:-$SCRIPT_DIR/netdimm_hle}"
GAME_ZIP="$OUT_DIR/mj1.zip"

NAOMI_ENV="${NAOMI_ENV:-/opt/toolchains/naomi/env.sh}"
if [[ -f "$NAOMI_ENV" ]]; then
    # shellcheck disable=SC1090
    source "$NAOMI_ENV"
fi

for tool in make python3 genisoimage chdman; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "HATA: Gerekli komut bulunamadı: $tool" >&2
        exit 1
    fi
done

if [[ ! -x "$FLYCAST_BIN" ]]; then
    echo "HATA: Flycast çalıştırılabilir dosyası bulunamadı: $FLYCAST_BIN" >&2
    exit 1
fi
if [[ ! -f "$PACKER" ]]; then
    echo "HATA: Paketleyici bulunamadı: $PACKER" >&2
    exit 1
fi

if ! grep -Eq 'netdimm_net\.o|netdimm_net\.c' "$SCRIPT_DIR/Makefile" || \
   ! grep -Eq 'netdimm_dns\.o|netdimm_dns\.c' "$SCRIPT_DIR/Makefile"; then
    cat >&2 <<'MSG'
HATA: Makefile, yeni NetDIMM sürücü modülünü henüz derlemiyor.
Makefile kaynak listesine netdimm_net.c ve netdimm_dns.c ekle.
Örnek: SRCS += netdimm_net.c
       SRCS += netdimm_dns.c
Ardından ./run.sh komutunu yeniden çalıştır.
MSG
    exit 1
fi

echo "============================================================"
echo "[1/3] Naomi ROM zorunlu olarak yeniden derleniyor..."
echo "============================================================"
make -B -C "$SCRIPT_DIR"

if [[ ! -s "$ROM_BIN" ]]; then
    echo "HATA: Derleme sonrasında ROM bulunamadı veya boş: $ROM_BIN" >&2
    exit 1
fi

echo
echo "============================================================"
echo "[2/3] ROM.BIN MAME araçlarıyla GD-ROM CHD paketine çevriliyor..."
echo "============================================================"
mkdir -p "$OUT_DIR"
rm -f -- "$GAME_ZIP" "$OUT_DIR/mj1/cdp-10002b.chd"
python3 "$PACKER" "$ROM_BIN" \
    --flycast-src "$FLYCAST_SRC" \
    --out "$OUT_DIR"

if [[ ! -s "$GAME_ZIP" || ! -s "$OUT_DIR/mj1/cdp-10002b.chd" ]]; then
    echo "HATA: NetDIMM HLE paketi tamamlanmadı." >&2
    exit 1
fi

echo
echo "============================================================"
echo "[3/3] Flycast NetDIMM HLE başlatılıyor..."
echo "Oyun arşivi: $GAME_ZIP"
echo "============================================================"
exec env LIBGL_ALWAYS_SOFTWARE=1 "$FLYCAST_BIN" \
    -config Network:Enable=0 \
    -config Debug:ConsoleOutput=1 \
    -config Debug:Analyze=1 \
    "$GAME_ZIP"
