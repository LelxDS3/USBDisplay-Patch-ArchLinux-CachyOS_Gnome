#!/bin/bash
# cambiar-resolucion.sh <ANCHO> <ALTO> [rate]
# Fija la resolucion del monitor USB MacroSilicon en monitors.xml y reinicia.
# Cambiar EN CALIENTE desde GNOME crashea el kernel (use-after-free del fb);
# monitors.xml se aplica al iniciar sesion, sin modeset en caliente.
# Uso: sudo ./cambiar-resolucion.sh 800 600 [59.96]
# EN: Set the MacroSilicon USB monitor resolution in monitors.xml and reboot.
# Hot-changing from GNOME crashes the kernel (fb use-after-free); monitors.xml
# is applied at login, no hot modeset.

set -e

W="$1"
H="$2"
RATE="$3"

if [ -z "$W" ] || [ -z "$H" ]; then
    echo "Uso: sudo $0 <ANCHO> <ALTO> [rate]   ej: sudo $0 800 600" >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: ejecuta con sudo." >&2
    exit 1
fi

# Usuario real (con sudo HOME apunta a /root)
# EN: Real user (sudo sets HOME to /root)
RUNUSER="${SUDO_USER:-$USER}"
USER_HOME="$(getent passwd "$RUNUSER" | cut -d: -f6)"
CONF="$USER_HOME/.config/monitors.xml"

if [ ! -f "$CONF" ]; then
    echo "Error: no existe $CONF (inicia sesion una vez en GNOME)." >&2
    exit 1
fi

# Rate real: parametro > modetest > tabla estatica
# (el refresh CVT no es 60.0 exacto; GNOME exige coincidencia)
# EN: Real rate: arg > modetest > static table (CVT refresh isn't exactly 60.0)
if [ -z "$RATE" ]; then
    # modetest: refresh = $12*1000/($7*$11) (clock/(htot*vtot))
    RATE="$(modetest -c 2>/dev/null | awk -v w="$W" -v h="$H" \
        '$1 ~ /^#/ && $2 == w "x" h { printf "%.6f", $12*1000/($7*$11); exit }')"
fi
if [ -z "$RATE" ]; then
    RATE="$(awk -v w="$W" -v h="$H" '$1==w && $2==h { print $3; exit }' \
        <<'TABLE'
1920 1080 60.00
1680 1050 60.00
1440 900 59.991
1400 1050 60.00
1366 768 59.969
1360 768 59.954
1280 1024 59.954
1280 960 59.999
1280 800 59.966
1280 768 59.954
1280 720 59.968
1280 600 59.966
1152 864 59.975
1024 768 59.954
800 600 59.960
640 480 59.941
TABLE
)"
fi
if [ -z "$RATE" ]; then
    echo "Error: no pude determinar el rate para ${W}x${H}." >&2
    exit 1
fi

echo "==> Tasa de refresco real para ${W}x${H}: $RATE Hz"

# Conector DRM del monitor USB
# EN: USB monitor DRM connector
USB_CARD=""
for c in /sys/class/drm/card*-*; do
    [ -e "$c/status" ] || continue
    [ "$(cat "$c/status" 2>/dev/null)" = "connected" ] || continue
    dev="$(readlink -f "$c/device" 2>/dev/null)"
    case "$dev" in
        *msdisp_plat*|*usbevdi*) USB_CARD="$(basename "$c")"; break ;;
    esac
done

if [ -z "$USB_CARD" ]; then
    echo "No se encontro el monitor USB conectado." >&2
    exit 1
fi

# card0-HDMI-A-2 -> HDMI-2 (nombre que usa GNOME)
# EN: card0-HDMI-A-2 -> HDMI-2 (GNOME connector name)
case "$USB_CARD" in
    card*-HDMI-A-*) CONN="HDMI-${USB_CARD##*-HDMI-A-}" ;;
    card*-eDP-*)    CONN="eDP-${USB_CARD##*-eDP-}" ;;
    card*-DP-*)     CONN="DP-${USB_CARD##*-DP-}" ;;
    card*-VGA-*)    CONN="VGA-${USB_CARD##*-VGA-}" ;;
    *)              CONN="${USB_CARD##*-}" ;;
