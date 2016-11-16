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

/* Video Input Unit */

#ifndef __MESON_VIU_H
#define __MESON_VIU_H

/* Update OSD1 Coordinates */
void meson_viu_update_osd1(struct meson_drm *priv, struct drm_plane *plane);

/* Commit OSD1 changes */
void meson_viu_commit_osd1(struct meson_drm *priv);

/* Synchronize Field Change for OSD1 */
void meson_viu_sync_osd1(struct meson_drm *priv);

void meson_viu_init(struct meson_drm *priv);

#endif /* __MESON_VIU_H */
