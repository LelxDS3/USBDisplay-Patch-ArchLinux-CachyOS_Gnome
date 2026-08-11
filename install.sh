#!/bin/bash
# Instalador msdisp-drm parcheado para pantallas USB MacroSilicon
# MS9120/MS912C (ID 534d:6021, puerto VGA).
# Configura: modos VESA (pref_w/pref_h), 30 Hz (block_transfer=1),
# puntero visible (MUTTER_DEBUG_DISABLE_HW_CURSORS) y regla udev opcional.
# Uso: sudo bash install.sh | Testeado: CachyOS (Arch, GNOME/Wayland)
# EN: Patched msdisp-drm installer for MacroSilicon USB-VGA displays
# (MS9120/MS912C, ID 534d:6021). Sets VESA modes (pref_w/pref_h), 30 Hz
# (block_transfer=1), visible cursor and optional udev rule.
set -e

SRC="msdisp-drm-3.0.3.12"
NAME="msdisp-drm"
VER="3.0.3.12"

if [ "$(id -u)" != "0" ]; then
    echo "Este instalador requiere privilegios de root. Ejecuta: sudo bash install.sh"
    exit 1
fi

# Los ZIP de GitHub no preservan +x; se asegura que los scripts sean ejecutables
# EN: GitHub ZIPs strip +x; ensure the scripts are executable
chmod +x "$(dirname "$0")/MENU" 2>/dev/null || true
chmod +x "$(dirname "$0")/cambiar-resolucion.sh" 2>/dev/null || true

echo "==> [1/6] Comprobando requisitos..."
for cmd in dkms make gcc; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Falta la herramienta '$cmd'. Instalala primero (ej. '# pacman -S dkms base-devel')."
        exit 1
    fi
done
KV=$(uname -r)
echo "    Kernel actual: $KV"

echo "==> [2/6] Copiando fuente a /usr/src..."
if [ -d "/usr/src/${SRC}" ]; then
    echo "    Ya existe /usr/src/${SRC}; se conserva (usa --force para reemplazar)."
else
    cp -r "$SRC" "/usr/src/${SRC}"
fi

echo "==> [3/6] Registrando y compilando en DKMS..."
if ! dkms status -m "$NAME" -v "$VER" | grep -q installed; then
    dkms add -m "$NAME" -v "$VER" 2>/dev/null || true
    dkms install -m "$NAME" -v "$VER" -k "$KV" --force
else
    echo "    msdisp-drm ${VER} ya esta instalado en DKMS."
fi

echo "==> [4/6] Configurando modprobe (bloquear jcrobles, habilitar msdisp 30Hz)..."
# Limpia bloqueos previos y bloquea el driver alternativo ms912x (jcrobles)
# EN: Clear previous blocks and blacklist the alternative ms912x driver
sed -i '/^install usbdisp_usb/d; /^install usbdisp_drm/d' /etc/modprobe.d/blacklist-msdisp.conf 2>/dev/null || true
rm -f /etc/modprobe.d/blacklist-msdisp.conf
printf '%s\n' \
    "# Configuracion msdisp-drm (parche VGA) - bloqueo de jcrobles" \
    "blacklist ms912x" > /etc/modprobe.d/blacklist-ms912x.conf
# 30 Hz: transporte por bloques YUV422 (dirty-rects)
# EN: 30 Hz via YUV422 block transfer (dirty-rects)
printf '%s\n' \
    "# msdisp-drm: transporte por bloques YUV422 (30 Hz estables)." \
    "options usbdisp_usb block_transfer=1" > /etc/modprobe.d/msdisp-block.conf

echo "==> [5/6] Configurando puntero visible (mutter software cursor)..."
# Bug multi-GPU: fuerza el cursor por software de mutter (aparece en el monitor USB)
# EN: Multi-GPU bug: force mutter software cursor so it shows on the USB monitor
if ! grep -q "MUTTER_DEBUG_DISABLE_HW_CURSORS" /etc/environment 2>/dev/null; then
    printf '\n# Puntero visible en monitor USB (mutter software cursor)\nMUTTER_DEBUG_DISABLE_HW_CURSORS=1\n' >> /etc/environment
fi

echo "==> [6/6] Recargando modulos..."
rmmod ms912x 2>/dev/null || true
rmmod usbdisp_usb 2>/dev/null || true
modprobe usbdisp_drm 2>/dev/null || true
modprobe usbdisp_usb 2>/dev/null || true
depmod -a || true

echo ""
echo "Instalacion completada."
echo "El driver arrancara por defecto a 800x600 (seguro para cualquier monitor)."

# Elegir la resolucion antes del primer reinicio evita la pantalla negra
# EN: Pick the resolution before the first reboot to avoid a black screen
if command -v whiptail >/dev/null 2>&1; then
    echo ""
    echo "==> [7/7] Selecciona la resolucion de tu monitor USB:"
    echo "    (escribe 'no' si quieres dejar la 800x600 por defecto)"
    if whiptail --title "Configurar resolucion" \
        --yesno "Detectar y elegir AHORA la resolucion del monitor USB?\n\nSi tu monitor no alcanza la resolucion por defecto, eligela aqui y el primer arranque saldra correctamente." 10 70; then
        DIR="$(cd "$(dirname "$0")" && pwd)"
        "$DIR/MENU"
    else
        echo "Resolucion por defecto 800x600 mantenida."
    fi
else
    echo "whiptail no esta instalado; se mantiene la resolucion por defecto 800x600."
    echo "Tras reiniciar, ejecuta:  sudo ./MENU"
fi

echo ""
echo "Cuando reinicies quedara todo activo (30 Hz + puntero visible + resolucion elegida)."
