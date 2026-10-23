#pragma once

#include <string>

// Readiness notification over the sd_notify datagram protocol, for whichever process started the shell
// (a compositor or a systemd unit) and advertised a socket in NOTIFY_SOCKET.
class ServiceNotifier {
public:
  // Takes NOTIFY_SOCKET out of the environment so processes the shell launches don't inherit it.
  void takeSocketFromEnvironment();
  void notifyReady();

private:
  std::string m_socketPath;
};
