set(server_file "${SOURCE_DIR}/lib/src/HttpServer.cc")
file(READ "${server_file}" source)
string(REPLACE "\r\n" "\n" source "${source}")
set(original [=[                conn->send(utils::formattedString(
                    "HTTP/1.1 %d %s\r\nConnection: close\r\n\r\n",
                    code,
                    statusCodeToString(code).data()));]=])
set(replacement [=[                auto response = app().getCustomErrorHandler()(code, req);
                response->setCloseConnection(true);
                sendResponse(conn, response, req->method() == Head);]=])
string(FIND "${source}" "${replacement}" patched)
if(NOT patched EQUAL -1)
    return()
endif()
string(FIND "${source}" "${original}" position)
if(position EQUAL -1)
    message(FATAL_ERROR "Pinned Drogon HTTP error patch no longer matches")
endif()
string(REPLACE "${original}" "${replacement}" source "${source}")
file(WRITE "${server_file}" "${source}")
