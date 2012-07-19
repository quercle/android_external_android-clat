/*
 * Copyright 2011 Daniel Drown
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * clatd.c - tun interface setup and main event loop
 */
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <linux/icmp.h>

#include <linux/capability.h>
#include <linux/prctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/if_ether.h>

#include <private/android_filesystem_config.h>

#include "ipv4.h"
#include "ipv6.h"
#include "clatd.h"
#include "config.h"
#include "logging.h"
#include "setif.h"
#include "setroute.h"
#include "mtu.h"
#include "getaddr.h"
#include "dump.h"

#define DEVICENAME "clat"

int forwarding_fd = -1;
volatile sig_atomic_t running = 1;

/* function: set_forwarding
 * enables/disables ipv6 forwarding
 */
void set_forwarding(int fd, const char *setting) {
  /* we have to forward packets from the WAN to the tun interface */
  if(write(fd, setting, strlen(setting)) < 0) {
    logmsg(ANDROID_LOG_FATAL,"set_forwarding(%s) failed: %s", setting, strerror(errno));
    exit(1);
  }
}

/* function: set_accept_ra
 * accepts IPv6 RAs on all interfaces, even when forwarding is on
 */
void set_accept_ra() {
  int fd, i;
  const char *interface_names[] = {"wlan0","default",NULL};
  const char ipv6_sysctl[] = "/proc/sys/net/ipv6/conf/";
  const char accept_ra[] = "/accept_ra";

  for(i = 0; interface_names[i]; i++) {
    ssize_t sysctl_path_len = strlen(ipv6_sysctl)+strlen(interface_names[i])+strlen(accept_ra)+1;
    char *sysctl_path = malloc(sysctl_path_len);
    if(!sysctl_path) {
      logmsg(ANDROID_LOG_FATAL,"set_accept_ra: malloc failed");
      exit(1);
    }
    snprintf(sysctl_path, sysctl_path_len, "%s%s%s", ipv6_sysctl, interface_names[i], accept_ra);

    fd = open(sysctl_path, O_RDWR);
    free(sysctl_path);
    if(fd < 0) {
      continue;
    }
    if(write(fd, "2\n", 2) < 0) {
      logmsg(ANDROID_LOG_WARN,"write to (%s)accept_ra failed: %s",interface_names[i],strerror(errno));
    }
    close(fd);
  }
}

/* function: got_sigterm
 * signal handler: mark it time to clean up
 */
void got_sigterm(int signal) {
  running = 0;
}

/* function: tun_open
 * tries to open the tunnel device
 */
int tun_open() {
  int fd;

  fd = open("/dev/tun", O_RDWR);
  if(fd < 0) {
    fd = open("/dev/net/tun", O_RDWR);
  }

  return fd;
}

/* function: tun_alloc
 * creates a tun interface and names it
 * dev - the name for the new tun device
 */
int tun_alloc(char *dev, int fd) {
  struct ifreq ifr;
  int err;

  memset(&ifr, 0, sizeof(ifr));

  ifr.ifr_flags = IFF_TUN;
  if( *dev ) {
    strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ-1] = '\0';
  }

  if( (err = ioctl(fd, TUNSETIFF, (void *) &ifr)) < 0 ){
    close(fd);
    return err;
  }
  strcpy(dev, ifr.ifr_name);
  return 0;
}

/* function: deconfigure_tun_ipv6
 * removes the ipv6 route
 * device - the clat device name
 */
void deconfigure_tun_ipv6(const char *device) {
  int status;

  status = if_route(device, AF_INET6, &Global_Clatd_Config.ipv6_local_subnet,
      128, NULL, 1, 0, ROUTE_DELETE);
  if(status < 0) {
    logmsg(ANDROID_LOG_WARN,"deconfigure_tun_ipv6/if_route(6) failed: %s",strerror(-status));
  }
}