esac

echo "==> Monitor USB: conector '$CONN'"
echo "==> Nueva resolucion: ${W}x${H} @ ${RATE} (aplicada en el proximo login/reboot)"

# Marca la resolucion como PREFERRED del driver (pref_w/pref_h): GNOME la
# adopta sin modeset en caliente -> evita el use-after-free (pantalla gris)
# EN: Set it as the driver PREFERRED mode (pref_w/pref_h) so GNOME adopts it
# without a hot modeset -> avoids the use-after-free grey screen
PREF_CONF="/etc/modprobe.d/msdisp-pref.conf"
if ! printf 'options usbdisp_drm pref_w=%s pref_h=%s\n' "$W" "$H" > "$PREF_CONF"; then
    echo "Error: no pude escribir $PREF_CONF." >&2
    exit 1
fi
echo "==> Modo preferido del driver: $PREF_CONF"

# Backup del archivo antes de editarlo
# EN: Back up the file before editing
cp "$CONF" "$CONF.bak-res"
echo "==> Backup: $CONF.bak-res"

# Editar el XML con python: cambia resolucion y recoloca las posiciones x por
# fila (mutter rechaza el archivo -> "Logical monitors not adjacent")
# EN: Edit the XML with python: change the resolution and re-lay x positions by
# row (mutter rejects the file otherwise -> "Logical monitors not adjacent")
python3 - "$CONF" "$CONN" "$W" "$H" "$RATE" <<'PY'
import re, sys

conf, conn, w, h, rate = sys.argv[1:6]
w, h = int(w), int(h)

data = open(conf).read()

def lm_conn(blk):
    m = re.search(r"<connector>([^<]+)</connector>", blk)
    return m.group(1) if m else ""

def lm_x(blk):
    m = re.search(r"<x>(\d+)</x>", blk)
    return int(m.group(1)) if m else 0

def lm_y(blk):
    m = re.search(r"<y>(\d+)</y>", blk)
    return int(m.group(1)) if m else 0

def lm_width(blk):
    m = re.search(r"<width>(\d+)</width>", blk)
    return int(m.group(1)) if m else 0

found = [False]
def set_mode(m):
    blk = m.group(0)
    if lm_conn(blk) == conn:
        blk = re.sub(r"(<width>)\d+(</width>)", r"\g<1>%d\g<2>" % w, blk, 1)
        blk = re.sub(r"(<height>)\d+(</height>)", r"\g<1>%d\g<2>" % h, blk, 1)
        blk = re.sub(r"(<rate>)[0-9.]+(</rate>)", r"\g<1>%s\g<2>" % rate, blk, 1)
        found[0] = True
    return blk

data = re.sub(r"<logicalmonitor>.*?</logicalmonitor>", set_mode, data, flags=re.S)

if not found[0]:
    sys.stderr.write("Error: no se encontro el monitor %s en monitors.xml\n" % conn)
    sys.exit(1)

# Recoloca posiciones: por fila (mismo y), de izquierda a derecha
# EN: Re-lay positions: by row (same y), left to right
blocks = re.findall(r"<logicalmonitor>.*?</logicalmonitor>", data, flags=re.S)
rows = {}
for blk in blocks:
    rows.setdefault(lm_y(blk), []).append((lm_x(blk), blk))

newpos = {}
for y, lst in rows.items():
    x = 0
    for _, blk in sorted(lst):
        newpos[id(blk)] = x
        x += lm_width(blk)

def set_pos(m):
    blk = m.group(0)
    if id(blk) in newpos:
        blk = re.sub(r"(<x>)\d+(</x>)", r"\g<1>%d\g<2>" % newpos[id(blk)], blk, 1)
    return blk

data = re.sub(r"<logicalmonitor>.*?</logicalmonitor>", set_pos, data, flags=re.S)

open(conf, "w").write(data)
print("OK: monitors.xml actualizado (%s -> %dx%d @ %s)" % (conn, w, h, rate))
PY

echo "==> Reiniciando para aplicar..."
sync
reboot
