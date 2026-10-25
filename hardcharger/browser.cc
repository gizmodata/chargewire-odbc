// Copyright (C) 2026 GizmoData LLC. Apache-2.0.
#include "browser.h"
#include <odbcabstraction/exceptions.h>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif
namespace driver::hardcharger {
void OpenBrowser(const std::string& url, const std::string& command) {
  if (!(url.starts_with("http://") || url.starts_with("https://")))
    throw odbcabstraction::DriverException("Invalid browser sign-in URL", "28000");
  if (command == "none") { std::fprintf(stderr, "Hardcharger sign-in: %s\n", url.c_str()); return; }
#ifdef _WIN32
  const auto result = reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", command.empty() ? url.c_str() : command.c_str(), command.empty() ? nullptr : url.c_str(), nullptr, SW_SHOWNORMAL));
  if (result <= 32) throw odbcabstraction::DriverException("Could not open the sign-in browser", "28000");
#else
#ifdef __APPLE__
  const std::string opener = command.empty() ? "/usr/bin/open" : command;
#else
  const std::string opener = command.empty() ? "xdg-open" : command;
#endif
  // No shell: the authorize URL is one argument, never executable text.
  char* args[] = {const_cast<char*>(opener.c_str()), const_cast<char*>(url.c_str()), nullptr};
  pid_t pid;
  if (posix_spawnp(&pid, opener.c_str(), nullptr, nullptr, args, environ) != 0)
    throw odbcabstraction::DriverException("Could not open the sign-in browser", "28000");
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
#endif
}
}
