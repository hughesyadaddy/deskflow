/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "coordination/WakeOnLan.h"

#include "base/Log.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cctype>
#include <cerrno>
#include <cstring>

namespace deskflow::coordination {

namespace {

const uint16_t kWakeOnLanPort = 9;

void platformCloseSocket(int fd)
{
#if defined(_WIN32)
  ::closesocket(fd);
#else
  ::close(fd);
#endif
}

std::optional<uint8_t> parseHexOctet(const std::string &text, std::size_t offset)
{
  const auto hexValue = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower >= 'a' && lower <= 'f') {
      return lower - 'a' + 10;
    }
    return -1;
  };
  const int high = hexValue(text[offset]);
  const int low = hexValue(text[offset + 1]);
  if (high < 0 || low < 0) {
    return std::nullopt;
  }
  return static_cast<uint8_t>((high << 4) | low);
}

} // namespace

std::optional<MacAddress> parseMacAddress(const std::string &text)
{
  // Exactly six two-hex-digit octets: "aa:bb:cc:dd:ee:ff" is 17 chars.
  if (text.size() != 17) {
    return std::nullopt;
  }
  MacAddress mac{};
  for (std::size_t octet = 0; octet < mac.size(); ++octet) {
    const std::size_t offset = octet * 3;
    if (octet > 0) {
      const char separator = text[offset - 1];
      if (separator != ':' && separator != '-') {
        return std::nullopt;
      }
    }
    const auto value = parseHexOctet(text, offset);
    if (!value) {
      return std::nullopt;
    }
    mac[octet] = *value;
  }
  return mac;
}

std::vector<uint8_t> buildMagicPacket(const MacAddress &mac)
{
  std::vector<uint8_t> packet;
  packet.reserve(6 + 16 * mac.size());
  packet.insert(packet.end(), 6, 0xFF);
  for (int repeat = 0; repeat < 16; ++repeat) {
    packet.insert(packet.end(), mac.begin(), mac.end());
  }
  return packet;
}

bool sendWakeOnLan(const std::string &mac)
{
  const auto parsed = parseMacAddress(mac);
  if (!parsed) {
    LOG_WARN("coordination: wake-on-lan: invalid mac \"%s\"", mac.c_str());
    return false;
  }
  const auto packet = buildMagicPacket(*parsed);

  const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
  if (fd < 0) {
    LOG_WARN("coordination: wake-on-lan: socket() failed: %s", std::strerror(errno));
    return false;
  }

  int broadcast = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&broadcast), sizeof(broadcast));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(kWakeOnLanPort);
  address.sin_addr.s_addr = INADDR_BROADCAST;

  const auto sent = ::sendto(
      fd, reinterpret_cast<const char *>(packet.data()), static_cast<int>(packet.size()), 0,
      reinterpret_cast<const sockaddr *>(&address), sizeof(address)
  );
  platformCloseSocket(fd);

  if (sent != static_cast<decltype(sent)>(packet.size())) {
    LOG_WARN("coordination: wake-on-lan: sendto failed for %s: %s", mac.c_str(), std::strerror(errno));
    return false;
  }
  LOG_INFO("coordination: wake-on-lan: magic packet broadcast for %s", mac.c_str());
  return true;
}

} // namespace deskflow::coordination
