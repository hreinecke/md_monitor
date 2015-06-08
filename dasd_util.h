/*
 * dasd_util.h
 *
 * Copyright (C) 2015 Hannes Reinecke <hare@suse.de>
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

#ifndef _DASD_UTIL_H
#define _DASD_UITL_H

extern int dasd_set_attribute(struct device_monitor *dev, const char *attr,
			      int value);
extern int dasd_setup_aio(struct device_monitor *dev);
extern void dasd_cleanup_aio(struct device_monitor *dev);
extern enum device_io_status dasd_check_aio(struct device_monitor *dev,
					    int timeout);

#endif /* _DASD_UTIL_H */

