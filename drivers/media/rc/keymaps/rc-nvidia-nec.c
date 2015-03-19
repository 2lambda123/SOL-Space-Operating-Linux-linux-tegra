/* Keytable for NVIDIA Remote Controller
 *
 * Copyright (c) 2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <media/rc-map.h>
#include <linux/module.h>

static struct rc_map_table foster_table[] = {
	{ 0x310c, KEY_0 },
	{ 0x3103, KEY_1 },
	{ 0x3104, KEY_2 },
	{ 0x3105, KEY_3 },
	{ 0x3106, KEY_4 },
	{ 0x3107, KEY_5 },
	{ 0x3108, KEY_6 },
	{ 0x3109, KEY_7 },
	{ 0x310a, KEY_8 },
	{ 0x310b, KEY_9 },
	{ 0x311f, KEY_VOLUMEUP },
	{ 0x3140, KEY_VOLUMEDOWN },
	{ 0x3149, KEY_CHANNELUP },
	{ 0x314a, KEY_CHANNELDOWN },
	{ 0x3111, KEY_UP },
	{ 0x3115, KEY_DOWN },
	{ 0x3112, KEY_LEFT },
	{ 0x3114, KEY_RIGHT },
	{ 0x310e, KEY_HOMEPAGE },
	{ 0x3100, KEY_POWER },
	{ 0x3113, KEY_ENTER },
	{ 0x3141, KEY_BACK },
};

static struct rc_map_list nvidia_map = {
	.map = {
			.scan = foster_table,
			.size = ARRAY_SIZE(foster_table),
			.rc_type = RC_BIT_NEC,
			.name = RC_MAP_NVIDIA_NEC,
	}
};

static int __init init_rc_map_nvidia(void)
{
	return rc_map_register(&nvidia_map);
}

static void __exit exit_rc_map_nvidia(void)
{
	rc_map_unregister(&nvidia_map);
}

module_init(init_rc_map_nvidia);
module_exit(exit_rc_map_nvidia);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel Fu <danifu@nvidia.com>");
