/**
 * @file llmcptools.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llmcptools.h"

#include "llmcptoolregistry.h"

#include "llagent.h"
#include "llagentdata.h"
#include "llavatarnamecache.h"
#include "llevents.h"
#include "llsdutil.h"
#include "lluuid.h"
#include "llviewerregion.h"
#include "llworld.h"

#include "fsnearbychatbarlistener.h"

#include <stdexcept>

namespace
{
    using ToolResultCallback = LLMCPToolRegistry::ToolResultCallback;

    LLSD vector3ToLLSD(const LLVector3& v)
    {
        LLSD result;
        result["x"] = v.mV[VX];
        result["y"] = v.mV[VY];
        result["z"] = v.mV[VZ];
        return result;
    }

    LLSD vector3dToLLSD(const LLVector3d& v)
    {
        LLSD result;
        result["x"] = v.mdV[VX];
        result["y"] = v.mdV[VY];
        result["z"] = v.mdV[VZ];
        return result;
    }

    void toolGetPosition(const LLSD& arguments, const ToolResultCallback& callback)
    {
        (void)arguments;

        LLSD result;
        result["region_position"] = vector3ToLLSD(gAgent.getPositionAgent());
        result["global_position"] = vector3dToLLSD(gAgent.getPositionGlobal());

        LLViewerRegion* region = gAgent.getRegion();
        result["region_name"] = region ? region->getName() : std::string();
        callback(result);
    }

    void toolGetRegionInfo(const LLSD& arguments, const ToolResultCallback& callback)
    {
        (void)arguments;

        LLViewerRegion* region = gAgent.getRegion();
        LLSD result;
        if (!region)
        {
            result["error"] = "No current region (not logged in)";
            callback(result);
            return;
        }

        result["name"] = region->getName();
        result["region_handle"] = llformat("%llu", (unsigned long long)region->getHandle());
        result["origin_global"] = vector3dToLLSD(region->getOriginGlobal());
        result["water_height"] = region->getWaterHeight();
        result["maturity"] = region->getSimAccessString();
        callback(result);
    }

    void toolGetNearbyAgents(const LLSD& arguments, const ToolResultCallback& callback)
    {
        F32 radius = arguments.has("radius") ? (F32)arguments["radius"].asReal() : 96.f;

        uuid_vec_t avatar_ids;
        std::vector<LLVector3d> positions;
        LLWorld::instance().getAvatars(&avatar_ids, &positions, gAgent.getPositionGlobal(), radius);

        LLSD agents = LLSD::emptyArray();
        for (size_t i = 0; i < avatar_ids.size(); ++i)
        {
            const LLUUID& agent_id = avatar_ids[i];
            if (agent_id == gAgentID)
            {
                continue; // skip ourselves
            }

            LLSD entry;
            entry["id"] = agent_id.asString();

            LLAvatarName av_name;
            if (LLAvatarNameCache::get(agent_id, &av_name))
            {
                entry["name"] = av_name.getUserName();
                entry["display_name"] = av_name.getDisplayName();
            }

            entry["position"] = vector3dToLLSD(positions[i]);
            entry["distance"] = (F32)(positions[i] - gAgent.getPositionGlobal()).magVec();
            agents.append(entry);
        }

        LLSD result;
        result["agents"] = agents;
        callback(result);
    }

    // Converts our {"x":,"y":,"z":} MCP convention into the [x,y,z] array
    // form that LLAgentListener's LLEventAPI handlers expect
    // (ll_vector3_from_sd / ll_vector3d_from_sd).
    LLSD xyzMapToArray(const LLSD& xyz)
    {
        LLSD arr = LLSD::emptyArray();
        arr.append(xyz["x"].asReal());
        arr.append(xyz["y"].asReal());
        arr.append(xyz["z"].asReal());
        return arr;
    }

    // requestSit/requestStand/requestTeleport post to the "LLAgent" event
    // pump (LLEventAPI, dispatch key "op") and never send a reply - they
    // just trigger the action synchronously. startAutoPilot is similar but
    // completes asynchronously (posts to the separate "LLAutopilot" pump
    // once the avatar arrives), so we don't wait for it here - the caller
    // is expected to poll get_position instead.

    void toolAvatarSit(const LLSD& arguments, const ToolResultCallback& callback)
    {
        LLSD event;
        event["op"] = "requestSit";
        if (arguments.has("obj_uuid"))
        {
            event["obj_uuid"] = LLUUID(arguments["obj_uuid"].asString());
        }
        else if (arguments.has("position"))
        {
            event["position"] = xyzMapToArray(arguments["position"]);
        }
        LLEventPumps::instance().obtain("LLAgent").post(event);

        LLSD result;
        result["status"] = "requested";
        callback(result);
    }

    void toolAvatarStand(const LLSD& arguments, const ToolResultCallback& callback)
    {
        (void)arguments;

        LLSD event;
        event["op"] = "requestStand";
        LLEventPumps::instance().obtain("LLAgent").post(event);

        LLSD result;
        result["status"] = "requested";
        callback(result);
    }

    void toolAvatarWalkTo(const LLSD& arguments, const ToolResultCallback& callback)
    {
        if (!arguments.has("target_global"))
        {
            throw std::runtime_error("Missing required argument 'target_global' ({x,y,z}, see get_position/get_nearby_agents).");
        }

        LLSD event;
        event["op"] = "startAutoPilot";
        event["target_global"] = xyzMapToArray(arguments["target_global"]);
        if (arguments.has("stop_distance"))
        {
            event["stop_distance"] = arguments["stop_distance"];
        }
        LLEventPumps::instance().obtain("LLAgent").post(event);

        LLSD result;
        result["status"] = "started";
        result["note"] = "Autopilot runs asynchronously; poll get_position to track progress.";
        callback(result);
    }

    void toolAvatarTeleport(const LLSD& arguments, const ToolResultCallback& callback)
    {
        if (!arguments.has("regionname"))
        {
            throw std::runtime_error("Missing required argument 'regionname'.");
        }

        LLSD event;
        event["op"] = "requestTeleport";
        event["regionname"] = arguments["regionname"];
        event["x"] = arguments.has("x") ? arguments["x"] : LLSD(0.0);
        event["y"] = arguments.has("y") ? arguments["y"] : LLSD(0.0);
        event["z"] = arguments.has("z") ? arguments["z"] : LLSD(0.0);
        LLEventPumps::instance().obtain("LLAgent").post(event);

        LLSD result;
        result["status"] = "requested";
        callback(result);
    }

    void toolAvatarFly(const LLSD& arguments, const ToolResultCallback& callback)
    {
        bool enabled = arguments.has("enabled") ? arguments["enabled"].asBoolean() : true;
        gAgent.setFlying(enabled);

        LLSD result;
        result["flying"] = gAgent.getFlying();
        callback(result);
    }

    // FSNearbyChatBarListener (pump "LLChatBar", op "sendChat") exists in
    // the tree but nothing ever instantiates it - unlike LLAgentListener,
    // which is a permanent member of LLAgent. Constructing one here
    // activates it: LLEventAPI's constructor self-registers with the named
    // pump. Never destroyed, same lifetime as the process, exactly like
    // gAgent.mListener.
    FSNearbyChatBarListener& nearbyChatBarListener()
    {
        static FSNearbyChatBarListener sListener;
        return sListener;
    }

    void toolChatSend(const LLSD& arguments, const std::string& chat_type, const ToolResultCallback& callback)
    {
        if (!arguments.has("message"))
        {
            throw std::runtime_error("Missing required argument 'message'.");
        }

        LLSD event;
        event["op"] = "sendChat";
        event["message"] = arguments["message"];
        event["type"] = chat_type;
        if (arguments.has("channel"))
        {
            event["channel"] = arguments["channel"];
        }
        LLEventPumps::instance().obtain("LLChatBar").post(event);

        LLSD result;
        result["status"] = "sent";
        callback(result);
    }

    void toolChatSay(const LLSD& arguments, const ToolResultCallback& callback)
    {
        toolChatSend(arguments, "normal", callback);
    }

    void toolChatShout(const LLSD& arguments, const ToolResultCallback& callback)
    {
        toolChatSend(arguments, "shout", callback);
    }
}

void registerMCPCoreTools()
{
    LLMCPToolRegistry& registry = LLMCPToolRegistry::instance();

    LLSD empty_schema;
    empty_schema["type"] = "object";
    empty_schema["properties"] = LLSD::emptyMap();

    registry.registerTool(
        "get_position",
        "Returns the agent's current region and global position, and current region name.",
        empty_schema,
        toolGetPosition);

    registry.registerTool(
        "get_region_info",
        "Returns information about the current region (name, handle, origin, water height, maturity rating).",
        empty_schema,
        toolGetRegionInfo);

    LLSD nearby_schema;
    nearby_schema["type"] = "object";
    LLSD radius_prop;
    radius_prop["type"] = "number";
    radius_prop["description"] = "Search radius in meters (default 96).";
    LLSD nearby_props;
    nearby_props["radius"] = radius_prop;
    nearby_schema["properties"] = nearby_props;

    registry.registerTool(
        "get_nearby_agents",
        "Returns nearby avatars (id, name, position, distance) within a radius of the agent (default 96m).",
        nearby_schema,
        toolGetNearbyAgents);

    LLSD xyz_prop;
    xyz_prop["type"] = "object";
    LLSD xyz_props;
    xyz_props["x"] = LLSDMap("type", "number");
    xyz_props["y"] = LLSDMap("type", "number");
    xyz_props["z"] = LLSDMap("type", "number");
    xyz_prop["properties"] = xyz_props;

    LLSD sit_schema;
    sit_schema["type"] = "object";
    LLSD sit_props;
    sit_props["obj_uuid"] = LLSDMap("type", "string")("description", "UUID of the object to sit on.");
    sit_props["position"] = xyz_prop;
    sit_schema["properties"] = sit_props;
    registry.registerTool(
        "avatar_sit",
        "Sit on an object (by obj_uuid or nearest to position), or on the ground if neither is given. Fire-and-forget, no reply.",
        sit_schema,
        toolAvatarSit);

    registry.registerTool(
        "avatar_stand",
        "Stand up. Fire-and-forget, no reply.",
        empty_schema,
        toolAvatarStand);

    LLSD walk_schema;
    walk_schema["type"] = "object";
    LLSD walk_props;
    walk_props["target_global"] = xyz_prop;
    walk_props["stop_distance"] = LLSDMap("type", "number")("description", "Max stop distance from target [default: autopilot guess].");
    walk_schema["properties"] = walk_props;
    walk_schema["required"] = LLSD::emptyArray();
    walk_schema["required"].append("target_global");
    registry.registerTool(
        "avatar_walk_to",
        "Starts walking (autopilot) to a global {x,y,z} position, e.g. from get_position/get_nearby_agents. "
        "Asynchronous: returns immediately with status=started, poll get_position to track progress.",
        walk_schema,
        toolAvatarWalkTo);

    LLSD teleport_schema;
    teleport_schema["type"] = "object";
    LLSD teleport_props;
    teleport_props["regionname"] = LLSDMap("type", "string");
    teleport_props["x"] = LLSDMap("type", "number");
    teleport_props["y"] = LLSDMap("type", "number");
    teleport_props["z"] = LLSDMap("type", "number");
    teleport_schema["properties"] = teleport_props;
    teleport_schema["required"] = LLSD::emptyArray();
    teleport_schema["required"].append("regionname");
    registry.registerTool(
        "avatar_teleport",
        "Teleport to a region by name at local coordinates x,y,z (default 0,0,0). Fire-and-forget, no reply.",
        teleport_schema,
        toolAvatarTeleport);

    LLSD fly_schema;
    fly_schema["type"] = "object";
    LLSD fly_props;
    fly_props["enabled"] = LLSDMap("type", "boolean")("description", "true to start flying, false to stop [default: true].");
    fly_schema["properties"] = fly_props;
    registry.registerTool(
        "avatar_fly",
        "Toggles flying on/off for the agent.",
        fly_schema,
        toolAvatarFly);

    nearbyChatBarListener(); // activate the "LLChatBar" pump (see comment above)

    LLSD chat_schema;
    chat_schema["type"] = "object";
    LLSD chat_props;
    chat_props["message"] = LLSDMap("type", "string");
    chat_props["channel"] = LLSDMap("type", "integer")("description", "Chat channel [default: 0 = public].");
    chat_schema["properties"] = chat_props;
    chat_schema["required"] = LLSD::emptyArray();
    chat_schema["required"].append("message");

    registry.registerTool(
        "chat_say",
        "Say a message in local/nearby chat at normal volume (channel 0 unless specified).",
        chat_schema,
        toolChatSay);

    registry.registerTool(
        "chat_shout",
        "Shout a message in local/nearby chat (audible further away than say).",
        chat_schema,
        toolChatShout);
}
