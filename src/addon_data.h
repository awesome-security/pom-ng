/*
 *  This file is part of pom-ng.
 *  Copyright (C) 2012 Guy Martin <gmsoft@tuxicoman.be>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifndef __ADDON_DATA_H__
#define __ADDON_DATA_H__

#include <pom-ng/data.h>

#define ADDON_DATA_METATABLE		"addon.data"
#define ADDON_DATA_ITEM_METATABLE	"addon.data_item"

struct addon_data {
	struct data *data;
	struct data_reg *reg;
};

void addon_data_lua_register(lua_State *L);
void addon_data_push(lua_State *L, struct data *data, struct data_reg *data_reg);

#endif
