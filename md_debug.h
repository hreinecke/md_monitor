/*
 * md_debug.h
 *
 * Copyright (C) 2013 Hannes Reinecke <hare@suse.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _MD_DEBUG_H
#define _MD_DEBUG_H

#define dbg(fmt, args...) log_fn(LOG_DEBUG, fmt "\n", ##args)
#define info(fmt, args...) log_fn(LOG_INFO, fmt "\n", ##args)
#define warn(fmt, args...) log_fn(LOG_WARNING, fmt "\n", ##args)
#define err(fmt, args...) log_fn(LOG_ERR, fmt "\n", ##args)

extern void log_fn(int priority, const char *format, ...);

#endif /* _MD_DEBUG_H */
