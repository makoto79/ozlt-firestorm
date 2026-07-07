/**
 * @file llmcptoolsinventory.cpp
 * @brief Inventory MCP tools (inventory_list, notecard_write)
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
#include "lleventtimer.h"
#include "llevents.h"
#include "llfoldertype.h"
#include "llinventorymodel.h"
#include "llnotecard.h"
#include "llpermissionsflags.h"
#include "llsdutil.h"
#include "lluuid.h"
#include "llviewerassetupload.h"
#include "llviewerregion.h"

#include <sstream>
#include <stdexcept>

// Defined in llviewerinventory.cpp with external linkage, but not exposed
// via any header. Signature matched exactly; used by the "New Note" /
// "New Script" / etc. inventory menu actions to create a blank item of a
// given type, invoking created_cb(new_item_id) once the server confirms
// creation.
void create_new_item(const std::string& name,
                      const LLUUID& parent_id,
                      LLAssetType::EType asset_type,
                      LLInventoryType::EType inv_type,
                      U32 next_owner_perm,
                      std::function<void(const LLUUID&)> created_cb = nullptr);

namespace
{
    using ToolResultCallback = LLMCPToolRegistry::ToolResultCallback;

    // --- inventory_list (synchronous: bridges to the already-active
    // "LLInventory" LLEventAPI pump, all local/cached data, no network
    // wait) ---------------------------------------------------------

    // Posts `event` to `pump_name` and synchronously captures whatever
    // gets sent back to the "reply" pump, exactly as LLEventAPI's
    // Response class posts one when its request had a "reply" key.
    // LLEventPump::post() dispatches to listeners immediately/in-process,
    // so this works without any async machinery - the reply pump is a
    // plain stack-local LLEventStream, auto-unregistered on return.
    LLSD callEventAPISync(const std::string& pump_name, LLSD event)
    {
        LLSD captured;
        LLEventStream reply_pump("mcp-reply", true); // tweak=true: auto-unique name
        reply_pump.listen("mcp", [&captured](const LLSD& response) -> bool
        {
            captured = response;
            return false;
        });

        event["reply"] = reply_pump.getName();
        LLEventPumps::instance().obtain(pump_name).post(event);
        return captured;
    }

    void toolInventoryList(const LLSD& arguments, const ToolResultCallback& callback)
    {
        LLUUID folder_id = arguments.has("folder_id")
            ? LLUUID(arguments["folder_id"].asString())
            : gInventory.getRootFolderID();
        if (folder_id.isNull())
        {
            throw std::runtime_error("Could not resolve inventory folder (not logged in?).");
        }

        LLSD event;
        event["op"] = "getDirectDescendants";
        event["folder_id"] = folder_id;
        LLSD reply = callEventAPISync("LLInventory", event);

        LLSD result;
        if (reply.has("error"))
        {
            result["error"] = reply["error"];
            callback(result);
            return;
        }

        LLSD folders = LLSD::emptyArray();
        if (reply.has("categories"))
        {
            for (LLSD::map_const_iterator it = reply["categories"].beginMap(); it != reply["categories"].endMap(); ++it)
            {
                folders.append(it->second);
            }
        }

        LLSD items = LLSD::emptyArray();
        if (reply.has("items"))
        {
            for (LLSD::map_const_iterator it = reply["items"].beginMap(); it != reply["items"].endMap(); ++it)
            {
                items.append(it->second);
            }
        }

        result["folder_id"] = folder_id.asString();
        result["folders"] = folders;
        result["items"] = items;
        callback(result);
    }

    // --- notecard_write (asynchronous: item creation, then a separate
    // asset upload, each a network round-trip) -----------------------
    //
    // Same lifetime pattern as MCPAvatarProfileRequest (llmcptoolssocial.cpp):
    // a single timeout timer, scheduled up front, is the sole owner of
    // this object's lifetime (`delete this` appears only in that one
    // place). Success paths just mark mDone and report the result,
    // without freeing the object out from under the still-pending timer.
    class MCPNotecardWriteRequest
    {
    public:
        MCPNotecardWriteRequest(const std::string& name, const std::string& text,
                                 const LLUUID& parent_id, const ToolResultCallback& callback)
            : mText(text), mCallback(callback), mDone(false)
        {
            LLEventTimer::run_after(20.f, [this]()
            {
                if (!mDone)
                {
                    LLSD error_result;
                    error_result["error"] = "Timed out creating/uploading notecard.";
                    complete(error_result);
                }
                delete this;
            });

            create_new_item(name, parent_id, LLAssetType::AT_NOTECARD, LLInventoryType::IT_NOTECARD, PERM_ALL,
                [this](const LLUUID& item_id) { onItemCreated(item_id); });
        }

    private:
        void onItemCreated(const LLUUID& item_id)
        {
            if (mDone)
            {
                return;
            }
            if (item_id.isNull())
            {
                LLSD result;
                result["error"] = "Failed to create notecard inventory item.";
                complete(result);
                return;
            }

            LLViewerRegion* region = gAgent.getRegion();
            std::string url = region ? region->getCapability("UpdateNotecardAgentInventory") : std::string();
            if (url.empty())
            {
                LLSD result;
                result["error"] = "Region has no UpdateNotecardAgentInventory capability.";
                result["item_id"] = item_id.asString();
                complete(result);
                return;
            }

            LLNotecard notecard;
            notecard.setText(mText);
            std::ostringstream out;
            notecard.exportStream(out);

            LLResourceUploadInfo::ptr_t uploadInfo = std::make_shared<LLBufferedAssetUploadInfo>(
                item_id, LLAssetType::AT_NOTECARD, out.str(),
                [this](LLUUID itemId, LLUUID newAssetId, LLUUID, LLSD)
                {
                    onUploadFinished(itemId, newAssetId);
                },
                [this](LLUUID itemId, LLUUID, LLSD, std::string reason) -> bool
                {
                    onUploadFailed(itemId, reason);
                    return true;
                });

            LLViewerAssetUpload::EnqueueInventoryUpload(url, uploadInfo);
        }

        void onUploadFinished(const LLUUID& item_id, const LLUUID& asset_id)
        {
            LLSD result;
            result["status"] = "created";
            result["item_id"] = item_id.asString();
            result["asset_id"] = asset_id.asString();
            complete(result);
        }

        void onUploadFailed(const LLUUID& item_id, const std::string& reason)
        {
            LLSD result;
            result["error"] = "Notecard asset upload failed: " + reason;
            result["item_id"] = item_id.asString();
            complete(result);
        }

        void complete(const LLSD& result)
        {
            if (mDone)
            {
                return;
            }
            mDone = true;
            mCallback(result);
        }

        std::string mText;
        ToolResultCallback mCallback;
        bool mDone;
    };

    void toolNotecardWrite(const LLSD& arguments, const ToolResultCallback& callback)
    {
        if (!arguments.has("name") || !arguments.has("text"))
        {
            throw std::runtime_error("Missing required argument 'name' or 'text'.");
        }

        std::string name = arguments["name"].asString();
        std::string text = arguments["text"].asString();
        LLUUID parent_id = arguments.has("folder_id")
            ? LLUUID(arguments["folder_id"].asString())
            : gInventory.findCategoryUUIDForType(LLFolderType::FT_NOTECARD);
        if (parent_id.isNull())
        {
            throw std::runtime_error("Could not resolve destination folder for notecard.");
        }

        new MCPNotecardWriteRequest(name, text, parent_id, callback); // self-owning, see class comment
    }
}

void registerMCPInventoryTools()
{
    LLMCPToolRegistry& registry = LLMCPToolRegistry::instance();

    LLSD list_schema;
    list_schema["type"] = "object";
    LLSD list_props;
    list_props["folder_id"] = LLSDMap("type", "string")("description", "UUID of the folder to list [default: My Inventory root].");
    list_schema["properties"] = list_props;
    registry.registerTool(
        "inventory_list",
        "Lists the direct contents (subfolders and items) of an inventory folder [default: My Inventory root].",
        list_schema,
        toolInventoryList);

    LLSD write_schema;
    write_schema["type"] = "object";
    LLSD write_props;
    write_props["name"] = LLSDMap("type", "string")("description", "Name of the new notecard inventory item.");
    write_props["text"] = LLSDMap("type", "string")("description", "Plain-text body of the notecard.");
    write_props["folder_id"] = LLSDMap("type", "string")("description", "Destination folder UUID [default: Notecards folder].");
    write_schema["properties"] = write_props;
    write_schema["required"] = LLSD::emptyArray();
    write_schema["required"].append("name");
    write_schema["required"].append("text");
    registry.registerTool(
        "notecard_write",
        "Creates a new notecard inventory item with the given text content. Asynchronous "
        "(inventory item creation + asset upload, each a network round-trip; ~20s timeout).",
        write_schema,
        toolNotecardWrite);
}
