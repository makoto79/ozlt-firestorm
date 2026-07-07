/**
 * @file llmcphttpnode.cpp
 * @brief HTTP transport (JSON-RPC over HTTP) for the local MCP server
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

#include "llmcphttpnode.h"

#include "llmcptoolregistry.h"

#include "llapr.h"
#include "llchainio.h"
#include "llhttpconstants.h"
#include "lliohttpserver.h"
#include "lliopipe.h"
#include "lliosocket.h"
#include "llnotificationsutil.h"
#include "llpumpio.h"
#include "llsdjson.h"
#include "llviewercontrol.h"

#include <boost/json.hpp>

extern LLPumpIO* gServicePump;

bool LLMCPHttpServer::sRunning = false;

namespace
{
    // Constant-time comparison to avoid leaking the configured token
    // length/prefix through response-timing side channels.
    bool constantTimeEquals(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
        }
        return diff == 0;
    }

    LLSD jsonContentTypeHeader()
    {
        LLSD headers;
        headers[HTTP_OUT_HEADER_CONTENT_TYPE] = "application/json";
        return headers;
    }

    LLSD makeJsonRpcError(S32 code, const std::string& message)
    {
        LLSD error;
        error["code"] = code;
        error["message"] = message;

        LLSD response;
        response["jsonrpc"] = "2.0";
        response["id"] = LLSD();
        response["error"] = error;
        return response;
    }

    std::string serializeLlsdAsJson(const LLSD& value)
    {
        return boost::json::serialize(LlsdToJson(value));
    }

    // Wraps our root LLHTTPNode tree (with the "mcp" child mounted) as the
    // chain factory that LLIOServerSocket needs for every accepted
    // connection. Mirrors LLIOHTTPServer::create()'s private
    // LLHTTPResponseFactory, but we cannot reuse that class directly since
    // it is a translation-unit-local implementation detail of
    // lliohttpserver.cpp. Written ourselves so we retain the LLSocket and
    // can gracefully handle a bind failure instead of the LL_ERRS crash in
    // LLIOHTTPServer::create().
    class LLMCPResponseFactory : public LLChainIOFactory
    {
    public:
        LLMCPResponseFactory()
        {
            mRoot.addNode("mcp", new LLMCPHttpNode());
        }

        bool build(LLPumpIO::chain_t& chain, LLSD ctx) const override
        {
            LLIOHTTPServer::createPipe(chain, mRoot, ctx);
            return true;
        }

    private:
        LLHTTPNode mRoot;
    };
}

// static
bool LLMCPHttpNode::checkAuthToken(const LLSD& context)
{
    std::string configured_token = gSavedSettings.getString("MCPServerAuthToken");
    if (configured_token.empty())
    {
        // No token configured - rely on localhost-only binding.
        return true;
    }

    std::string header = context[CONTEXT_REQUEST][CONTEXT_HEADERS]["authorization"].asString();
    const std::string prefix = "Bearer ";
    if (header.size() <= prefix.size() || header.compare(0, prefix.size(), prefix) != 0)
    {
        return false;
    }

    return constantTimeEquals(header.substr(prefix.size()), configured_token);
}

void LLMCPHttpNode::post(ResponsePtr response, const LLSD& context, const LLSD& input) const
{
    if (!gSavedSettings.getBOOL("MCPServerEnabled"))
    {
        response->extendedResult(503,
            serializeLlsdAsJson(makeJsonRpcError(-32000, "MCP server is disabled")),
            jsonContentTypeHeader());
        return;
    }

    if (!checkAuthToken(context))
    {
        response->extendedResult(401,
            serializeLlsdAsJson(makeJsonRpcError(-32001, "Unauthorized")),
            jsonContentTypeHeader());
        return;
    }

    // CONTENT_TYPE_TEXT makes the wire server hand us the raw body as a
    // plain LLSD string, bypassing its hardcoded LLSD-XML request codec.
    std::string body = input.asString();

    boost::system::error_code ec;
    boost::json::value parsed = boost::json::parse(body, ec);

    if (ec)
    {
        response->extendedResult(200,
            serializeLlsdAsJson(makeJsonRpcError(-32700, "Parse error")),
            jsonContentTypeHeader());
        return;
    }

    LLSD request = LlsdFromJson(parsed);

    // `response` is ref-counted (LLPointer); capturing it by value keeps
    // the HTTP response alive until the tool actually completes, which for
    // asynchronous tools (e.g. get_avatar_profile) may be well after this
    // post() call returns.
    LLMCPToolRegistry::instance().handleRequest(request, [response](const LLSD& json_rpc_response) mutable
    {
        response->extendedResult(200, serializeLlsdAsJson(json_rpc_response), jsonContentTypeHeader());
    });
}

// static
void LLMCPHttpServer::startIfEnabled()
{
    if (sRunning || !gSavedSettings.getBOOL("MCPServerEnabled"))
    {
        return;
    }

    U32 port = gSavedSettings.getU32("MCPServerPort");

    LLSocket::ptr_t socket = LLSocket::create(
        gAPRPoolp, LLSocket::STREAM_TCP, static_cast<U16>(port), "127.0.0.1");
    if (!socket)
    {
        LL_WARNS("MCP") << "MCP server: failed to bind to 127.0.0.1:" << port
                         << " - server not started (port already in use?)" << LL_ENDL;
        return;
    }

    std::shared_ptr<LLChainIOFactory> factory(new LLMCPResponseFactory());
    LLIOServerSocket* server = new LLIOServerSocket(gAPRPoolp, socket, factory);

    LLPumpIO::chain_t chain;
    chain.push_back(LLIOPipe::ptr_t(server));
    gServicePump->addChain(chain, NEVER_CHAIN_EXPIRY_SECS);

    sRunning = true;
    LL_INFOS("MCP") << "MCP server listening on 127.0.0.1:" << port << LL_ENDL;

    // Visible reminder that AI remote-control is active this session -
    // easy to miss otherwise since the setting only takes effect after a
    // restart (see class comment).
    LLSD args;
    args["MESSAGE"] = llformat("MCP server active: AI assistants can control this viewer via port %u.", port);
    LLNotificationsUtil::add("SystemMessageTip", args);
}
