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
#include "llimview.h"
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
            result["total_messages"] = 0;
            result["offset"] = 0;
            result["returned"] = 0;
            result["has_more"] = false;
            result["note"] = "No saved chat history found for this avatar.";
            callback(result);
            return;
        }

        // loadChatHistory() only reads the last LOG_RECALL_SIZE bytes of the
        // transcript unless told to load everything, so an explicit offset
        // (i.e. an agent actively paging) needs the full file loaded -
        // otherwise offset/has_more would be relative to a truncated tail
        // instead of the whole history.
        LLSD load_params;
        load_params["load_all_history"] = arguments.has("offset");

        std::list<LLSD> messages;
        LLLogChat::loadChatHistory(avatarLogFileBaseName(avatar_id), messages, load_params);

        S32 total = (S32)messages.size();

        S32 max_lines = arguments.has("max_lines") ? arguments["max_lines"].asInteger() : 200;
        if (max_lines <= 0)
        {
            max_lines = 200;
        }

        // Without an explicit offset, keep the old behavior (most recent
        // max_lines messages). With an explicit offset, page chronologically
        // from the oldest message forward - an agent can walk the whole
        // history via offset=0, offset=max_lines, offset=2*max_lines, ...
        // using has_more/total_messages to know when to stop.
        S32 offset = arguments.has("offset")
            ? arguments["offset"].asInteger()
            : std::max((S32)0, total - max_lines);
        if (offset < 0)
        {
            offset = 0;
        }

        LLSD out_messages = LLSD::emptyArray();
        S32 idx = 0;
        S32 returned = 0;
        for (const LLSD& msg : messages)
        {
            if (idx < offset)
            {
                ++idx;
                continue;
            }
            if (returned >= max_lines)
            {
                break;
            }
            out_messages.append(msg);
            ++returned;
            ++idx;
        }

        result["messages"] = out_messages;
        result["total_messages"] = total;
        result["offset"] = offset;
        result["returned"] = returned;
        result["has_more"] = (offset + returned) < total;
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

    // --- send_im (asynchronous: needs the avatar's display name first) -
    //
    // LLIMMgr::addSession() rejects an empty session name, so the avatar's
    // display name must be resolved - immediately if LLAvatarNameCache
    // already has it, otherwise via its async callback - before the P2P
    // session can be created/found and the message delivered through
    // LLIMModel::sendMessage(), the same call FSFloaterIM's send path uses
    // (local echo + on-disk transcript logging included, so a follow-up
    // get_chat_history call sees it). Deliberately does not show the IM
    // floater, unlike the manual UI flow, so a remote-controlled send
    // doesn't pop a window open on screen.
    //
    // Same self-owning/timeout-guarded shape as MCPAvatarProfileRequest
    // above: the scheduled timeout timer is the sole owner of this
    // object's lifetime (see complete()) - a name resolving before the
    // timeout completes the callback immediately without freeing the
    // object out from under the still-pending timer.
    class MCPSendIMRequest
    {
    public:
        MCPSendIMRequest(const LLUUID& avatar_id, const std::string& message, const ToolResultCallback& callback)
            : mAvatarId(avatar_id), mMessage(message), mCallback(callback), mDone(false)
        {
            mConnection = LLAvatarNameCache::get(avatar_id,
                [this](const LLUUID& id, const LLAvatarName& av_name)
                {
                    onNameResolved(id, av_name);
                });

            LLEventTimer::run_after(10.f, [this]()
            {
                if (!mDone)
                {
                    LLSD error_result;
                    error_result["error"] = "Timed out resolving avatar name for IM.";
                    complete(error_result);
                }
                delete this;
            });
        }

    private:
        void onNameResolved(const LLUUID& id, const LLAvatarName& av_name)
        {
            if (mDone || id != mAvatarId)
            {
                return;
            }

            LLUUID session_id = gIMMgr->addSession(av_name.getDisplayName(), IM_NOTHING_SPECIAL, mAvatarId);
            if (session_id.isNull())
            {
                LLSD error_result;
                error_result["error"] = "Failed to create IM session.";
                complete(error_result);
                return;
            }

            LLIMModel::sendMessage(mMessage, session_id, mAvatarId, IM_NOTHING_SPECIAL);

            LLSD result;
            result["status"] = "sent";
            result["session_id"] = session_id.asString();
            complete(result);
        }

        void complete(const LLSD& result)
        {
            if (mDone)
            {
                return;
            }
            mDone = true;
            mConnection.disconnect();
            mCallback(result);
        }

        LLUUID mAvatarId;
        std::string mMessage;
        ToolResultCallback mCallback;
        bool mDone;
        LLAvatarNameCache::callback_connection_t mConnection;
    };

    void toolSendIM(const LLSD& arguments, const ToolResultCallback& callback)
    {
        LLUUID avatar_id = requireAvatarId(arguments);
        if (!arguments.has("message"))
        {
            throw std::runtime_error("Missing required argument 'message'.");
        }

        new MCPSendIMRequest(avatar_id, arguments["message"].asString(), callback); // self-owning, see class comment
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
    history_props["max_lines"] = LLSDMap("type", "integer")("description", "Page size [default: 200].");
    history_props["offset"] = LLSDMap("type", "integer")("description",
        "0-based message index to start at, oldest message first [default: last page, i.e. most recent max_lines messages]. "
        "To page through the whole history, start at offset=0 and keep advancing by the previous call's 'returned' "
        "count while 'has_more' is true.");
    history_schema["properties"] = history_props;
    history_schema["required"] = LLSD::emptyArray();
    history_schema["required"].append("avatar_id");
    registry.registerTool(
        "get_chat_history",
        "Reads the locally saved 1:1 IM transcript with an avatar, paginated (max_lines per page). "
        "Response includes total_messages/offset/returned/has_more for paging through the entire history.",
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

    LLSD send_im_schema;
    send_im_schema["type"] = "object";
    LLSD send_im_props;
    send_im_props["avatar_id"] = avatar_id_prop;
    send_im_props["message"] = LLSDMap("type", "string")("description", "Message text to send.");
    send_im_schema["properties"] = send_im_props;
    send_im_schema["required"] = LLSD::emptyArray();
    send_im_schema["required"].append("avatar_id");
    send_im_schema["required"].append("message");
    registry.registerTool(
        "send_im",
        "Sends a 1:1 instant message to an avatar, creating the IM session if needed. Does not open the IM "
        "window. Asynchronous (resolves the avatar's display name first, up to ~10s timeout).",
        send_im_schema,
        toolSendIM);
}
