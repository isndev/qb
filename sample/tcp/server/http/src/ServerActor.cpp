/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

#include "ServerActor.h"
#include "Session.h"
#include <iostream>
#include <qb/io/protocol/json.h>
#include <utility>

ServerActor::ServerActor(std::string iface, uint16_t port) noexcept
    : _iface(std::move(iface))
    , _port(port) {

    qb::http::Response res;
    res.status_code = HTTP_STATUS_NOT_FOUND;
    res.headers = {
        {"Server", "qb/2.0.0"}, {"Content-Type", "text/html"}, {"Connection", "close"}};

    _router.setDefaultResponse(res)
        .GET("/",
             [](auto &ctx) {
                 ctx.response.status_code = HTTP_STATUS_OK;
                 ctx.response.body = "<!DOCTYPE html>"
                                     "<html>"
                                     "<head><title>Home</title></head>"
                                     "<body>Home</body>"
                                     "</html>";
                 ctx.session.publish(ctx.response);
             })
        .GET("/file/:path",
             [](auto &ctx) {
                 std::string fname = "./" + ctx.param("path", "index.html");
                 if (ctx.session._file.open(fname)) {
                     ctx.response.status_code = HTTP_STATUS_OK;
                     ctx.response.content_length = ctx.session._file.expected_size();
                 }
                 ctx.session.publish(ctx.response);
             })
        .GET("/message/:msg", [](auto &ctx) {
            ctx.response.status_code = HTTP_STATUS_OK;
            ctx.response.headers["Content-Type"] = "application/json";
            ctx.response.body = qb::json::object{{"message", ctx.param("msg", "empty")},
                                         {"valid", ctx.query("valid", "false")}}
                                    .dump();
            ctx.session.publish(ctx.response);
        });
}

// Actor initialization
bool
ServerActor::onInit() {
    if (qb::io::SocketStatus::Done == transport().listen(_port, _iface)) {
        std::cout << "Server started listening on " << _iface << ":" << _port
                  << std::endl;
        // register io to qb::io::listener
        start();
        return true;
    }

    return false;
}

// Called from qb::io on new session connected
void
ServerActor::on(Session &session) {
    std::cout << "Session(" << session.transport().ident() << ") ip("
              << session.transport().getRemoteAddress() << ") connected" << std::endl;
}

// Called from qb::io on server disconnected
void
ServerActor::on(qb::io::async::event::disconnected const &) {
    // kill ServerActor
    kill();
}