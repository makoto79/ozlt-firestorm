/**
 * @file llmcptoolregistry.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llmcptoolregistry.h"

#include "llsdjson.h"
#include "llversioninfo.h"
#include "lltimer.h"

#include <boost/json.hpp>

namespace
{
    // Standard JSON-RPC 2.0 error codes.
    const S32 MCP_PARSE_ERROR      = -32700;
    const S32 MCP_INVALID_REQUEST  = -32600;
    const S32 MCP_METHOD_NOT_FOUND = -32601;
    const S32 MCP_INTERNAL_ERROR   = -32603;
    const S32 MCP_RATE_LIMITED     = -32003;

    const char* const MCP_PROTOCOL_VERSION = "2024-11-05";

    // Minimum interval between two calls to the *same* tool. Deliberately
    // permissive - this is a flood guard against a runaway loop, not a
    // real rate limit; per-tool throttles (e.g. chat_say's 1s cooldown,
    // inherited from FSNearbyChatBarListener) can be stricter.
    const F64 MCP_TOOL_MIN_INTERVAL = 0.2;
}

LLMCPToolRegistry::LLMCPToolRegistry()
{
}

LLMCPToolRegistry::~LLMCPToolRegistry()
{
}

void LLMCPToolRegistry::registerTool(const std::string& name,
                                      const std::string& description,
                                      const LLSD& inputSchema,
                                      ToolHandler handler)
{
    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.inputSchema = inputSchema;
    tool.handler = handler;
    mTools[name] = tool;
}

bool LLMCPToolRegistry::isRateLimited(const std::string& tool_name)
{
    F64 now = LLTimer::getElapsedSeconds();
    auto it = mLastCallTime.find(tool_name);
    if (it != mLastCallTime.end() && (now - it->second) < MCP_TOOL_MIN_INTERVAL)
    {
        return true;
    }
    mLastCallTime[tool_name] = now;
    return false;
}

// static
LLSD LLMCPToolRegistry::makeResult(const LLSD& id, const LLSD& result)
{
    LLSD response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    return response;
}

// static
LLSD LLMCPToolRegistry::makeError(const LLSD& id, S32 code, const std::string& message)
{
    LLSD error;
    error["code"] = code;
    error["message"] = message;

    LLSD response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = error;
    return response;
}

LLSD LLMCPToolRegistry::handleInitialize(const LLSD& params) const
{
    (void)params;

    LLSD capabilities;
    capabilities["tools"] = LLSD::emptyMap();

    LLSD server_info;
    server_info["name"] = "phoenix-firestorm-mcp";
    server_info["version"] = LLVersionInfo::instance().getVersion();

    LLSD result;
    result["protocolVersion"] = MCP_PROTOCOL_VERSION;
    result["capabilities"] = capabilities;
    result["serverInfo"] = server_info;
    return result;
}

LLSD LLMCPToolRegistry::handlePing(const LLSD& params) const
{
    (void)params;
    return LLSD::emptyMap();
}

LLSD LLMCPToolRegistry::handleToolsList(const LLSD& params) const
{
    (void)params;

    LLSD tools = LLSD::emptyArray();
    for (const auto& entry : mTools)
    {
        const Tool& tool = entry.second;
        LLSD tool_desc;
        tool_desc["name"] = tool.name;
        tool_desc["description"] = tool.description;
        tool_desc["inputSchema"] = tool.inputSchema;
        tools.append(tool_desc);
    }

    LLSD result;
    result["tools"] = tools;
    return result;
}

void LLMCPToolRegistry::handleRequest(const LLSD& request, const ResponseCallback& callback)
{
    LLSD id = request.has("id") ? request["id"] : LLSD();

    if (!request.has("method") || request["method"].asString().empty())
    {
        callback(makeError(id, MCP_INVALID_REQUEST, "Missing 'method'"));
        return;
    }

    std::string method = request["method"].asString();
    LLSD params = request.has("params") ? request["params"] : LLSD::emptyMap();

    if (method == "initialize")
    {
        callback(makeResult(id, handleInitialize(params)));
        return;
    }
    if (method == "ping")
    {
        callback(makeResult(id, handlePing(params)));
        return;
    }
    if (method == "tools/list")
    {
        callback(makeResult(id, handleToolsList(params)));
        return;
    }
    if (method == "tools/call")
    {
        std::string tool_name = params.has("name") ? params["name"].asString() : "";
        auto it = mTools.find(tool_name);
        if (it == mTools.end())
        {
            callback(makeError(id, MCP_METHOD_NOT_FOUND, "Unknown tool: " + tool_name));
            return;
        }

        if (isRateLimited(tool_name))
        {
            callback(makeError(id, MCP_RATE_LIMITED, "Rate limited: '" + tool_name + "' called too soon, slow down."));
            return;
        }

        LLSD arguments = params.has("arguments") ? params["arguments"] : LLSD::emptyMap();

        // Wraps a raw tool result into the JSON-RPC envelope. May run
        // synchronously (below) or later, from an async tool's own
        // completion path (e.g. after a network round-trip).
        auto complete = [callback, id](const LLSD& tool_result)
        {
            LLSD content_block;
            content_block["type"] = "text";
            content_block["text"] = boost::json::serialize(LlsdToJson(tool_result));

            LLSD result;
            result["content"] = LLSD::emptyArray();
            result["content"].append(content_block);
            result["structuredContent"] = tool_result;
            callback(makeResult(id, result));
        };

        try
        {
            it->second.handler(arguments, complete);
        }
        catch (const std::exception& ex)
        {
            // Only catches synchronous failures (thrown before the handler
            // returns control to us). Asynchronous tools are responsible
            // for reporting their own errors via the callback.
            callback(makeError(id, MCP_INTERNAL_ERROR, std::string("Tool execution failed: ") + ex.what()));
        }
        return;
    }

    callback(makeError(id, MCP_METHOD_NOT_FOUND, "Unknown method: " + method));
}