/* function: configure_tun_ipv6
 * configures the ipv6 route
 * note: routes a /128 out of the (assumed routed to us) /64 to the CLAT interface
 * device - the clat device name to configure
 */
void configure_tun_ipv6(const char *device) {
  struct in6_addr local_nat64_prefix_6;
  int status;

  status = if_route(device, AF_INET6, &Global_Clatd_Config.ipv6_local_subnet,
      128, NULL, 1, 0, ROUTE_CREATE);
  if(status < 0) {
    logmsg(ANDROID_LOG_FATAL,"configure_tun_ipv6/if_route(6) failed: %s",strerror(-status));
    exit(1);
  }
}

/* function: interface_poll
 * polls the uplink network interface for address changes
 */
void interface_poll(const char *tun_device) {
  union anyip *interface_ip;

  interface_ip = getinterface_ip(Global_Clatd_Config.default_pdp_interface, AF_INET6);
  if(!interface_ip) {
    logmsg(ANDROID_LOG_WARN,"unable to find an ipv6 ip on interface %s",Global_Clatd_Config.default_pdp_interface);
    return;
  }

  config_generate_local_ipv6_subnet(&interface_ip->ip6);

  if(!IN6_ARE_ADDR_EQUAL(&interface_ip->ip6, &Global_Clatd_Config.ipv6_local_subnet)) {
    char from_addr[INET6_ADDRSTRLEN], to_addr[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &Global_Clatd_Config.ipv6_local_subnet, from_addr, sizeof(from_addr));
    inet_ntop(AF_INET6, &interface_ip->ip6, to_addr, sizeof(to_addr));
    logmsg(ANDROID_LOG_WARN, "clat subnet changed from %s to %s", from_addr, to_addr);

    // remove old route
    deconfigure_tun_ipv6(tun_device);

    // add new route, start translating packets to the new prefix
    memcpy(&Global_Clatd_Config.ipv6_local_subnet, &interface_ip->ip6, sizeof(struct in6_addr));
    configure_tun_ipv6(tun_device);
  }

  free(interface_ip);
}

/* function: configure_tun_ip
 * configures the ipv4 and ipv6 addresses on the tunnel interface
 * device - the clat device name to configure
 */
void configure_tun_ip(const char *device) {
  struct in_addr default_4;
  int status;

  default_4.s_addr = INADDR_ANY;

  if((status = if_up(device, Global_Clatd_Config.mtu)) < 0) {
    logmsg(ANDROID_LOG_FATAL,"configure_tun_ip/if_up failed: %s",strerror(-status));
    exit(1);
  }
  status = add_address(device, AF_INET, &Global_Clatd_Config.ipv4_local_subnet,
      32, &Global_Clatd_Config.ipv4_local_subnet);
  if(status < 0) {
    logmsg(ANDROID_LOG_FATAL,"configure_tun_ip/if_address(4) failed: %s",strerror(-status));
    exit(1);
  }

  configure_tun_ipv6(device);

  /* setup default ipv4 route */
  status = if_route(device, AF_INET, &default_4, 0, NULL, 1, Global_Clatd_Config.ipv4mtu, ROUTE_REPLACE);
  if(status < 0) {
    logmsg(ANDROID_LOG_FATAL,"configure_tun_ip/if_route failed: %s",strerror(-status));
    exit(1);
  }
}

/* function: drop_root
 * drops root privs but keeps the needed capability
 */
