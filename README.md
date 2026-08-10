# USBDisplay-Patch-ArchLinux-CachyOS-Gnome

Idioma: **Español** | [English](README.en.md)

---

*Driver oficial msdisp-drm 3.0.3.12 con parche personalizado para dar soporte
completo a pantallas USB MacroSilicon conectadas por interfaz VGA (chipsets
MS9120 / MS912C, ID `534d:6021`).*

**Estado:** FINAL y PROBADO. **Resultado:** **30 Hz estables** con puntero visible y
fluido, sin intercalado, sin lag y sin bajo rendimiento.

---

## Resumen

| Aspecto | Resultado |
| :--- | :--- |
| Resolución | Configurable con el MENU; por defecto 800×600 (seguro para cualquier monitor) |
| Refresco efectivo | 30 Hz (transporte por bloques YUV422) |
| Puntero | Visible y fluido (software cursor de mutter) |
| Imagen | Estable, sin intercalado ni artefactos |

---

## Por qué la imagen se intercalaba o daba errores

La causa raíz del intercalado no era el servidor gráfico (Wayland vs X11), sino
el ancho de banda del bus **USB 2.0**.

### 1. El bus USB 2.0 no alcanza para 60 Hz a 1440×900

El driver alternativo **ms912x (jcrobles)** intentaba entregar cada frame a la
tasa *nominal* de 60 Hz. Pero un frame completo de 1440×900 en RGB888 son
3.9 MB; a la velocidad real del USB 2.0 (40 MB/s) eso da 19 Hz como máximo.
El resultado era que cada frame llegaba tarde: **el panel ya estaba escaneando
cuando llegaba el siguiente** → aparecía intercalado (tearing) e inestabilidad.

El servidor gráfico no tiene nada que ver: tanto Wayland como X11 componen la
imagen y se la entregan al driver de kernel, que la empuja por el mismo cable
USB hacia el mismo chip. Cambiar de servidor gráfico no altera el transporte.

### 2. Solución de transporte: bloques YUV422 (Driver Windows)

El driver de Windows no envía el frame completo, sino **solo la región que
cambió** (dirty-rects) en formato **YUV422 (2 bytes/px)**, con su cabecera de
bloque. Eso reduce drásticamente los bytes a transmitir y permite subir de
19 Hz a **30 Hz reales**.

En este parche ese transporte se activa con `block_transfer=1` (módulo
`usbdisp_usb`). Cada frame se divide en bloques de píxeles y solo viajan los
que cambiaron desde el anterior, lo que deja la imagen estable y sin
intercalado, porque cada frame llega completo a tiempo.

> En su momento este transporte lo descarté por "imagen rota", pero el fallo
> era una combinación con la falta de modo de video (parche VGA) y no el
> transporte en sí. Con el parche VGA aplicado + `block_transfer=1` la imagen
> es estable.

---

## Cómo se logró el puntero funcional

El chip **no tiene overlay de cursor por hardware**; el cursor debe componerse
por software en el framebuffer. El problema apareció en GNOME/Wayland con
sistemas multi-GPU (iGPU Intel + GPU USB): mutter silenciaba el puntero en el
monitor USB porque asumía que el cursor hardware de la GPU principal cubría
todos los monitores.

### La solución: forzar el software cursor de mutter

Se añade a `/etc/environment`:

```bash
MUTTER_DEBUG_DISABLE_HW_CURSORS=1
```

Eso hace que **mutter pinte el cursor por software en cada framebuffer**, y
como ese framebuffer es exactamente el que viaja por USB al adaptador, el
puntero aparece de forma correcta y fluida.

> Es un ajuste **del compositor (mutter)**, no del driver de kernel. Por eso
> es compatible con este driver de 30 Hz: se combinan ambos — driver estable
> a 30 Hz + puntero pintado por mutter.

---

## Selección de resolución: segura, sin crasheos

Cambiar la resolución **en caliente** desde *Configuración > Pantallas* de GNOME
crashea el kernel en este monitor USB: el driver hace un modeset mientras el
hilo KMS aún copia el framebuffer anterior (use-after-free → page fault →
pantalla gris).

La solución actúa en dos niveles y es lo que automatizan `MENU` y
`cambiar-resolucion.sh`:

1. **Modo preferido del driver (`pref_w`/`pref_h`).** El driver construye su
   modo PREFERRED a partir de estos parámetros de módulo al arrancar (por
   defecto 800×600). GNOME adopta ese modo al iniciar sesión **sin ningún
   modeset en caliente**, así el use-after-free nunca ocurre. El valor se
   persiste en `/etc/modprobe.d/msdisp-pref.conf`:

   ```
   options usbdisp_drm pref_w=1280 pref_h=1024
   ```

2. **Tasa de refresco real.** Los modos CVT que genera el driver tienen tasas
   no enteras (59.96 o 74.98, no 60). GNOME descarta `monitors.xml` si la tasa
   guardada no coincide exactamente con un modo. Por eso
   `cambiar-resolucion.sh` lee la tasa real de `modetest` (refresh =
   clock×1000/(htot×vtot)) con una tabla estática como respaldo, y escribe la
   tasa exacta en `monitors.xml`.

---

## Configuración que queda aplicada (persistente)

