#!/bin/bash
# cambiar-resolucion.sh <ANCHO> <ALTO> [rate]
#
# Generador comun: cambia la resolucion del monitor USB MacroSilicon en el
# arranque editando ~/.config/monitors.xml (el mapa que GNOME lee al iniciar
# sesion) y reinicia.
#
# Por que esto y no "Configuracion > Pantallas"?
#   Cambiar la resolucion EN CALIENTE desde GNOME provoca un modeset mientras
#   el hilo KMS esta copiando el framebuffer anterior -> el driver msdisp
#   crashea con un page fault (use-after-free del fb). Fijar la resolucion en
#   monitors.xml hace que se aplique al iniciar sesion, sin cambio en caliente
#   y sin tocar el driver: el propio driver emite el refresh que el bus USB 2.0
#   permite (a menos pixeles, mas Hz reales).
#
# Uso:   sudo ./cambiar-resolucion.sh 800 600
#        sudo ./cambiar-resolucion.sh 800 600 59.96
# (rate por defecto: se lee del driver con modetest - el refresh real del modo
#  CVT no es 60.0 exacto, y GNOME exige coincidencia; fallback a tabla estatica)

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

# --- usuario real (con sudo HOME apunta a /root) ---
RUNUSER="${SUDO_USER:-$USER}"
USER_HOME="$(getent passwd "$RUNUSER" | cut -d: -f6)"
CONF="$USER_HOME/.config/monitors.xml"

if [ ! -f "$CONF" ]; then
    echo "Error: no existe $CONF (inicia sesion una vez en GNOME)." >&2
    exit 1
fi

# --- resolver el rate real: el refresh del modo CVT no es 60.0 exacto ---
# GNOME guarda el rate real (ej 59.96, 59.991...) y al leerlo busca un modo
# con esa misma tasa; si no coincide descarta el monitors.xml.
# Fuentes (por orden): parametro explicito > modetest > tabla estatica.
if [ -z "$RATE" ]; then
    # modetest -c:  #13 800x600 59.96 800 832 912 1024 600 603 607 624 38313
    # $7=htot $11=vtot $12=clock(kHz) -> refresh = clock*1000/(htot*vtot)
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

# --- detectar el conector DRM del monitor USB (card del msdisp) ---
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

# convertir card0-HDMI-A-2 -> HDMI-2 (nombre que usa GNOME)
case "$USB_CARD" in
    card*-HDMI-A-*) CONN="HDMI-${USB_CARD##*-HDMI-A-}" ;;
    card*-eDP-*)    CONN="eDP-${USB_CARD##*-eDP-}" ;;
    card*-DP-*)     CONN="DP-${USB_CARD##*-DP-}" ;;
    card*-VGA-*)    CONN="VGA-${USB_CARD##*-VGA-}" ;;
    *)              CONN="${USB_CARD##*-}" ;;
esac

echo "==> Monitor USB: conector '$CONN'"
echo "==> Nueva resolucion: ${W}x${H} @ ${RATE} (aplicada en el proximo login/reboot)"

# --- marcar la resolucion como PREFERIDA en el driver ---
# El driver genera su modo PREFERRED desde pref_w/pref_h al arrancar. Asi GNOME
# adopta esta resolucion sin modeset en caliente (+ elimina el use-after-free
# de usb_hal_update_frame que dejaba la pantalla gris al cambiar de modo al
# iniciar sesion). Tambien se actualiza monitors.xml por coherencia.
PREF_CONF="/etc/modprobe.d/msdisp-pref.conf"
if ! printf 'options usbdisp_drm pref_w=%s pref_h=%s\n' "$W" "$H" > "$PREF_CONF"; then
    echo "Error: no pude escribir $PREF_CONF." >&2
    exit 1
fi
echo "==> Modo preferido del driver: $PREF_CONF"

# --- backup ---
cp "$CONF" "$CONF.bak-res"
echo "==> Backup: $CONF.bak-res"

# --- editar el XML con python (cambia en TODAS las configs el monitor USB) ---
ubicado="$(command -v python3 || echo /usr/bin/python3)"
"$ubicado" - "$CONF" "$CONN" "$W" "$H" "$RATE" <<'PY'
import re, sys, collections

path, conn, w, h, rate = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
data = open(path).read()
w, h = int(w), int(h)

def lm_x(blk):
    m = re.search(r"<x>(\d+)</x>", blk)
    return int(m.group(1)) if m else 0

def lm_y(blk):
    m = re.search(r"<y>(\d+)</y>", blk)
    return int(m.group(1)) if m else 0

def lm_width(blk):
    m = re.search(r"<width>(\d+)</width>", blk)
    return int(m.group(1)) if m else 0

def lm_conn(blk):
    m = re.search(r"<connector>([^<]+)</connector>", blk)
    return m.group(1) if m else ""

changed = 0
def repl_res(m):
    global changed
    blk = m.group(0)
    if lm_conn(blk) == conn:
        blk = re.sub(r"(<width>)\d+(</width>)", r"\g<1>%d\g<2>" % w, blk, count=1)
        blk = re.sub(r"(<height>)\d+(</height>)", r"\g<1>%d\g<2>" % h, blk, count=1)
        blk = re.sub(r"(<rate>)[0-9.]+(</rate>)", r"\g<1>%s\g<2>" % rate, blk, count=1)
        changed += 1
    return blk

# 1) cambiar resolucion del monitor USB en todos los logicalmonitor
data = re.sub(r"<logicalmonitor>.*?</logicalmonitor>", repl_res, data,
              count=0, flags=re.S)
if not changed:
    sys.exit(1)

# 2) reacomodar posiciones por fila para que queden adjacentes
#    (mutter rechaza el archivo -> "Logical monitors not adjacent")
lms = re.findall(r"<logicalmonitor>.*?</logicalmonitor>", data, flags=re.S)

rows = collections.defaultdict(list)
for blk in lms:
    rows[lm_y(blk)].append((lm_x(blk), blk))

newpos = {}
for y, row in rows.items():
    row.sort(key=lambda t: t[0])
    xcur = 0
    for x, blk in row:
        newpos[(y, x)] = xcur
        xcur += lm_width(blk)

def repl_pos(m):
    blk = m.group(0)
    key = (lm_y(blk), lm_x(blk))
    if key in newpos:
        nx = newpos[key]
        blk = re.sub(r"(<x>)\d+(</x>)", r"\g<1>%d\g<2>" % nx, blk, count=1)
    return blk

data = re.sub(r"<logicalmonitor>.*?</logicalmonitor>", repl_pos, data,
              count=0, flags=re.S)
open(path, "w").write(data)
sys.exit(0)
PY

echo "==> monitors.xml actualizado (logicalmonitors con '$CONN': 1)."
echo "==> Reiniciando..."
sync
reboot now