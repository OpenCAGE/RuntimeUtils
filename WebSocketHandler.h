#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <string>
#include <thread>

class WebSocketHandler
{
public:
    static bool Initialize(int port = 8765);
    static void Shutdown();
    static bool IsRunning();

private:
    static void ServerThread(int port);
    static bool HandleWebSocketHandshake(SOCKET clientSocket, const std::string& request);
    static void HandleWebSocketMessage(SOCKET clientSocket, const std::string& message);
    static std::string GenerateWebSocketAccept(const std::string& key);
    static std::string Base64Encode(const std::string& input);
    static void SendWebSocketFrame(SOCKET socket, const std::string& message);
    static std::string ReceiveWebSocketFrame(SOCKET socket);
    static std::string SHA1Hash(const std::string& input);

    static bool s_running;
    static std::thread s_serverThread;
    static SOCKET s_listenSocket;
};

