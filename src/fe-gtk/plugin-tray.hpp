/* HexChat
 * Copyright (C) 1998-2010 Peter Zelezny.
 * Copyright (C) 2009-2013 Berke Viktor.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef HEXCHAT_PLUGIN_TRAY_HPP
#define HEXCHAT_PLUGIN_TRAY_HPP

#include "../common/plugin.hpp"

int tray_plugin_init (hexchat_plugin *, char **, char **, char **, char *);
int tray_plugin_deinit (hexchat_plugin *);
bool tray_toggle_visibility (bool force_hide);
void tray_apply_setup (void);

#endif