| Archivo | Contenido | Efecto |
| :--- | :--- | :--- |
| `/etc/modprobe.d/blacklist-ms912x.conf` | `blacklist ms912x` | Impide que jcrobles tome el adaptador |
| `/etc/modprobe.d/msdisp-block.conf` | `options usbdisp_usb block_transfer=1` | Modo 30 Hz (bloques YUV422) |
| `/etc/modprobe.d/msdisp-pref.conf` | `options usbdisp_drm pref_w=... pref_h=...` | Resolución de arranque del monitor USB |
| `/etc/environment` | `MUTTER_DEBUG_DISABLE_HW_CURSORS=1` | Puntero visible por software |

### Reinicio sin cuelgue (opcional pero recomendado)

Con varias GPUs (UHD + NVIDIA + adaptador USB), plymouth puede colgarse
dibujando la pantalla de reinicio sobre el adaptador que se está desconectando.
El sistema igual reinicia tras el timeout de systemd, pero parece congelado.
Para reiniciar directo sin la animación:

```bash
sudo systemctl mask plymouth-reboot.service plymouth-quit-wait.service
```

Verificación del estado en caliente:

```bash
# Modo de transporte (Y = bloques 30Hz, N = frame completo)
cat /sys/module/usbdisp_usb/parameters/block_transfer

# Parámetros del modo preferido del módulo DRM
cat /sys/module/usbdisp_drm/parameters/pref_w
cat /sys/module/usbdisp_drm/parameters/pref_h

# Estado del pipeline activo (pipeline0) y frames enviados
cat /sys/devices/usbevdi/msdisp_plat.0/pipeline0/info
cat /sys/devices/usbevdi/msdisp_plat.0/pipeline0/frame
```

---

## Instalación

### Requisitos previos

- Arch Linux / CachyOS - Gnome.
- Dependencias: `sudo pacman -S dkms base-devel linux-headers`

### Instalación desde el terminal

```bash
git clone https://github.com/LelxDS3/USBDisplay-Patch-ArchLinux-CachyOS_Gnome.git
cd USBDisplay-Patch-ArchLinux-CachyOS_Gnome
sudo bash install.sh
```

`install.sh` copia la fuente a `/usr/src`, compila e instala vía DKMS, escribe
los archivos de configuración de la tabla anterior y recarga los módulos.
Luego (paso 7/7) ofrece lanzar `MENU` para que elijas la resolución del monitor
USB **antes del primer reinicio** — el primer arranque ya sale con tu resolución
y sin pantalla negra. El reboot es necesario porque los módulos no se descargan
en caliente.

---

## Cambiar resolución de forma segura (desde terminal, sin crashear)

```bash
# Menú interactivo: muestra los modos que el driver expone con su Hz real
# (detectados vía modetest) y reinicia con la selección aplicada
sudo ./MENU

# Generador genérico (lo usa el menú): fija resolución y reinicia
sudo ./cambiar-resolucion.sh 1280 1024         # tasa real autodetectada
sudo ./cambiar-resolucion.sh 800 600 59.96     # o fuerza la tasa explícitamente
```

`MENU` lista dinámicamente los modos del conector (de 640×480 hasta 1920×1080)
con su tasa real, así sirve para cualquier monitor 
(60 Hz, 75 Hz, etc.) sin errores de rate. Si `modetest` no
está disponible, usa una tabla estática como respaldo.

Cada cambio:
1. Escribe `/etc/modprobe.d/msdisp-pref.conf` para que el driver arranque con
   esa resolución como PREFERRED (sin modeset en caliente → sin pantalla gris).
2. Hace backup de `monitors.xml` (`.bak-res`) y reescribe la entrada del monitor
   USB con la tasa real exacta.
3. Reacomoda las posiciones de los monitores por fila para que mutter no rechace
   el archivo (`Logical monitors not adjacent`).
4. Reinicia.

Menor resolución = menos MB por frame en el bus USB 2.0 = mayor tasa real de
refresco (el driver siempre emite la mayor tasa que el bus permite).

---

## Revertir / Desinstalar

```bash
sudo rm -f /etc/modprobe.d/blacklist-ms912x.conf
sudo rm -f /etc/modprobe.d/msdisp-block.conf
sudo rm -f /etc/modprobe.d/msdisp-pref.conf
sudo dkms remove -m msdisp-drm -v 3.0.3.12 --all
sudo sed -i '/MUTTER_DEBUG_DISABLE_HW_CURSORS/d' /etc/environment
sudo systemctl unmask plymouth-reboot.service plymouth-quit-wait.service 2>/dev/null
sudo reboot
```

---

## Estructura del repositorio

| Archivo / Directorio | Descripción |
| :--- | :--- |
| `msdisp-drm-3.0.3.12/` | Código fuente del driver de MacroSilicon con el parche VGA y los parámetros `pref_w`/`pref_h`. |
| `install.sh` | Instalador DKMS + configuración final (30Hz + puntero + paso de resolución). |
| `MENU` | Menú interactivo para cambiar la resolución (y reinicia). |
| `cambiar-resolucion.sh` | Motor: fija resolución, modo preferido y reinicia. |
| `README.md` | Documentación técnica principal (Español). |
| `README.en.md` | Documentación técnica traducida (Inglés). |

---

## Créditos y Referencias

- **Parche VGA:** Desarrollado por LelxDS3.
- **Driver base:** MacroSilicon msdisp-drm versión 3.0.3.12 (GPLv2, © MacroSilicon Technology).
- **Driver Linux de referencia:** [MindShow/USBDisplay](https://github.com/MindShow/USBDisplay)
- **Puntero por software (mutter):** `MUTTER_DEBUG_DISABLE_HW_CURSORS` (mutter MR 2150).
