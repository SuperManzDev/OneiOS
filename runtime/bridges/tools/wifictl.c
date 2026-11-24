// SPDX-License-Identifier: MIT
// wifictl: the Wi-Fi bridge on the phone without xnuhost or a guest (docs/hardware.md section 3). Run as root (su):
//   wifictl status [IFACE]           read only: the interface's address, the supplicant's AIDL service, its station
//                                    interface, its networks' ids, the signal poll. Safe beside One UI.
//   wifictl scan [SECONDS [IFACE]]   one nl80211 scan (default 6 s): each network's name, access point, channel
//                                    frequency, signal and security. Safe beside One UI (its own scans do the same).
//   wifictl join SSID [PASSPHRASE [SECONDS]]
//                                    the whole host path with this tool as the guest: takes wlan0 as a session does
//                                    (the supplicant's networks removed, IPv4 addresses removed), scans, joins,
//                                    asks for an address by DHCP and prints the offer, stays SECONDS (default 5),
//                                    leaves and gives wlan0 back. It takes the Wi-Fi from One UI while it runs: One
//                                    UI joins its network again by itself after it. Not during a session.
//   wifictl restore FILE             wlan0's settings back from a session's wifi.state (after a killed xnuhost)
//   wifictl tap TAP [DIR]            the guest's en0 through crosvm's virtio-net card (`phone vm-boot`, an Image of
//                                    xnuloader-crosvm --wifi): takes wlan0 as a session does (its state in DIR/wifi.state,
//                                    default /data/local/tmp), makes TAP a persistent tap and writes the address the guest
//                                    must have (wlan0's) to DIR/wifi.mac for crosvm's --net tap-name=TAP,mac=..., then
//                                    carries frames between TAP and the bridge as xnuhost's virtio-net does: the guest's
//                                    frames and the station's messages to br_wlan_send, the bridge's frames for the guest
//                                    out of TAP into crosvm. It ends on SIGTERM or SIGINT, or when crosvm lets go of TAP
//                                    (its carrier goes down); then it leaves the network, gives wlan0 back and removes TAP.
// Each ends with "ok" or "failed".
#define _GNU_SOURCE
#include "bridges/bridges.h"
#include "../wifi/supplicant.h"
#include "../wifi/wifi.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#ifndef PACKET_IGNORE_OUTGOING
#define PACKET_IGNORE_OUTGOING 23
#endif

static void say(int level, const char *line) { printf("  %s%s\n", level == 0 ? "error: " : "", line); }

static const char *security_text(const uint8_t *ies, size_t n, uint16_t capability)
{
  static char text[64];
  br_wifi_security s;
  br_wifi_security_parse(ies, n, &s);
  if (!s.rsn && !s.wpa) return capability & 0x10 ? "WEP" : "open";
  snprintf(text, sizeof text, "%s%s%s%s%s%s", s.sae ? "WPA3 " : "", s.psk || s.psk_sha256 ? (s.rsn ? "WPA2 " : "WPA ") : "", s.eap ? "802.1X " : "",
           s.owe ? "OWE " : "", s.pmf_required ? "PMF required " : "", s.tkip && !s.ccmp ? "TKIP only" : "");
  return text;
}

static void print_bss(void *ctx, const struct ow_bss *b, const uint8_t *ies, size_t n)
{
  (void)ctx;
  char name[33] = "";
  for (unsigned i = 0; i < b->ssid_length && i < 32; i++) name[i] = b->ssid[i] >= 0x20 && b->ssid[i] < 0x7f ? (char)b->ssid[i] : '?';
  printf("  %02x:%02x:%02x:%02x:%02x:%02x %4u MHz %4d dBm  %-22s \"%s\"%s\n", b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5],
         b->freq_mhz, b->signal_dbm, security_text(ies, n, b->capability), name, b->ssid_length ? "" : " (hidden)");
}

static int scan(int argc, char **argv)
{
  unsigned seconds = argc > 2 ? (unsigned)atoi(argv[2]) : 6;
  int n = br_wlan_scan_once(argc > 3 ? argv[3] : "wlan0", seconds * 1000, print_bss, NULL);
  if (n < 0) { printf("scan: %s\n", strerror(-n)); return 1; }
  printf("scan: %d networks\n", n);
  return 0;
}

