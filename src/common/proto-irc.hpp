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



#ifndef HEXCHAT_PROTO_HPP
#define HEXCHAT_PROTO_HPP

#include <ctime>
#include "hexchat.hpp"

/* Message tag information that might be passed along with a server message
 *
 * See http://ircv3.atheme.org/specification/capability-negotiation-3.1
 */
struct message_tags_data
{
	time_t timestamp;
};

#endif