void drop_root() {
  gid_t groups[] = { AID_INET };
  if(setgroups(sizeof(groups)/sizeof(groups[0]), groups) < 0) {
    logmsg(ANDROID_LOG_FATAL,"drop_root/setgroups failed: %s",strerror(errno));
    exit(1);
  }

  prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);

  if(setgid(AID_CLATD) < 0) {
    logmsg(ANDROID_LOG_FATAL,"drop_root/setgid failed: %s",strerror(errno));
    exit(1);
  }
  if(setuid(AID_CLATD) < 0) {
    logmsg(ANDROID_LOG_FATAL,"drop_root/setuid failed: %s",strerror(errno));
    exit(1);
  }

  struct __user_cap_header_struct header;
  struct __user_cap_data_struct cap;
  memset(&header, 0, sizeof(header));
  memset(&cap, 0, sizeof(cap));

  header.version = _LINUX_CAPABILITY_VERSION;
  header.pid = 0; // 0 = change myself
  cap.effective = cap.permitted = (1 << CAP_NET_ADMIN);

  if(capset(&header, &cap) < 0) {
    logmsg(ANDROID_LOG_FATAL,"drop_root/capset failed: %s",strerror(errno));
    exit(1);
  }
}

/* function: configure_interface
 * reads the configuration and applies it to the interface
 * uplink_interface - network interface to use to reach the ipv6 internet
 * plat_prefix      - PLAT prefix to use
 * fd               - file descriptor to tun device
 * device           - (in) requested device name (out) allocated device name
 */
void configure_interface(const char *uplink_interface, const char *plat_prefix, int fd, char *device) {
  int error;

  if(!read_config("/system/etc/clatd.conf", uplink_interface, plat_prefix)) {
    logmsg(ANDROID_LOG_FATAL,"read_config failed");
    exit(1);
  }

  if(Global_Clatd_Config.mtu < 0) {
    Global_Clatd_Config.mtu = getifmtu(Global_Clatd_Config.default_pdp_interface);
    logmsg(ANDROID_LOG_WARN,"ifmtu=%d",Global_Clatd_Config.mtu);
  }
  if(Global_Clatd_Config.mtu < 1280) {
    logmsg(ANDROID_LOG_WARN,"mtu too small = %d", Global_Clatd_Config.mtu);
    Global_Clatd_Config.mtu = 1280;
  }

  if(Global_Clatd_Config.ipv4mtu < 0 || (Global_Clatd_Config.ipv4mtu > Global_Clatd_Config.mtu - 20)) {
    Global_Clatd_Config.ipv4mtu = Global_Clatd_Config.mtu-20;
    logmsg(ANDROID_LOG_WARN,"ipv4mtu now set to = %d",Global_Clatd_Config.ipv4mtu);
  }

  error = tun_alloc(device, fd);
  if(error < 0) {
    logmsg(ANDROID_LOG_FATAL,"tun_alloc failed: %s",strerror(errno));
    exit(1);
  }

  configure_tun_ip(device);
}

/* function: packet_handler
 * takes a tun header and a packet and sends it down the stack
 * tun_fd     - tun file descriptor
 * tun_header - tun header
 * packet     - packet
 * packetsize - size of packet
 */
void packet_handler(int tun_fd, struct tun_pi *tun_header, const char *packet, size_t packetsize) {
  tun_header->proto = ntohs(tun_header->proto);

  if(tun_header->flags != 0) {
    logmsg(ANDROID_LOG_WARN,"packet_handler: unexpected flags = %d", tun_header->flags);
  }

  if(tun_header->proto == ETH_P_IP) {
    ip_packet(tun_fd,packet,packetsize);
  } else if(tun_header->proto == ETH_P_IPV6) {
    ipv6_packet(tun_fd,packet,packetsize);
  } else {
    logmsg(ANDROID_LOG_WARN,"packet_handler: unknown packet type = %x",tun_header->proto);
  }
}

/* function: read_packet
 * reads a packet from the tunnel fd and passes it down the stack
 * tun_fd - tun file descriptor
 */
void read_packet(int tun_fd) {
  ssize_t readlen;
  char packet[PACKETLEN];

  // in case something ignores the packet length
  memset(packet, 0, PACKETLEN);

  readlen = read(tun_fd,packet,PACKETLEN);

  if(readlen < 0) {
    logmsg(ANDROID_LOG_WARN,"read_packet/read error: %s", strerror(errno));
    return;
  } else if(readlen == 0) {
    logmsg(ANDROID_LOG_WARN,"read_packet/tun interface removed");
    running = 0;
  } else {
    struct tun_pi tun_header;
    ssize_t header_size = sizeof(struct tun_pi);

    if(readlen < header_size) {
      logmsg(ANDROID_LOG_WARN,"read_packet/short read: got %ld bytes", readlen);
      return;
    }

    memcpy(&tun_header, packet, header_size);

    packet_handler(tun_fd, &tun_header, packet+header_size, readlen-header_size);
  }
}

