/**
 * @file llmcptoolsworld.cpp
 * @brief World/object MCP tools (list_nearby_objects)
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

#include "fsareasearch.h"
#include "llagent.h"
#include "lleventtimer.h"
#include "llfloaterreg.h"
#include "llsdutil.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"

namespace
{
    using ToolResultCallback = LLMCPToolRegistry::ToolResultCallback;

    const F32 MCP_AREA_SEARCH_SETTLE_TIME = 3.f;

    LLSD collectAreaSearchResults()
    {
        LLSD objects = LLSD::emptyArray();

        FSAreaSearch* area_search = LLFloaterReg::getTypedInstance<FSAreaSearch>("area_search");
        if (area_search)
        {
            LLVector3d agent_pos = gAgent.getPositionGlobal();
            for (const auto& entry : area_search->mObjectDetails)
            {
                LLViewerObject* objectp = gObjectList.findObject(entry.first);
                if (!objectp)
                {
                    continue;
                }

                const FSObjectProperties& props = entry.second;
                LLSD obj;
                obj["id"] = entry.first.asString();
                obj["name"] = props.name;
                obj["owner_id"] = props.owner_id.asString();
                obj["group_owned"] = props.group_owned;
                obj["distance"] = (F32)(objectp->getPositionGlobal() - agent_pos).magVec();
                objects.append(obj);
            }
        }

        LLSD result;
        result["objects"] = objects;
        if (!area_search)
        {
            result["error"] = "Area Search floater unavailable.";
        }
        return result;
    }

    // FSAreaSearch (the Area Search floater's engine) only scans/resolves
    // object names while its own mActive flag is set, which normally only
    // happens when the user opens the floater and hits "Search". We flip
    // that on ourselves via the public refreshList(), then wait a few
    // seconds for its idle-driven scan + per-object name requests
    // (LLEventTimer-based, same pattern as get_avatar_profile) before
    // reading back whatever it has resolved so far. Repeated calls will
    // pick up names that were still pending on the first call.
    void toolListNearbyObjects(const LLSD& arguments, const ToolResultCallback& callback)
    {
        (void)arguments;

        FSAreaSearch* area_search = LLFloaterReg::getTypedInstance<FSAreaSearch>("area_search");
        if (area_search)
        {
            area_search->refreshList(false);
        }

        LLEventTimer::run_after(MCP_AREA_SEARCH_SETTLE_TIME, [callback]()
        {
            callback(collectAreaSearchResults());
        });
    }
}

void registerMCPWorldTools()
{
    LLMCPToolRegistry& registry = LLMCPToolRegistry::instance();

    LLSD empty_schema;
    empty_schema["type"] = "object";
    empty_schema["properties"] = LLSD::emptyMap();

    registry.registerTool(
        "list_nearby_objects",
        "Lists objects in view (id, name, owner, distance), via the Area Search engine. "
        "Asynchronous: triggers/refreshes a scan and waits a few seconds for names to resolve; "
        "call again if some names are still missing on the first call.",
        empty_schema,
        toolListNearbyObjects);
}
