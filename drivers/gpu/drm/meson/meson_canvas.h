/* 
 * Copyright (C) 2016 BayLibre, SAS
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

/* Canvas LUT Memory */

#ifndef __MESON_CANVAS_H
#define __MESON_CANVAS_H

#define MESON_CANVAS_ID_OSD1	0x4e

void meson_canvas_update_osd1_buffer(struct meson_drm *priv, 
				     struct drm_plane *plane);

void meson_canvas_init(struct meson_drm *priv);

#endif /* __MESON_VIU_H */
