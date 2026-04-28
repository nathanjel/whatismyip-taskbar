#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

constexpr UINT WM_TRAY_ICON = WM_APP + 1;
constexpr UINT WM_NETWORK_CHANGED = WM_APP + 2;

constexpr UINT_PTR ID_TRAY_ICON = 1;
constexpr UINT ID_MENU_COPY_IP = 1001;
constexpr UINT ID_MENU_EXIT = 1002;