static int status(int argc, char **argv)
{
  const char *ifname = argc > 2 ? argv[2] : "wlan0";
  char path[128], text[64] = "";
  snprintf(path, sizeof path, "/sys/class/net/%s/address", ifname);
  FILE *f = fopen(path, "r");
  if (f) { if (!fgets(text, sizeof text, f)) text[0] = 0; fclose(f); }
  text[strcspn(text, "\n")] = 0;
  printf("%s: %s\n", ifname, *text ? text : "no such interface");
  br_supplicant *s = br_supplicant_open(ifname, 0, NULL, NULL);       // a look: nothing registered, added or removed
  if (!s) return 1;
  uint8_t mac[6];
  int32_t ids[32], poll[5];
  unsigned n = 32;
  int rc = br_supplicant_status(s, mac, ids, &n, poll);
  if (!rc) {
    printf("supplicant: service %s, station interface %s, address %02x:%02x:%02x:%02x:%02x:%02x, %u network(s):", SUP_SERVICE, ifname, mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5], n);
    for (unsigned i = 0; i < n; i++) printf(" %d", ids[i]);
    printf("\nsignal poll: %s", poll[4] ? "" : "none (not joined)\n");
    if (poll[4]) printf("%d dBm, tx %d Mb/s, rx %d Mb/s, %d MHz\n", poll[1], poll[2], poll[3], poll[4]);
  }
  br_supplicant_close(s);
  return rc ? 1 : 0;
}

// ---- join: this tool as the guest's driver ----
static br_net *wlan;
static uint8_t our_mac[6];
static volatile uint32_t link_state, radio_state, scan_count, join_status = ~0u, join_code;
static volatile bool scan_done, offered;
static volatile unsigned frames;
static uint8_t offer_ip[4], offer_server[4];

static void from_host(void *ctx, const uint8_t *frame, size_t length)
{
  (void)ctx;
  if (br_wifi_ethertype(frame, length) != OW_ETHERTYPE) {
    frames++;
    // A DHCP reply for us: IPv4, UDP 67 -> 68, BOOTREPLY with our transaction.
    if (length >= 14 + 20 + 8 + 240 && frame[12] == 0x08 && frame[13] == 0x00 && frame[23] == 17) {
      const uint8_t *udp = frame + 14 + (frame[14] & 15) * 4;
      if (udp + 8 + 240 <= frame + length && udp[0] == 0 && udp[1] == 67 && udp[2] == 0 && udp[3] == 68 && udp[8] == 2 &&
          !memcmp(udp + 12, "\x4f\x57\x49\x46", 4) && !offered) {
        memcpy(offer_ip, udp + 8 + 16, 4);
        memcpy(offer_server, frame + 26, 4);
        offered = true;
      }
    }
    return;
  }
  struct ow_header h;
  if (length < 14 + sizeof h) return;
  memcpy(&h, frame + 14, sizeof h);
  const uint8_t *p = frame + 14 + sizeof h;
  if (h.type == OW_STATE && h.length >= sizeof(struct ow_state)) {
    struct ow_state s;
    memcpy(&s, p, sizeof s);
    if (s.link != link_state || s.radio != radio_state)
      printf("  state: radio %u, link %u%s, %d dBm, %u Mb/s, %u MHz, reason %u\n", s.radio, s.link, s.link == OW_LINK_UP ? " (up)" : "", s.rssi_dbm,
             s.tx_rate_mbps, s.bss.freq_mhz, s.reason);
    link_state = s.link;
    radio_state = s.radio;
  } else if (h.type == OW_BSS && h.length >= sizeof(struct ow_bss)) {
    struct ow_bss b;
    memcpy(&b, p, sizeof b);
    print_bss(NULL, &b, p + sizeof b, h.length - sizeof b < b.ie_length ? h.length - sizeof b : b.ie_length);
  } else if (h.type == OW_SCAN_DONE && h.length >= sizeof(struct ow_scan_done)) {
    struct ow_scan_done d;
    memcpy(&d, p, sizeof d);
    printf("  scan done: %u networks (error %d)\n", d.count, d.error);
    scan_count = d.count;
    scan_done = true;
  } else if (h.type == OW_JOIN_RESULT && h.length >= sizeof(struct ow_join_result)) {
    struct ow_join_result r;
    memcpy(&r, p, sizeof r);
    join_code = r.code;
    join_status = r.status;
  }
}

static void to_host(uint16_t type, const void *payload, uint16_t n)
{
  uint8_t frame[256];
  struct ow_header h = { OW_MAGIC, type, n };
  memcpy(frame, ow_address, 6);
  memcpy(frame + 6, our_mac, 6);
  frame[12] = (uint8_t)(OW_ETHERTYPE >> 8);
  frame[13] = (uint8_t)OW_ETHERTYPE;
  memcpy(frame + 14, &h, sizeof h);
  if (n) memcpy(frame + 14 + sizeof h, payload, n);
  (void)br_wlan_send(wlan, frame, 14 + sizeof h + n);
}

