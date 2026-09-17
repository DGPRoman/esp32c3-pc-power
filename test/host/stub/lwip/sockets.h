/**
 * @file sockets.h
 * @brief Host stand-in for lwIP's socket header.
 *
 * lwIP's socket API is the BSD one, which is what this server is written
 * against, so on a host it is enough to point at the real thing. Nothing is
 * reimplemented here and no socket is opened by the tests — this exists so the
 * translation unit compiles.
 */

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
