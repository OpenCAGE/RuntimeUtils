#include "WebSocketHandler.h"
#include "Menu.h"
#include "GAME_LEVEL_MANAGER.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <wincrypt.h>
#include "libs/json.hpp"

using json = nlohmann::json;

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

bool WebSocketHandler::s_running = false;
std::thread WebSocketHandler::s_serverThread;
SOCKET WebSocketHandler::s_listenSocket = INVALID_SOCKET;

bool WebSocketHandler::Initialize(int port)
{
    if (s_running)
        return false;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        return false;
    }

    s_running = true;
    s_serverThread = std::thread(ServerThread, port);
    return true;
}

void WebSocketHandler::Shutdown()
{
    if (!s_running)
        return;

    s_running = false;

    if (s_listenSocket != INVALID_SOCKET)
    {
        closesocket(s_listenSocket);
        s_listenSocket = INVALID_SOCKET;
    }

    if (s_serverThread.joinable())
    {
        s_serverThread.join();
    }

    WSACleanup();
}

bool WebSocketHandler::IsRunning()
{
    return s_running;
}

void WebSocketHandler::ServerThread(int port)
{
    s_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listenSocket == INVALID_SOCKET)
    {
        s_running = false;
        return;
    }

    int opt = 1;
    setsockopt(s_listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(s_listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        closesocket(s_listenSocket);
        s_listenSocket = INVALID_SOCKET;
        s_running = false;
        return;
    }

    if (listen(s_listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        closesocket(s_listenSocket);
        s_listenSocket = INVALID_SOCKET;
        s_running = false;
        return;
    }

    while (s_running)
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(s_listenSocket, &readSet);

        timeval timeout{};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int result = select(0, &readSet, nullptr, nullptr, &timeout);
        if (result > 0 && FD_ISSET(s_listenSocket, &readSet))
        {
            sockaddr_in clientAddr{};
            int clientAddrLen = sizeof(clientAddr);
            SOCKET clientSocket = accept(s_listenSocket, (sockaddr*)&clientAddr, &clientAddrLen);

            if (clientSocket != INVALID_SOCKET)
            {
                char buffer[4096];
                int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                
                if (bytesReceived > 0)
                {
                    buffer[bytesReceived] = '\0';
                    std::string request(buffer);

                    if (HandleWebSocketHandshake(clientSocket, request))
                    {
                        while (s_running)
                        {
                            std::string message = ReceiveWebSocketFrame(clientSocket);
                            if (message.empty())
                                break;

                            HandleWebSocketMessage(clientSocket, message);
                        }
                    }
                }

                closesocket(clientSocket);
            }
        }
    }

    if (s_listenSocket != INVALID_SOCKET)
    {
        closesocket(s_listenSocket);
        s_listenSocket = INVALID_SOCKET;
    }
}

bool WebSocketHandler::HandleWebSocketHandshake(SOCKET clientSocket, const std::string& request)
{
    std::string key;
    std::istringstream iss(request);
    std::string line;
    
    while (std::getline(iss, line))
    {
        if (line.find("Sec-WebSocket-Key:") != std::string::npos)
        {
            size_t pos = line.find(':');
            if (pos != std::string::npos)
            {
                key = line.substr(pos + 1);
                key.erase(0, key.find_first_not_of(" \t\r\n"));
                key.erase(key.find_last_not_of(" \t\r\n") + 1);
                break;
            }
        }
    }

    if (key.empty())
        return false;

    std::string acceptKey = GenerateWebSocketAccept(key);

    std::string response = 
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + acceptKey + "\r\n"
        "\r\n";

    send(clientSocket, response.c_str(), (int)response.length(), 0);
    return true;
}

