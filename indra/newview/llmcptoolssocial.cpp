/**
 * @file llmcptoolssocial.cpp
 * @brief Social MCP tools: avatar profile/notes, chat history, friends list
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

#include "llavatarnamecache.h"
#include "llavatarpropertiesprocessor.h"
#include "llcallingcard.h"
#include "lleventtimer.h"
#include "lllogchat.h"
#include "llsdutil.h"
#include "lluuid.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <stdexcept>

namespace
{
    using ToolResultCallback = LLMCPToolRegistry::ToolResultCallback;

    LLUUID requireAvatarId(const LLSD& arguments)
    {
        if (!arguments.has("avatar_id"))
        {
            throw std::runtime_error("Missing required argument 'avatar_id'.");
        }
        LLUUID id(arguments["avatar_id"].asString());
        if (id.isNull())
        {
            throw std::runtime_error("Invalid 'avatar_id' (expected a UUID string).");
        }
        return id;
    }

    // --- set_avatar_notes (synchronous, fire-and-forget) ---------------

    void toolSetAvatarNotes(const LLSD& arguments, const ToolResultCallback& callback)
    {
        LLUUID avatar_id = requireAvatarId(arguments);
        if (!arguments.has("text"))
        {
            throw std::runtime_error("Missing required argument 'text'.");
        }

        LLAvatarPropertiesProcessor::instance().sendNotes(avatar_id, arguments["text"].asString());

        LLSD result;
        result["status"] = "sent";
        callback(result);
    }

    // --- list_friends (synchronous, local data only) -------------------

    void toolListFriends(const LLSD& arguments, const ToolResultCallback& callback)
    {
        (void)arguments;

        LLAvatarTracker::buddy_map_t buddies;
        LLAvatarTracker::instance().copyBuddyList(buddies);

        LLSD friends_list = LLSD::emptyArray();
        for (const auto& entry : buddies)
        {
            const LLUUID& id = entry.first;

            LLSD friend_entry;
            friend_entry["id"] = id.asString();
            friend_entry["online"] = LLAvatarTracker::instance().isBuddyOnline(id);

            LLAvatarName av_name;
            if (LLAvatarNameCache::get(id, &av_name))
            {
                friend_entry["name"] = av_name.getUserName();
                friend_entry["display_name"] = av_name.getDisplayName();
            }

            friends_list.append(friend_entry);
        }

        LLSD result;
        result["friends"] = friends_list;
        callback(result);
    }

    // --- get_chat_history (synchronous, reads a local transcript file) -

    // Mirrors LLLogChat::isTranscriptExist()'s internal account-name ->
    // log-file-base-name mapping, which isn't exposed as a standalone
    // function. loadChatHistory() applies its own makeLogFileName() to
    // whatever base name we pass it, so we must NOT pre-apply it ourselves.
    std::string avatarLogFileBaseName(const LLUUID& avatar_id)
    {
        LLAvatarName avatar_name;
        LLAvatarNameCache::get(avatar_id, &avatar_name);

        std::string base_name;
        if (gSavedSettings.getBOOL("UseLegacyIMLogNames"))
        {
            base_name = avatar_name.getUserName();
            base_name = base_name.substr(0, base_name.find(" Resident"));
        }
        else
        {
            base_name = avatar_name.getAccountName();
            std::replace(base_name.begin(), base_name.end(), '.', '_');
        }
        return base_name;
    }

    void toolGetChatHistory(const LLSD& arguments, const ToolResultCallback& callback)
    {
        LLUUID avatar_id = requireAvatarId(arguments);

        LLSD result;
        if (!LLLogChat::isTranscriptExist(avatar_id))
        {
            result["messages"] = LLSD::emptyArray();
            result["note"] = "No saved chat history found for this avatar.";
            callback(result);
            return;
        }

        std::list<LLSD> messages;
        LLLogChat::loadChatHistory(avatarLogFileBaseName(avatar_id), messages);

        S32 max_lines = arguments.has("max_lines") ? arguments["max_lines"].asInteger() : 200;
        if (max_lines <= 0)
        {
            max_lines = 200;
        }

        S32 skip = std::max((S32)0, (S32)messages.size() - max_lines);
        S32 idx = 0;
        LLSD out_messages = LLSD::emptyArray();
        for (const LLSD& msg : messages)
        {
            if (idx++ < skip)
            {
                continue;
            }
            out_messages.append(msg);
        }

        result["messages"] = out_messages;
        callback(result);
    }

    // --- get_avatar_profile (asynchronous: network round-trip) --------
    //
    // LLAvatarPropertiesProcessor::sendAvatarPropertiesRequest() replies
    // asynchronously via LLAvatarPropertiesObserver::processProperties().
    // This request object registers itself as the observer, then either
    // completes when the reply arrives or after a fixed timeout,
    // whichever comes first - never both. The already-scheduled timeout
    // timer is the sole owner of this object's lifetime (see finish()):
    // it is the only place `delete this` appears, so a success reply
    // arriving before the timeout can complete the callback immediately
    // without freeing the object out from under the still-pending timer.
    class MCPAvatarProfileRequest : public LLAvatarPropertiesObserver
    {
    public:
        MCPAvatarProfileRequest(const LLUUID& avatar_id, const ToolResultCallback& callback)
            : mAvatarId(avatar_id), mCallback(callback), mDone(false)
        {
            LLAvatarPropertiesProcessor::instance().addObserver(mAvatarId, this);
            LLAvatarPropertiesProcessor::instance().sendAvatarPropertiesRequest(mAvatarId);

            LLEventTimer::run_after(15.f, [this]()
            {
                if (!mDone)
                {
                    LLSD error_result;
                    error_result["error"] = "Timed out waiting for avatar properties.";
                    complete(error_result);
                }
                delete this;
            });
        }

        void processProperties(void* data, EAvatarProcessorType type) override
        {
            if (mDone || (type != APT_PROPERTIES && type != APT_PROPERTIES_LEGACY))
            {
                return;
            }

            const LLAvatarData* avatar_data = static_cast<const LLAvatarData*>(data);
            if (avatar_data->avatar_id != mAvatarId)
            {
                return;
            }

            complete(buildResult(*avatar_data));
        }

    private:
        static LLSD buildResult(const LLAvatarData& data)
        {
            LLSD result;
            result["avatar_id"] = data.avatar_id.asString();
            result["about_text"] = data.about_text;
            result["born_on"] = data.born_on.asString();
            result["partner_id"] = data.partner_id.notNull() ? data.partner_id.asString() : std::string();
            result["notes"] = data.notes;

            LLSD groups = LLSD::emptyArray();
            for (const auto& group : data.group_list)
            {
                LLSD group_entry;
                group_entry["id"] = group.group_id.asString();
                group_entry["name"] = group.group_name;
                groups.append(group_entry);
            }
            result["groups"] = groups;

            LLSD picks = LLSD::emptyArray();
            for (const auto& pick : data.picks_list)
            {
                LLSD pick_entry;
                pick_entry["id"] = pick.first.asString();
                pick_entry["name"] = pick.second;
                picks.append(pick_entry);
            }
            result["picks"] = picks;

            return result;
        }

        void complete(const LLSD& result)
        {
            if (mDone)
            {
                return;
            }
            mDone = true;
            LLAvatarPropertiesProcessor::instance().removeObserver(mAvatarId, this);
            mCallback(result);
        }

        LLUUID mAvatarId;
        ToolResultCallback mCallback;
        bool mDone;
    };

    void toolGetAvatarProfile(const LLSD& arguments, const ToolResultCallback& callback)
    {
        LLUUID avatar_id = requireAvatarId(arguments);
        new MCPAvatarProfileRequest(avatar_id, callback); // self-owning, see class comment
    }
}

void registerMCPSocialTools()
{
    LLMCPToolRegistry& registry = LLMCPToolRegistry::instance();

    LLSD avatar_id_prop = LLSDMap("type", "string")("description", "UUID of the avatar.");

    LLSD notes_schema;
    notes_schema["type"] = "object";
    LLSD notes_props;
    notes_props["avatar_id"] = avatar_id_prop;
    notes_props["text"] = LLSDMap("type", "string");
    notes_schema["properties"] = notes_props;
    notes_schema["required"] = LLSD::emptyArray();
    notes_schema["required"].append("avatar_id");
    notes_schema["required"].append("text");
    registry.registerTool(
        "set_avatar_notes",
        "Overwrites the private profile notes for an avatar (server-stored). Fire-and-forget, no reply.",
        notes_schema,
        toolSetAvatarNotes);

    LLSD empty_schema;
    empty_schema["type"] = "object";
    empty_schema["properties"] = LLSD::emptyMap();
    registry.registerTool(
        "list_friends",
        "Returns the friends list (id, name, display name, online status).",
        empty_schema,
        toolListFriends);

    LLSD history_schema;
    history_schema["type"] = "object";
    LLSD history_props;
    history_props["avatar_id"] = avatar_id_prop;
    history_props["max_lines"] = LLSDMap("type", "integer")("description", "Max number of most-recent messages to return [default: 200].");
    history_schema["properties"] = history_props;
    history_schema["required"] = LLSD::emptyArray();
    history_schema["required"].append("avatar_id");
    registry.registerTool(
        "get_chat_history",
        "Reads the locally saved 1:1 IM transcript with an avatar (most recent max_lines messages).",
        history_schema,
        toolGetChatHistory);

    LLSD profile_schema;
    profile_schema["type"] = "object";
    LLSD profile_props;
    profile_props["avatar_id"] = avatar_id_prop;
    profile_schema["properties"] = profile_props;
    profile_schema["required"] = LLSD::emptyArray();
    profile_schema["required"].append("avatar_id");
    registry.registerTool(
        "get_avatar_profile",
        "Returns an avatar's profile: about text, join date, partner, groups, picks and current notes. "
        "Asynchronous (network round-trip to the server, up to ~15s timeout).",
        profile_schema,
        toolGetAvatarProfile);
}