static uint16_t ip_sum(const uint8_t *p, size_t n)
{
  uint32_t sum = 0;
  for (size_t i = 0; i + 1 < n; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t)~sum;
}

static void dhcp_discover(void)
{
  uint8_t f[14 + 20 + 8 + 300];
  memset(f, 0, sizeof f);
  memset(f, 0xff, 6);
  memcpy(f + 6, our_mac, 6);
  f[12] = 0x08;
  uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8;
  ip[0] = 0x45; ip[2] = (uint8_t)((sizeof f - 14) >> 8); ip[3] = (uint8_t)(sizeof f - 14); ip[8] = 64; ip[9] = 17;
  memset(ip + 16, 0xff, 4);
  uint16_t sum = ip_sum(ip, 20);
  ip[10] = (uint8_t)(sum >> 8); ip[11] = (uint8_t)sum;
  udp[1] = 68; udp[3] = 67; udp[4] = (uint8_t)((8 + 300) >> 8); udp[5] = (uint8_t)(8 + 300);
  b[0] = 1; b[1] = 1; b[2] = 6;
  memcpy(b + 4, "\x4f\x57\x49\x46", 4);                // the transaction
  b[10] = 0x80;                                        // broadcast the reply
  memcpy(b + 28, our_mac, 6);
  memcpy(b + 236, "\x63\x82\x53\x63", 4);
  memcpy(b + 240, "\x35\x01\x01\x37\x03\x01\x03\x06\xff", 9);    // DISCOVER; ask for the mask, the router, DNS
  int rc = br_wlan_send(wlan, f, sizeof f);
  if (rc) printf("  DHCP discover not sent: %s\n", strerror(-rc));
}

static int join(int argc, char **argv)
{
  const char *ssid = argv[2], *pass = argc > 3 ? argv[3] : "";
  unsigned stay = argc > 4 ? (unsigned)atoi(argv[4]) : 5;
  if (strlen(ssid) > 32 || (*pass && (strlen(pass) < 8 || strlen(pass) > 63))) { printf("a name of at most 32 bytes, a passphrase of 8 to 63\n"); return 1; }
  br_net_open_fn open_fn = br_wlan_prepare("wlan0", "/data/local/tmp", our_mac);     // /data/local/tmp/wifi.state
  if (!open_fn || !(wlan = open_fn(NULL, from_host, NULL))) return 1;
  int rc = 1;
  struct ow_hello hello = { OW_VERSION };
  to_host(OW_HELLO, &hello, sizeof hello);
  for (unsigned i = 0; i < 100 && !radio_state; i++) usleep(100000);
  if (!radio_state) { printf("the bridge did not answer\n"); goto out; }
  sleep(2);                                            // the supplicant: found, or started by the lookup
  to_host(OW_SCAN, NULL, 0);
  for (unsigned i = 0; i < 120 && !scan_done; i++) usleep(100000);
  struct ow_join j = { .security = *pass ? OW_SECURITY_AUTO : OW_SECURITY_OPEN };
  j.ssid_length = (uint8_t)strlen(ssid);
  memcpy(j.ssid, ssid, j.ssid_length);
  j.key_length = (uint8_t)strlen(pass);
  memcpy(j.key, pass, j.key_length);
  to_host(OW_JOIN, &j, sizeof j);
  memset(&j, 0, sizeof j);
  for (unsigned i = 0; i < 320 && join_status == ~0u; i++) usleep(100000);
  if (join_status != OW_JOIN_OK) { printf("join: %s (status %u, 802.11 code %u)\n", join_status == ~0u ? "no result in 32 s" : "failed", join_status, join_code); goto out; }
  printf("join: joined\n");
  for (unsigned i = 0; i < 4 && !offered; i++) { dhcp_discover(); sleep(2); }
  if (offered) printf("DHCP: %u.%u.%u.%u offered by %u.%u.%u.%u\n", offer_ip[0], offer_ip[1], offer_ip[2], offer_ip[3], offer_server[0], offer_server[1],
                      offer_server[2], offer_server[3]);
  else printf("DHCP: no offer in 8 s (%u other frames came from the network)\n", frames);
  sleep(stay);
  rc = offered ? 0 : 1;
out:
  to_host(OW_LEAVE, NULL, 0);
  usleep(500000);
  br_wlan_close();
  return rc;
}

