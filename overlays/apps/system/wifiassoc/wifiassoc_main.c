/****************************************************************************
 * apps/system/wifiassoc/wifiassoc_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CONFIG_SYSTEM_WIFIASSOC_BOARDINIT
#  include <sys/boardctl.h>
#endif
#include <unistd.h>

#include "netutils/netlib.h"
#include "wireless/wapi.h"

static void wifiassoc_showusage(FAR const char *progname, int exitcode)
{
  fprintf(stderr,
          "Usage: %s [-n] [-a assoc-retries] [-A assoc-delay-ms] "
          "[-r dhcp-retries] [-w dhcp-delay-ms] <ifname> <ssid> <psk>\n",
          progname);
  exit(exitcode);
}

static int wifiassoc_step(FAR const char *name, int ret)
{
  if (ret < 0)
    {
      fprintf(stderr, "association step failed: %s: %d\n", name, ret);
    }

  return ret;
}

static int wifiassoc_request(FAR const char *ifname, FAR const char *ssid,
                             FAR const char *psk, int attempts,
                             int delay_ms)
{
  int sockfd;
  int ret;
  int i;

  for (i = 1; i <= attempts; i++)
    {
      if (delay_ms > 0)
        {
          usleep(delay_ms * 1000);
        }

      sockfd = wapi_make_socket();
      if (sockfd < 0)
        {
          fprintf(stderr, "ERROR: wapi_make_socket() failed: %d\n", sockfd);
          return sockfd;
        }

      ret = wifiassoc_step("mode",
                           wapi_set_mode(sockfd, ifname,
                                         WAPI_MODE_MANAGED));
      if (ret >= 0)
        {
          ret = wifiassoc_step("wpa-version",
                               wpa_driver_wext_set_auth_param(sockfd,
                                 ifname, IW_AUTH_WPA_VERSION,
                                 IW_AUTH_WPA_VERSION_WPA2));
        }

      if (ret >= 0)
        {
          ret = wifiassoc_step("cipher",
                               wpa_driver_wext_set_auth_param(sockfd,
                                 ifname, IW_AUTH_CIPHER_PAIRWISE,
                                 IW_AUTH_CIPHER_CCMP));
        }

      if (ret >= 0)
        {
          ret = wifiassoc_step("key",
                               wpa_driver_wext_set_key_ext(sockfd, ifname,
                                 WPA_ALG_CCMP, psk, strlen(psk)));
        }

      if (ret >= 0)
        {
          ret = wifiassoc_step("essid",
                               wapi_set_essid(sockfd, ifname, ssid,
                                              WAPI_ESSID_ON));
        }

      close(sockfd);

      if (ret >= 0)
        {
          printf("Association requested for %s on attempt %d/%d\n",
                 ifname, i, attempts);
          return EXIT_SUCCESS;
        }

      fprintf(stderr, "association attempt %d/%d failed: %d\n",
              i, attempts, ret);
    }

  fprintf(stderr, "ERROR: association failed after %d attempts\n", attempts);
  return EXIT_FAILURE;
}

static int wifiassoc_renew(FAR const char *ifname, int retries, int delay_ms)
{
  struct in_addr addr;
  int ret = -ETIMEDOUT;
  int i;

  for (i = 1; i <= retries; i++)
    {
      usleep(delay_ms * 1000);

      ret = netlib_obtain_ipv4addr(ifname);
      if (ret >= 0)
        {
          ret = netlib_get_ipv4addr(ifname, &addr);
          if (ret >= 0)
            {
              printf("DHCP address: %s\n", inet_ntoa(addr));
            }

          return EXIT_SUCCESS;
        }

      fprintf(stderr, "DHCP attempt %d/%d failed\n", i, retries);
    }

  fprintf(stderr, "ERROR: DHCP failed after %d attempts\n", retries);
  return EXIT_FAILURE;
}

int main(int argc, FAR char *argv[])
{
  FAR const char *ifname;
  FAR const char *ssid;
  FAR const char *psk;
  bool dhcp = true;
  int assoc_retries = CONFIG_SYSTEM_WIFIASSOC_ASSOC_RETRIES;
  int assoc_delay_ms = CONFIG_SYSTEM_WIFIASSOC_ASSOC_DELAY_MS;
  int retries = CONFIG_SYSTEM_WIFIASSOC_DHCP_RETRIES;
  int delay_ms = CONFIG_SYSTEM_WIFIASSOC_DHCP_DELAY_MS;
  int opt;
  int ret;

  while ((opt = getopt(argc, argv, "ha:A:nr:w:")) != ERROR)
    {
      switch (opt)
        {
          case 'a':
            assoc_retries = atoi(optarg);
            break;
          case 'A':
            assoc_delay_ms = atoi(optarg);
            break;
          case 'n':
            dhcp = false;
            break;
          case 'r':
            retries = atoi(optarg);
            break;
          case 'w':
            delay_ms = atoi(optarg);
            break;
          case 'h':
            wifiassoc_showusage(argv[0], EXIT_SUCCESS);
            break;
          default:
            wifiassoc_showusage(argv[0], EXIT_FAILURE);
            break;
        }
    }

  if (argc - optind != 3 || assoc_retries < 1 || assoc_delay_ms < 0 ||
      retries < 1 || delay_ms < 0)
    {
      wifiassoc_showusage(argv[0], EXIT_FAILURE);
    }

  ifname = argv[optind];
  ssid = argv[optind + 1];
  psk = argv[optind + 2];

  if (strlen(psk) < 8 || strlen(psk) > 63)
    {
      fprintf(stderr, "ERROR: WPA2 PSK must be 8..63 characters\n");
      return EXIT_FAILURE;
    }

#ifdef CONFIG_SYSTEM_WIFIASSOC_BOARDINIT
  ret = boardctl(BOARDIOC_USER + CONFIG_SYSTEM_WIFIASSOC_BOARDINIT_ARG, 0);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: board wireless initialization failed: %d\n",
              errno);
      return EXIT_FAILURE;
    }
#endif

  ret = netlib_ifup(ifname);
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: netlib_ifup(%s) failed: %d\n", ifname, ret);
      return EXIT_FAILURE;
    }

  ret = wifiassoc_request(ifname, ssid, psk, assoc_retries,
                          assoc_delay_ms);
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  if (dhcp)
    {
      return wifiassoc_renew(ifname, retries, delay_ms);
    }

  return EXIT_SUCCESS;
}
