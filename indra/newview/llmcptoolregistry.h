/**
 * @file llmcptoolregistry.h
 * @brief JSON-RPC dispatch and tool registry for the local MCP server
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

#ifndef LL_LLMCPTOOLREGISTRY_H
#define LL_LLMCPTOOLREGISTRY_H

#include "llsingleton.h"
#include "llsd.h"

#include <functional>
#include <map>
#include <string>

// Tool registry and JSON-RPC 2.0 dispatcher for the local MCP server.
// Operates entirely on LLSD; the HTTP/JSON wire format is handled by
// LLMCPHttpNode (see llmcphttpnode.h).
//
// Tool handlers are callback-based rather than simple return-a-value
// functions, because some tools (e.g. get_avatar_profile) complete
// asynchronously - the reply only arrives after a network round-trip via
// LLAvatarPropertiesObserver, potentially several frames/seconds later.
// Synchronous tools simply invoke the callback immediately, inline.
class LLMCPToolRegistry : public LLSingleton<LLMCPToolRegistry>
{
    LLSINGLETON(LLMCPToolRegistry);
    ~LLMCPToolRegistry();

public:
    using ToolResultCallback = std::function<void(const LLSD& result)>;
    using ToolHandler = std::function<void(const LLSD& arguments, const ToolResultCallback& callback)>;

    // Full JSON-RPC 2.0 response object (LLSD map mirroring the JSON that
    // will be sent back on the wire).
    using ResponseCallback = std::function<void(const LLSD& response)>;

    // Registers a tool. Overwrites any previously registered tool of the
    // same name.
    void registerTool(const std::string& name,
                       const std::string& description,
                       const LLSD& inputSchema,
                       ToolHandler handler);

    // Handles a single, already JSON->LLSD decoded JSON-RPC 2.0 request
    // object. Invokes callback exactly once with the JSON-RPC response -
    // synchronously for all built-in methods (initialize/ping/tools/list)
    // and for synchronous tools, but possibly later for asynchronous tools.
    void handleRequest(const LLSD& request, const ResponseCallback& callback);

private:
    struct Tool
    {
        std::string name;
        std::string description;
        LLSD        inputSchema;
        ToolHandler handler;
    };

    LLSD handleInitialize(const LLSD& params) const;
    LLSD handlePing(const LLSD& params) const;
    LLSD handleToolsList(const LLSD& params) const;

    // Simple per-tool flood guard: rejects a call to the same tool made
    // less than MCP_TOOL_MIN_INTERVAL after the previous one, to catch a
    // runaway AI-side loop before it does something like spamming
    // chat_say or avatar_teleport many times a second. chat_say/chat_shout
    // additionally get their own 1s throttle for free from the revived
    // FSNearbyChatBarListener (see llmcptools.cpp).
    bool isRateLimited(const std::string& tool_name);

    static LLSD makeResult(const LLSD& id, const LLSD& result);
    static LLSD makeError(const LLSD& id, S32 code, const std::string& message);

    std::map<std::string, Tool> mTools;
    std::map<std::string, F64>  mLastCallTime;
};

#endif // LL_LLMCPTOOLREGISTRY_H