// ---- tap: crosvm's card ---------------------------------------------------------------------------
static volatile sig_atomic_t tap_stop;
static int tap_socket = -1, tap_index;
static unsigned long long tap_to_guest, tap_to_guest_failed, tap_from_guest, tap_from_guest_refused;

static void tap_signal(int sig) { (void)sig; tap_stop = 1; }

// A frame of the bridge's for the guest (its threads): out of TAP, so crosvm reads it into the card's receive queue.
static void tap_rx(void *ctx, const uint8_t *frame, size_t length)
{
  (void)ctx;
  struct sockaddr_ll to = { .sll_family = AF_PACKET, .sll_ifindex = tap_index, .sll_halen = 6 };
  if (length >= 6) memcpy(to.sll_addr, frame, 6);
  if (tap_socket >= 0 && sendto(tap_socket, frame, length, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)length)
    __atomic_fetch_add(&tap_to_guest, 1, __ATOMIC_RELAXED);
  else
    __atomic_fetch_add(&tap_to_guest_failed, 1, __ATOMIC_RELAXED);
}

// TAP as a persistent tap (persist 1), or removed (persist 0). The fd is closed at once: crosvm attaches by the name.
static int tap_persist(const char *name, int persist)
{
  int fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
  if (fd < 0) return -errno;
  struct ifreq r;
  memset(&r, 0, sizeof r);
  r.ifr_flags = IFF_TAP | IFF_NO_PI;
  snprintf(r.ifr_name, sizeof r.ifr_name, "%s", name);
  int rc = ioctl(fd, TUNSETIFF, &r) || ioctl(fd, TUNSETPERSIST, persist) ? -errno : 0;
  close(fd);
  return rc;
}

static int tap_flags_up(const char *name)
{
  int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (s < 0) return -errno;
  struct ifreq r;
  memset(&r, 0, sizeof r);
  snprintf(r.ifr_name, sizeof r.ifr_name, "%s", name);
  int rc = ioctl(s, SIOCGIFFLAGS, &r);
  if (!rc && !(r.ifr_flags & IFF_UP)) { r.ifr_flags |= IFF_UP; rc = ioctl(s, SIOCSIFFLAGS, &r); }
  rc = rc ? -errno : 0;
  close(s);
  return rc;
}

// /sys/class/net/TAP/carrier: 1 while crosvm holds the tap, 0 before and after; -1 when unreadable.
static int tap_carrier(const char *name)
{
  char path[96], c = 0;
  snprintf(path, sizeof path, "/sys/class/net/%s/carrier", name);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t n = read(fd, &c, 1);
  close(fd);
  return n == 1 ? c == '1' : -1;
}