/* function: event_loop
 * reads packets from the tun network interface and passes them down the stack
 * tun_fd     - file descriptor for the tun network interface
 * tun_device - tun device interface name
 */
void event_loop(int tun_fd, const char *tun_device) {
  time_t last_interface_poll;
  struct pollfd wait_fd;

  // start the poll timer
  last_interface_poll = time(NULL);

  wait_fd.fd = tun_fd;
  wait_fd.events = POLLIN;
  wait_fd.revents = 0;

  while(running) {
    if(poll(&wait_fd, 1, NO_TRAFFIC_INTERFACE_POLL_FREQUENCY*1000) == -1) {
      if(errno != EINTR) {
        logmsg(ANDROID_LOG_WARN,"event_loop/poll returned an error: %s",strerror(errno));
      }
    } else {
      if((wait_fd.revents & POLLIN) != 0) {
        read_packet(tun_fd);
      }
    }

    time_t now = time(NULL);
    if(last_interface_poll < (now - INTERFACE_POLL_FREQUENCY)) {
      interface_poll(tun_device);
      last_interface_poll = now;
    }
  }
}

/* function: print_help
 * in case the user is running this on the command line
 */
void print_help() {
  printf("android-clat arguments:\n");
  printf("-i [uplink interface]\n");
  printf("-p [plat prefix]\n");
}

/* function: main
 * allocate and setup the tun device, then run the event loop
 */
int main(int argc, char **argv) {
  int tun_fd;
  char device[IFNAMSIZ] = DEVICENAME;
  int opt;
  char *uplink_interface = NULL, *plat_prefix = NULL;

  while((opt = getopt(argc, argv, "i:p:h")) != -1) {
    switch(opt) {
      case 'i':
        uplink_interface = optarg;
        break;
      case 'p':
        plat_prefix = optarg;
        break;
      case 'h':
      default:
        print_help();
        exit(1);
        break;
    }
  }

  if(uplink_interface == NULL) {
    logmsg(ANDROID_LOG_FATAL,"clatd called without an interface");
    printf("I need an interface\n");
    exit(1);
  }

  // open the tunnel device before dropping privs
  tun_fd = tun_open();
  if(tun_fd < 0) {
    logmsg(ANDROID_LOG_FATAL,"tun_open failed: %s",strerror(errno));
    exit(1);
  }

  // open the forwarding configuration before dropping privs
  forwarding_fd = open("/proc/sys/net/ipv6/conf/all/forwarding", O_RDWR);
  if(forwarding_fd < 0) {
    logmsg(ANDROID_LOG_FATAL,"open /proc/sys/net/ipv6/conf/all/forwarding failed: %s",strerror(errno));
    exit(1);
  }

  // forwarding slows down IPv6 config while transitioning to wifi
  set_accept_ra();

  // protect against forwarding being on when bringing up cell network
  // interface - RA is ignored when forwarding is on
  set_default_ipv6_route(uplink_interface);

  // run under a regular user
  drop_root();

  if(signal(SIGTERM, got_sigterm) == SIG_ERR) {
    logmsg(ANDROID_LOG_FATAL, "sigterm handler failed: %s", strerror(errno));
    exit(1);
  }

  configure_interface(uplink_interface, plat_prefix, tun_fd, device);

  set_forwarding(forwarding_fd,"1\n");

  event_loop(tun_fd,device); // event_loop returns if someone sets the tun interface down manually

  set_forwarding(forwarding_fd,"0\n");

  return 0;
}