void WebSocketHandler::HandleWebSocketMessage(SOCKET clientSocket, const std::string& message)
{
    std::string level_name = "";
    try
    {
        json jsonData = json::parse(message);
        if (jsonData.contains("load_level") && jsonData["load_level"].is_string())
        {
            level_name = jsonData["load_level"].get<std::string>();
        }
    }
    catch (const std::exception e) { }
    
    if (level_name != "")
    {
        if (GAME_LEVEL_MANAGER::m_instance != nullptr)
        {
            //TODO: currently just reloading active level (which will fail for custom added levels!!) - need to request by string
            int currentLevel = GAME_LEVEL_MANAGER::get_current_level(GAME_LEVEL_MANAGER::m_instance);
            if (currentLevel != 0)
            {
                GAME_LEVEL_MANAGER::queue_level(GAME_LEVEL_MANAGER::m_instance, currentLevel);
                GAME_LEVEL_MANAGER::request_next_level(GAME_LEVEL_MANAGER::m_instance, false);
                
                json response;
                response["status"] = "success";
                response["message"] = "Level loaded";
                SendWebSocketFrame(clientSocket, response.dump());
            }
        }
    }
}

std::string WebSocketHandler::GenerateWebSocketAccept(const std::string& key)
{
    const std::string magicString = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + magicString;
    
    std::string hash = SHA1Hash(combined);
    return Base64Encode(hash);
}

std::string WebSocketHandler::Base64Encode(const std::string& input)
{
    DWORD encodedLen = 0;
    if (!CryptBinaryToStringA((BYTE*)input.c_str(), (DWORD)input.length(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &encodedLen))
        return "";

    std::string encoded;
    encoded.resize(encodedLen);
    if (!CryptBinaryToStringA((BYTE*)input.c_str(), (DWORD)input.length(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &encoded[0], &encodedLen))
        return "";

    if (encodedLen > 0 && encoded[encodedLen - 1] == '\0')
    {
        encoded.resize(encodedLen - 1);
    }
    else
    {
        encoded.resize(encodedLen);
    }
    return encoded;
}

std::string WebSocketHandler::SHA1Hash(const std::string& input)
{
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string hash(20, '\0');

    if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return "";

    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash))
    {
        CryptReleaseContext(hProv, 0);
        return "";
    }

    if (!CryptHashData(hHash, (BYTE*)input.c_str(), (DWORD)input.length(), 0))
    {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }

    DWORD hashLen = 20;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, (BYTE*)hash.data(), &hashLen, 0))
    {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return hash;
}

void WebSocketHandler::SendWebSocketFrame(SOCKET socket, const std::string& message)
{
    size_t messageLen = message.length();
    
    std::vector<unsigned char> frame;
    frame.push_back(0x81);
    
    if (messageLen < 126)
    {
        frame.push_back((unsigned char)messageLen);
    }
    else if (messageLen < 65536)
    {
        frame.push_back(126);
        frame.push_back((unsigned char)((messageLen >> 8) & 0xFF));
        frame.push_back((unsigned char)(messageLen & 0xFF));
    }
    else
    {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--)
        {
            frame.push_back((unsigned char)((messageLen >> (i * 8)) & 0xFF));
        }
    }
    
    frame.insert(frame.end(), message.begin(), message.end());
    send(socket, (char*)frame.data(), (int)frame.size(), 0);
}

std::string WebSocketHandler::ReceiveWebSocketFrame(SOCKET socket)
{
    unsigned char buffer[4096];
    int bytesReceived = recv(socket, (char*)buffer, sizeof(buffer), 0);
    
    if (bytesReceived < 2)
        return "";
    
    bool masked = (buffer[1] & 0x80) != 0;
    size_t payloadLen = buffer[1] & 0x7F;
    size_t offset = 2;
    
    if (payloadLen == 126)
    {
        if (bytesReceived < 4)
            return "";
        payloadLen = (buffer[2] << 8) | buffer[3];
        offset = 4;
    }
    else if (payloadLen == 127)
    {
        if (bytesReceived < 10)
            return "";
        payloadLen = 0;
        for (int i = 0; i < 8; i++)
        {
            payloadLen = (payloadLen << 8) | buffer[2 + i];
        }
        offset = 10;
    }
    
    unsigned char maskingKey[4] = {0};
    if (masked)
    {
        if (bytesReceived < (int)(offset + 4))
            return "";
        memcpy(maskingKey, buffer + offset, 4);
        offset += 4;
    }
    
    if (bytesReceived < (int)(offset + payloadLen))
        return "";
    
    std::string payload((char*)buffer + offset, payloadLen);
    
    if (masked)
    {
        for (size_t i = 0; i < payloadLen; i++)
        {
            payload[i] ^= maskingKey[i % 4];
        }
    }
    return payload;
}