static int tap(int argc, char **argv)
{
  const char *name = argv[2], *dir = argc > 3 ? argv[3] : "/data/local/tmp";
  if (!*name || strlen(name) >= IFNAMSIZ || strchr(name, '/')) { printf("a tap name of 1 to %d bytes\n", IFNAMSIZ - 1); return 1; }
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = tap_signal;                                // no SA_RESTART: poll and recvfrom end with EINTR
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  uint8_t mac[6];
  br_net_open_fn open_fn = br_wlan_prepare("wlan0", dir, mac);
  if (!open_fn) return 1;                                    // wlan0 untouched (the bridge said why)
  int rc = 1;
  br_net *wlan = NULL;
  char path[256], text[64];
  bool made = false;
  int r = tap_persist(name, 1);
  if (r) { printf("tap: %s not made: %s\n", name, strerror(-r)); goto out; }
  made = true;
  snprintf(path, sizeof path, "/proc/sys/net/ipv6/conf/%s/disable_ipv6", name);    // the host's own stack stays off it
  int v6 = open(path, O_WRONLY | O_CLOEXEC);
  if (v6 >= 0) { (void)!write(v6, "1", 1); close(v6); }
  if ((r = tap_flags_up(name))) { printf("tap: %s not up: %s\n", name, strerror(-r)); goto out; }
  tap_index = (int)if_nametoindex(name);
  tap_socket = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
  struct sockaddr_ll at = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = tap_index };
  int one = 1;
  if (!tap_index || tap_socket < 0 || bind(tap_socket, (struct sockaddr *)&at, sizeof at)) {
    printf("tap: AF_PACKET on %s: %s\n", name, strerror(errno));
    goto out;
  }
  (void)setsockopt(tap_socket, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof one);   // what goes out to crosvm
  if (!(wlan = open_fn(NULL, tap_rx, NULL))) goto out;
  // the address last: vm-boot starts crosvm when it reads it
  snprintf(text, sizeof text, "%02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(path, sizeof path, "%s/wifi.mac", dir);
  int mf = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (mf < 0 || write(mf, text, strlen(text)) != (ssize_t)strlen(text)) { printf("tap: %s: %s\n", path, strerror(errno)); if (mf >= 0) close(mf); goto out; }
  close(mf);
  printf("tap: %s is crosvm's card, the guest's address %.17s (wlan0's); frames carried until crosvm lets go of it\n", name, text);
  bool held = false;
  unsigned long long said = ~0ull;
  struct timespec last;
  clock_gettime(CLOCK_MONOTONIC, &last);
  uint8_t frame[2048];
  while (!tap_stop) {
    struct pollfd p = { tap_socket, POLLIN, 0 };
    int n = poll(&p, 1, 500);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - last.tv_sec >= 30) {                     // a line every 30 s while frames move
      last = now;
      const unsigned long long moved = tap_from_guest + __atomic_load_n(&tap_to_guest, __ATOMIC_RELAXED);
      if (moved != said) {
        said = moved;
        printf("tap: %llu frames to the guest (%llu not sent), %llu from it (%llu refused by the bridge)\n",
               __atomic_load_n(&tap_to_guest, __ATOMIC_RELAXED), __atomic_load_n(&tap_to_guest_failed, __ATOMIC_RELAXED),
               tap_from_guest, tap_from_guest_refused);
      }
    }
    const int carrier = tap_carrier(name);
    if (carrier == 1) held = true;
    else if (held) { printf("tap: crosvm let go of %s\n", name); break; }
    if (n < 0) { if (errno == EINTR) continue; printf("tap: poll: %s\n", strerror(errno)); break; }
    if (n == 0 || !(p.revents & POLLIN)) continue;
    struct sockaddr_ll from;
    socklen_t fl = sizeof from;
    ssize_t len = recvfrom(tap_socket, frame, sizeof frame, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (len < 0) { if (errno == EAGAIN || errno == EINTR || errno == ENETDOWN) continue; printf("tap: recvfrom: %s\n", strerror(errno)); break; }
    if (from.sll_pkttype == PACKET_OUTGOING) continue;
    if (len < 14 || len > (ssize_t)BR_WIFI_FRAME) { tap_from_guest_refused++; continue; }
    if (br_wlan_send(wlan, frame, (size_t)len) == 0) tap_from_guest++; else tap_from_guest_refused++;
  }
  rc = 0;
out:
  br_wlan_close();                                           // the network left, the supplicant and wlan0 given back
  if (tap_socket >= 0) close(tap_socket);
  tap_socket = -1;
  snprintf(path, sizeof path, "%s/wifi.mac", dir);
  unlink(path);
  if (made && (r = tap_persist(name, 0))) printf("tap: %s not removed: %s\n", name, strerror(-r));
  printf("tap: %llu frames to the guest (%llu not sent), %llu from it (%llu refused by the bridge)\n",
         __atomic_load_n(&tap_to_guest, __ATOMIC_RELAXED), __atomic_load_n(&tap_to_guest_failed, __ATOMIC_RELAXED),
         tap_from_guest, tap_from_guest_refused);
  return rc;
}

int main(int argc, char **argv)
{
  int rc = 2;
  setvbuf(stdout, NULL, _IOLBF, 0);
  br_log_sink = say;
  if (argc < 2) { fprintf(stderr, "usage: wifictl status [IFACE] | scan [SECONDS [IFACE]] | join SSID [PASSPHRASE [SECONDS]] | restore FILE | tap TAP [DIR]\n"); return 2; }
  if (!strcmp(argv[1], "status")) rc = status(argc, argv);
  else if (!strcmp(argv[1], "scan")) rc = scan(argc, argv);
  else if (!strcmp(argv[1], "join") && argc > 2) rc = join(argc, argv);
  else if (!strcmp(argv[1], "tap") && argc > 2) rc = tap(argc, argv);
  else if (!strcmp(argv[1], "restore") && argc > 2) { rc = br_wlan_restore(argv[2]); if (rc == -ENOENT) { printf("nothing to give back (no %s)\n", argv[2]); rc = 0; } else rc = rc ? 1 : 0; }
  printf("%s\n", rc == 0 ? "ok" : rc == 2 ? "unknown command" : "failed");
  return rc;
}
