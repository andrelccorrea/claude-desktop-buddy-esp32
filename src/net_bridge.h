#pragma once
// ============================================================
// net_bridge.h — the bridge protocol over Wi-Fi (TCP), for a host bridge
// such as cyd-claude-buddy's buddyd.
//
// Wi-Fi credentials and a 32-byte shared secret are provisioned over USB
// only ({"cmd":"wifi",...}, handled in data.h). When they are present the
// board joins the network, skips BLE, and listens on NET_PORT; mDNS
// announces it as claude-buddy.local.
//
// Plain TCP has no confidentiality, so the link is authenticated instead:
//   host  -> {"hello":"<nonce_h>"}
//   board -> {"hello":"<nonce_b>","mac":HMAC(secret,"board|nh|nb")}
//   host  -> {"mac":HMAC(secret,"host|nh|nb")}
// Both sides then derive key = HMAC(secret,"key|nh|nb") and prefix every
// line with 32 hex chars of HMAC(key,"<dir>|<seq>|<json>"), dir "h" for
// host->board and "b" for board->host. A line that fails the check drops
// the connection, so nobody on the LAN can inject a permission decision.
// ============================================================
#include <Arduino.h>

#define NET_PORT 7777
#define NET_SECRET_HEX_LEN 64

bool netConfigured();
void netInit();
// Calls onLine(json) for every authenticated line.
void netPoll(void (*onLine)(const char* json));
void netWrite(const char* json);
bool netActive();
void netProvision(const char* ssid, const char* pass, const char* secretHex);
