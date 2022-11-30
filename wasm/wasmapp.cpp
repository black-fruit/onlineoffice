/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "wasmapp.hpp"

int coolwsd_server_socket_fd = -1;

const char* user_name;
const int SHOW_JS_MAXLEN = 70;

static std::string fileURL;
static COOLWSD *coolwsd = nullptr;
static int fakeClientFd;
static int closeNotificationPipeForForwardingThread[2] = {-1, -1};
emscripten::val contentArray = emscripten::val::array();

static void send2JS(const std::vector<char>& buffer)
{
    LOG_TRC_NOFILE("Send to JS: " << COOLProtocol::getAbbreviatedMessage(buffer.data(), buffer.size()));

    std::string js;

    // Check if the message is binary. We say that any message that isn't just a single line is
    // "binary" even if that strictly speaking isn't the case; for instance the commandvalues:
    // message has a long bunch of non-binary JSON on multiple lines. But _onMessage() in Socket.js
    // handles it fine even if such a message, too, comes in as an ArrayBuffer. (Look for the
    // "textMsg = String.fromCharCode.apply(null, imgBytes);".)

    const char *newline = (const char *)memchr(buffer.data(), '\n', buffer.size());
    if (newline != nullptr)
    {
        // The data needs to be an ArrayBuffer
        js = "window.TheFakeWebSocket.onmessage({'data': Base64ToArrayBuffer('";
        js = js + std::string(buffer.data(), buffer.size());
        js = js + "')});";
    }
    else
    {
        const unsigned char *ubufp = (const unsigned char *)buffer.data();
        std::vector<char> data;
        for (size_t i = 0; i < buffer.size(); i++)
        {
            if (ubufp[i] < ' ' || ubufp[i] == '\'' || ubufp[i] == '\\')
            {
                data.push_back('\\');
                data.push_back('x');
                data.push_back("0123456789abcdef"[(ubufp[i] >> 4) & 0x0F]);
                data.push_back("0123456789abcdef"[ubufp[i] & 0x0F]);
            }
            else
            {
                data.push_back(ubufp[i]);
            }
        }
        data.push_back(0);

        js = "window.TheFakeWebSocket.onmessage({'data': '";
        js = js + std::string(buffer.data(), buffer.size());
        js = js + "'});";
    }

    std::string subjs = js.substr(0, std::min(std::string::size_type(SHOW_JS_MAXLEN), js.length()));
    if (js.length() > SHOW_JS_MAXLEN)
        subjs += "...";

    LOG_TRC_NOFILE( "Evaluating JavaScript: " << subjs);

    emscripten_run_script(js.c_str());
}

static void handle_cool_message()
{
    // TODO what msg?
    const char* string_value = "HULLO";
    if (strcmp(string_value, "HULLO") == 0)
    {
        // Now we know that the JS has started completely

        // Contact the permanently (during app lifetime) listening COOLWSD server
        // "public" socket
        assert(coolwsd_server_socket_fd != -1);
        int rc = fakeSocketConnect(fakeClientFd, coolwsd_server_socket_fd);
        assert(rc != -1);

        // Create a socket pair to notify the below thread when the document has been closed
        fakeSocketPipe2(closeNotificationPipeForForwardingThread);

        // Start another thread to read responses and forward them to the JavaScript
        std::thread([]
                    {
                        Util::setThreadName("app2js");
                        while (true)
                        {
                           struct pollfd pollfd[2];
                           pollfd[0].fd = fakeClientFd;
                           pollfd[0].events = POLLIN;
                           pollfd[1].fd = closeNotificationPipeForForwardingThread[1];
                           pollfd[1].events = POLLIN;
                           if (fakeSocketPoll(pollfd, 2, -1) > 0)
                           {
                               if (pollfd[1].revents == POLLIN)
                               {
                                   // The code below handling the "BYE" fake Websocket
                                   // message has closed the other end of the
                                   // closeNotificationPipeForForwardingThread. Let's close
                                   // the other end too just for cleanliness, even if a
                                   // FakeSocket as such is not a system resource so nothing
                                   // is saved by closing it.
                                   fakeSocketClose(closeNotificationPipeForForwardingThread[1]);

                                   // Close our end of the fake socket connection to the
                                   // ClientSession thread, so that it terminates
                                   fakeSocketClose(fakeClientFd);

                                   return;
                               }
                               if (pollfd[0].revents == POLLIN)
                               {
                                   int n = fakeSocketAvailableDataLength(fakeClientFd);
                                   if (n == 0)
                                       return;
                                   std::vector<char> buf(n);
                                   n = fakeSocketRead(fakeClientFd, buf.data(), n);
                                   send2JS(buf);
                               }
                           }
                           else
                               break;
                       }
                       assert(false);
                    }).detach();

        // First we simply send it the URL. This corresponds to the GET request with Upgrade to
        // WebSocket.
        LOG_TRC_NOFILE("Actually sending to Online:" << fileURL);

        std::thread([]
                    {
                        struct pollfd pollfd;
                        pollfd.fd = fakeClientFd;
                        pollfd.events = POLLOUT;
                        fakeSocketPoll(&pollfd, 1, -1);
                        fakeSocketWrite(fakeClientFd, fileURL.c_str(), fileURL.size());
                    }).detach();
    }
    else if (strcmp(string_value, "BYE") == 0)
    {
        LOG_TRC_NOFILE("Document window terminating on JavaScript side. Closing our end of the socket.");

        // Close one end of the socket pair, that will wake up the forwarding thread above
        fakeSocketClose(closeNotificationPipeForForwardingThread[0]);
    }
    else
    {
        // As above
        char *string_copy = strdup(string_value);
        std::thread([=]
                    {
                        struct pollfd pollfd;
                        pollfd.fd = fakeClientFd;
                        pollfd.events = POLLOUT;
                        fakeSocketPoll(&pollfd, 1, -1);
                        fakeSocketWrite(fakeClientFd, string_copy, strlen(string_copy));
                        free(string_copy);
                    }).detach();
    }
}

/// Close the document.
void closeDocument()
{
    // Close one end of the socket pair, that will wake up the forwarding thread that was constructed in HULLO
    fakeSocketClose(closeNotificationPipeForForwardingThread[0]);

    LOG_DBG("Waiting for COOLWSD to finish...");
    std::unique_lock<std::mutex> lock(COOLWSD::lokit_main_mutex);
    LOG_DBG("COOLWSD has finished.");
}

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s document\n", argv[0]);
        _exit(1); // avoid log cleanup
    }

    Log::initialize("WASM", "trace", false, false, {});
    Util::setThreadName("main");

    fakeSocketSetLoggingCallback([](const std::string& line)
                                 {
                                     LOG_TRC_NOFILE(line);
                                 });

    std::thread([]
                {
                    assert(coolwsd == nullptr);
                    char *argv[2];
                    argv[0] = strdup("wasm");
                    argv[1] = nullptr;
                    Util::setThreadName("app");
                    while (true)
                    {
                        coolwsd = new COOLWSD();
                        coolwsd->run(1, argv);
                        delete coolwsd;
                        LOG_TRC("One run of COOLWSD completed");
                    }
                }).detach();

    fakeClientFd = fakeSocketSocket();
    handle_cool_message();

    return 0;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab cinoptions=b1,g0,N-s cinkeys+=0=break: */