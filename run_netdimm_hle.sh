#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLYCAST_BIN="$HOME/Downloads/flycast/build/flycast"
NETDIMM_ZIP="$SCRIPT_DIR/netdimm_hle/mj1.zip"

if [[ ! -x "$FLYCAST_BIN" ]]; then
    echo "Hata: Flycast bulunamadı: $FLYCAST_BIN" >&2
    exit 1
fi
if [[ ! -f "$NETDIMM_ZIP" ]]; then
    echo "Hata: $NETDIMM_ZIP bulunamadı." >&2
    echo "Önce pack_netdimm_hle.py ile mj1.zip ve CHD üret." >&2
    exit 1
fi
if [[ ! -f "$SCRIPT_DIR/netdimm_hle/mj1/cdp-10002b.chd" ]]; then
    echo "Hata: NetDIMM CHD bulunamadı." >&2
    exit 1
fi

echo "Flycast NetDIMM HLE preflight başlatılıyor: $NETDIMM_ZIP"
echo "Flycast kaynak kodu değiştirilmedi."
LIBGL_ALWAYS_SOFTWARE=1 "$FLYCAST_BIN" \
    -config Network:Enable=0 \
    -config Debug:ConsoleOutput=1 \
    -config Debug:Analyze=1 \
    "$NETDIMM_ZIP"
