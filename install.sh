#!/bin/bash
# Instalador final de msdisp-drm parcheado para pantallas USB MacroSilicon
# MS9120/MS912C (IDs 534d:6021) con puerto VGA.
#
# Instala y configura:
#   1) Driver msdisp-drm con modos VESA 60Hz expuestos (pref_w/pref_h).
#   2) Transporte por bloques YUV422 (block_transfer=1) = ~30 Hz estables.
#   3) Puntero visible via MUTTER_DEBUG_DISABLE_HW_CURSORS=1 (cursor por
#      software de mutter, necesario por el bug multi-GPU: el monitor USB
#      no recibia el puntero en Wayland).
#
# Uso:   sudo bash install.sh
# Testeado en: CachyOS (Arch, GNOME/Wayland) con DKMS.
set -e

SRC="msdisp-drm-3.0.3.12"
NAME="msdisp-drm"
VER="3.0.3.12"

if [ "$(id -u)" != "0" ]; then
    echo "Este instalador requiere privilegios de root. Ejecuta: sudo bash install.sh"
    exit 1
fi

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
# Deshabilitar cualquier bloqueo previo de msdisp
sed -i '/^install usbdisp_usb/d; /^install usbdisp_drm/d' /etc/modprobe.d/blacklist-msdisp.conf 2>/dev/null || true
rm -f /etc/modprobe.d/blacklist-msdisp.conf
# Bloquear el driver alternativo jcrobles (ms912x) para que msdisp tome el adaptador
printf '%s\n' \
    "# Configuracion msdisp-drm (parche VGA) - bloqueo de jcrobles" \
    "blacklist ms912x" > /etc/modprobe.d/blacklist-ms912x.conf
# Modo 30 Hz: transporte por bloques YUV422 (dirty-rects) del driver de Windows
printf '%s\n' \
    "# msdisp-drm: transporte por bloques YUV422 (30 Hz estables)." \
    "options usbdisp_usb block_transfer=1" > /etc/modprobe.d/msdisp-block.conf

echo "==> [5/6] Configurando puntero visible (mutter software cursor)..."
# Bug multi-GPU: Wayland/mutter no pintaba el puntero en el monitor USB.
# Forzar el cursor por software de mutter lo hace aparecer (persistente).
if ! grep -q "MUTTER_DEBUG_DISABLE_HW_CURSORS" /etc/environment 2>/dev/null; then
    printf '\n# Puntero visible en monitor USB (mutter software cursor)\nMUTTER_DEBUG_DISABLE_HW_CURSORS=1\n' >> /etc/environment
fi
grep "MUTTER_DEBUG_DISABLE_HW_CURSORS" /etc/environment || true

echo "==> [6/7] Regla udev (opcional): consola de texto en el monitor USB..."
# Mapea VT1..6 al framebuffer del panel USB cuando aparece (y tras un replug).
# Solo se instala si existe con2fbmap (paquete fbset); es un extra no esencial:
# la imagen, 30 Hz, puntero y resolucion funcionan sin esta regla.
if command -v con2fbmap >/dev/null 2>&1; then
    install -m 644 "$SRC/99-msdisp-fbcon.rules" /etc/udev/rules.d/99-msdisp-fbcon.rules
    echo "    Instalada en /etc/udev/rules.d/ (con2fbmap disponible)."
else
    echo "    Omitida (no hay con2fbmap; instala 'fbset' si quieres consola en el panel)."
fi

echo "==> [7/7] Recargando modulos..."
rmmod ms912x 2>/dev/null || true
rmmod usbdisp_usb 2>/dev/null || true
modprobe usbdisp_drm 2>/dev/null || true
modprobe usbdisp_usb 2>/dev/null || true
depmod -a || true

echo ""
echo "Instalacion completada."
echo "El driver arrancara por defecto a 800x600 (seguro para cualquier monitor)."

# --- elige tu resolucion AHORA, antes del primer reinicio ---
# Asi el primer arranque ya sale con tu resolucion nativa y nada de pantalla
# negra. El MENU detecta dinamicamente los modos + Hz reales del driver.
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