/**
 * @file llmcphttpnode.h
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

#ifndef LL_LLMCPHTTPNODE_H
#define LL_LLMCPHTTPNODE_H

#include "llhttpnode.h"

// Leaf node answering POST requests on the "mcp" path with JSON-RPC 2.0
// over plain JSON (not LLSD-XML, which is what LLHTTPNode speaks by
// default). Declaring CONTENT_TYPE_TEXT makes the wire server hand us the
// raw request body as a string instead of trying to parse it as LLSD-XML.
class LLMCPHttpNode : public LLHTTPNode
{
public:
    EHTTPNodeContentType getContentType() const override { return CONTENT_TYPE_TEXT; }

    void post(ResponsePtr response, const LLSD& context, const LLSD& input) const override;

private:
    static bool checkAuthToken(const LLSD& context);
};

// Lifecycle management for the local MCP HTTP listener.
//
// NOTE: the listener is started at most once, at viewer launch, if
// MCPServerEnabled is set. LLPumpIO/LLIOHTTPServer (the underlying
// framework) offer no supported way to tear down a running listener
// chain, so there is intentionally no stop()/restart() here - toggling
// the setting at runtime requires a viewer restart to take effect. The
// node itself still re-checks the setting on every request as a second
// line of defense.
class LLMCPHttpServer
{
public:
    // Starts the listener if MCPServerEnabled is true. Safe to call
    // unconditionally; does nothing if already running or disabled.
    // Never throws/crashes on a port conflict - logs a warning instead.
    static void startIfEnabled();

    static bool isRunning() { return sRunning; }

private:
    static bool sRunning;
};

#endif // LL_LLMCPHTTPNODE_H
