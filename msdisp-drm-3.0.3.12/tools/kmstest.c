/* Minimal raw-ioctl DRM/KMS test: open a card by path, probe the connected
 * connector's modes, then modeset a dumb buffer filled with vertical color
 * bars (red, green, blue, white) and hold it. No libdrm dependency.
 *
 * Usage: kmstest /dev/dri/cardN
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);	/* unbuffered so output survives signals */
	const char *path = (argc > 1) ? argv[1] : "/dev/dri/card0";
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 1; }

	/* --- resources --- */
	struct drm_mode_card_res res; memset(&res, 0, sizeof(res));
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("getres count"); return 1; }
	uint64_t conn_ids[32], crtc_ids[32], enc_ids[32], fb_ids[32];
	res.connector_id_ptr = (uintptr_t)conn_ids;
	res.crtc_id_ptr      = (uintptr_t)crtc_ids;
	res.encoder_id_ptr   = (uintptr_t)enc_ids;
	res.fb_id_ptr        = (uintptr_t)fb_ids;
	if (res.count_connectors > 32) res.count_connectors = 32;
	if (res.count_crtcs > 32) res.count_crtcs = 32;
	if (res.count_encoders > 32) res.count_encoders = 32;
	if (res.count_fbs > 32) res.count_fbs = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) { perror("getres"); return 1; }
	printf("%s: %u connectors, %u crtcs\n", path, res.count_connectors, res.count_crtcs);

	/* --- find a connected connector with modes --- */
	struct drm_mode_modeinfo modes[64];
	struct drm_mode_get_connector conn;
	uint32_t chosen_conn = 0, chosen_enc = 0; int mode_idx = -1, i, m;
	struct drm_mode_modeinfo chosen_mode;

	for (i = 0; i < (int)res.count_connectors; i++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conn_ids[i];
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn)) continue;   /* count pass (forces probe) */
		conn.modes_ptr = (uintptr_t)modes;
		conn.count_modes = (conn.count_modes > 64) ? 64 : conn.count_modes;
		conn.encoders_ptr = 0; conn.props_ptr = 0; conn.prop_values_ptr = 0;
		conn.count_props = 0; conn.count_encoders = 0;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn)) continue;
		printf("  connector %u: connection=%u (1=connected) modes=%u encoder=%u\n",
		       conn.connector_id, conn.connection, conn.count_modes, conn.encoder_id);
		for (m = 0; m < (int)conn.count_modes; m++)
			printf("      mode[%d]: %ux%u@%u \"%s\"\n", m, modes[m].hdisplay,
			       modes[m].vdisplay, modes[m].vrefresh, modes[m].name);
		if (conn.connection == 1 && conn.count_modes > 0 && chosen_conn == 0) {
			chosen_conn = conn.connector_id;
			chosen_enc = conn.encoder_id;
			/* prefer a 1024x600 mode, else mode 0 */
			mode_idx = 0;
			for (m = 0; m < (int)conn.count_modes; m++)
				if (modes[m].hdisplay == 1024 && modes[m].vdisplay == 600) { mode_idx = m; break; }
			chosen_mode = modes[mode_idx];
		}
	}
	if (!chosen_conn) { fprintf(stderr, "no connected connector with modes\n"); return 2; }
	printf("chosen connector %u, mode %ux%u \"%s\"\n", chosen_conn,
	       chosen_mode.hdisplay, chosen_mode.vdisplay, chosen_mode.name);

	/* --- crtc: from the connector's encoder --- */
	uint32_t crtc_id = 0;
	if (chosen_enc) {
		struct drm_mode_get_encoder enc; memset(&enc, 0, sizeof(enc));
		enc.encoder_id = chosen_enc;
		if (!ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) && enc.crtc_id)
			crtc_id = enc.crtc_id;
	}
	if (!crtc_id && res.count_crtcs) crtc_id = crtc_ids[0];
	printf("using crtc %u\n", crtc_id);

	/* --- dumb buffer (XRGB8888) --- */
	struct drm_mode_create_dumb cd; memset(&cd, 0, sizeof(cd));
	cd.width = chosen_mode.hdisplay; cd.height = chosen_mode.vdisplay; cd.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); return 1; }

	struct drm_mode_fb_cmd fb; memset(&fb, 0, sizeof(fb));
	fb.width = cd.width; fb.height = cd.height; fb.bpp = 32; fb.depth = 24;
	fb.pitch = cd.pitch; fb.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb)) { perror("addfb"); return 1; }

	struct drm_mode_map_dumb md; memset(&md, 0, sizeof(md));
	md.handle = cd.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("map_dumb"); return 1; }
	uint8_t *fbmem = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (fbmem == MAP_FAILED) { perror("mmap"); return 1; }

	/* vertical color bars: red | green | blue | white (XRGB8888) */
	uint32_t bars[4] = { 0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF };
	for (uint32_t y = 0; y < cd.height; y++) {
		uint32_t *row = (uint32_t *)(fbmem + (uint64_t)y * cd.pitch);
		for (uint32_t x = 0; x < cd.width; x++)
			row[x] = bars[(x * 4) / cd.width];
	}

	/* --- modeset --- */
	struct drm_mode_crtc set; memset(&set, 0, sizeof(set));
	uint32_t conns[1] = { chosen_conn };
	set.set_connectors_ptr = (uintptr_t)conns;
	set.count_connectors = 1;
	set.crtc_id = crtc_id;
	set.fb_id = fb.fb_id;
	set.mode = chosen_mode;
	set.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set)) { perror("setcrtc"); return 1; }
	printf("SETCRTC ok: color bars on %ux%u. Holding...\n", cd.width, cd.height);

	while (1) pause();
	return 0;
}
