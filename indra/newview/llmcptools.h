/**
 * @file llmcptools.h
 * @brief Read-only core MCP tools (position, region info, nearby agents)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLMCPTOOLS_H
#define LL_LLMCPTOOLS_H

// Registers the Phase 2-4 tools (get_position, get_region_info,
// get_nearby_agents, avatar_sit/stand/walk_to/teleport/fly, chat_say/shout)
// with LLMCPToolRegistry. Safe/cheap to call more than once (registerTool
// overwrites). Call once during viewer startup.
void registerMCPCoreTools();

// Registers the Phase 6 social tools (set_avatar_notes, list_friends,
// get_chat_history, get_avatar_profile) with LLMCPToolRegistry. See
// llmcptoolssocial.cpp. Call once during viewer startup.
void registerMCPSocialTools();

// Registers the Phase 7 world tool (list_nearby_objects) with
// LLMCPToolRegistry. See llmcptoolsworld.cpp. Call once during viewer
// startup.
void registerMCPWorldTools();

// Registers the Phase 8 environment tools (get_environment,
// set_environment) with LLMCPToolRegistry. See llmcptoolsenvironment.cpp.
// Call once during viewer startup.
void registerMCPEnvironmentTools();

// Registers the Phase 5 inventory tools (inventory_list, notecard_write)
// with LLMCPToolRegistry. See llmcptoolsinventory.cpp. Call once during
// viewer startup.
void registerMCPInventoryTools();

#endif // LL_LLMCPTOOLS_H
