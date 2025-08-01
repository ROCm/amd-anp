//
// Copyright(C) Advanced Micro Devices, Inc. All rights reserved.
//
// You may not use this software and documentation (if any) (collectively,
// the "Materials") except in compliance with the terms and conditions of
// the Software License Agreement included with the Materials or otherwise as
// set forth in writing and signed by you and an authorized signatory of AMD.
// If you do not have a copy of the Software License Agreement, contact your
// AMD representative for a copy.
//
// You agree that you will not reverse engineer or decompile the Materials,
// in whole or in part, except as allowed by applicable law.
//
// THE MATERIALS ARE DISTRIBUTED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OR
// REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
//

#define NCCL_BUILD_RDMA_CORE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <execinfo.h>
#include <vector>
#include <unistd.h>
#include <sys/time.h>
#include <x86intrin.h>
#include <dlfcn.h>
#include "net.h"
#include "timer.h"
#include "anp_ibvwrap.h"
#include "anp_param.h"
#include "anp_state.h"
#include "mpi.h"

extern "C" {
#include "infiniband/ionic_dv.h"
}

extern void *anp_rccl_bootstrap_handler(void *arg);

#define NCCL_NET_USE_WRITE_OP                (void *)0x1
#define ANP_CTS_QP_SLOT_INVALID              0xFF

//#define ANP_DEBUG_TRACE_EN
#define CTS_INLINE_ENABLED
#define CTS_RCVR_OFFLOAD_ENABLED

#define MAX_INLINE_DATA_SIZE 24
#define ENABLE_TIMER 0

#define MAXNAMESIZE 64
static char ncclIbIfName[MAX_IF_NAME_SIZE+1];
static union ncclSocketAddress ncclIbIfAddr;

#ifdef ANP_TELEMETRY_ENABLED
static anp_state g_anp_state;
#endif
anp_log_level_e anp_logger::log_level = LOG_ERROR;
std::atomic<int> active_threads(0);

static char libPathInfo[2048];

struct {
  uint64_t num_cts_sent;

  uint64_t num_signalled_cts_sent;
  uint64_t num_wr_wqe;
  uint64_t num_wi_wqe;
  uint64_t num_send_completion;
  uint64_t num_send_completion_ok;

  uint64_t num_recv_wqe;
  uint64_t num_recv_completion;
  uint64_t num_recv_completion_ok;
} g_debug_stats;

struct ncclIbMr {
  uintptr_t addr;
  size_t pages;
  int refs;
  ibv_mr *mr;
};

struct ncclIbMrCache {
  struct ncclIbMr *slots;
  int capacity, population;
};

static int ncclNMergedIbDevs = -1;
#define NCCL_IB_MAX_DEVS_PER_NIC 2
#define MAX_MERGED_DEV_NAME (MAXNAMESIZE*NCCL_IB_MAX_DEVS_PER_NIC)+NCCL_IB_MAX_DEVS_PER_NIC
struct alignas(64) ncclIbMergedDev {
  int ndevs;
  int devs[NCCL_IB_MAX_DEVS_PER_NIC]; // Points to an index in ncclIbDevs
  int speed;
  char devName[MAX_MERGED_DEV_NAME]; // Up to NCCL_IB_MAX_DEVS_PER_NIC * name size, and a character for each '+'
};

static int ncclNIbDevs = -1;
struct alignas(64) ncclIbDev {
  pthread_mutex_t lock;
  int device;
  uint64_t guid;
  uint8_t portNum;
  uint8_t link;
  int speed;
  ibv_context* context;
  int pdRefs;
  ibv_pd* pd;
  char devName[MAXNAMESIZE];
  char* pciPath;
  int realPort;
  int maxQp;
  struct ncclIbMrCache mrCache;
  int ar; // ADAPTIVE_ROUTING
  struct ibv_port_attr portAttr;
};

#define MAX_IB_DEVS 32
struct ncclIbMergedDev ncclIbMergedDevs[MAX_IB_DEVS];
struct ncclIbDev ncclIbDevs[MAX_IB_DEVS];
pthread_mutex_t ncclIbLock = PTHREAD_MUTEX_INITIALIZER;
static int ncclIbRelaxedOrderingEnabled = 0;

NCCL_PARAM(IbGidIndex, "IB_GID_INDEX", 0);
NCCL_PARAM(IbRoutableFlidIbGidIndex, "IB_ROUTABLE_FLID_GID_INDEX", 1);
NCCL_PARAM(IbRoceVersionNum, "IB_ROCE_VERSION_NUM", 2);
NCCL_PARAM(IbTimeout, "IB_TIMEOUT", 18);
NCCL_PARAM(IbRetryCnt, "IB_RETRY_CNT", 7);
NCCL_PARAM(IbPkey, "IB_PKEY", 0);
NCCL_PARAM(IbUseInline, "IB_USE_INLINE", 0);
NCCL_PARAM(IbSl, "IB_SL", 0);
NCCL_PARAM(IbTc, "IB_TC", 0);
NCCL_PARAM(IbArThreshold, "IB_AR_THRESHOLD", 8192);
NCCL_PARAM(IbPciRelaxedOrdering, "IB_PCI_RELAXED_ORDERING", 2);
NCCL_PARAM(IbAdaptiveRouting, "IB_ADAPTIVE_ROUTING", -2);
NCCL_PARAM(IbFifoTc, "IB_FIFO_TC", 0);
NCCL_PARAM(DmaBufEnable, "DMABUF_ENABLE", 0);

pthread_t ncclIbAsyncThread;
struct allocationTracker allocTracker[MAX_ALLOC_TRACK_NGPU] = {};

static void
anp_stats_dump_on_signal (void)
{
  fprintf(stderr, "=======\n");
  for (int i = 0; i < ncclNMergedIbDevs; i++) {
    fprintf(stderr, "Ibdev %s\n", ncclIbMergedDevs[i].devName);
  }
  fprintf(stderr, "%-52s : %lu\n", "num_cts_sent", g_debug_stats.num_cts_sent);
  fprintf(stderr, "%-52s : %lu\n", "num_signalled_cts_sent", g_debug_stats.num_signalled_cts_sent);
  fprintf(stderr, "%-52s : %lu\n", "num_recv_wqe", g_debug_stats.num_recv_wqe);
  if (g_debug_stats.num_recv_completion ==
          (g_debug_stats.num_signalled_cts_sent + g_debug_stats.num_recv_wqe)) {
      fprintf(stderr, "%-52s : %lu/%lu (OK)\n", "num_recv_completion/expected",
              g_debug_stats.num_recv_completion,
              (g_debug_stats.num_signalled_cts_sent + g_debug_stats.num_recv_wqe));
  } else {
      fprintf(stderr, "%-52s : %lu/%lu (ERR)\n", "num_recv_completion/expected",
              g_debug_stats.num_recv_completion,
              (g_debug_stats.num_signalled_cts_sent + g_debug_stats.num_recv_wqe));
  }
  fprintf(stderr, "%-52s : %lu\n", "num_recv_completion_ok", g_debug_stats.num_recv_completion_ok);
  if ((g_debug_stats.num_recv_completion - g_debug_stats.num_recv_completion_ok) > 0) {
      fprintf(stderr, "%-52s : %lu\n", "num_recv_completion_err (ERR)",
              g_debug_stats.num_recv_completion - g_debug_stats.num_recv_completion_ok);
  }

  fprintf(stderr, "%-52s : %lu\n", "num_wr_wqe", g_debug_stats.num_wr_wqe);
  fprintf(stderr, "%-52s : %lu\n", "num_wi_wqe", g_debug_stats.num_wi_wqe);
  if (g_debug_stats.num_send_completion ==
          (g_debug_stats.num_wr_wqe + g_debug_stats.num_wi_wqe)) {
      fprintf(stderr, "%-52s : %lu/%lu (OK)\n", "num_send_completion/expected",
              g_debug_stats.num_send_completion,
              (g_debug_stats.num_wr_wqe + g_debug_stats.num_wi_wqe));
  } else {
      fprintf(stderr, "%-52s : %lu/%lu (ERR)\n", "num_send_completion/expected",
              g_debug_stats.num_send_completion,
              (g_debug_stats.num_wr_wqe + g_debug_stats.num_wi_wqe));
  }
  if ((g_debug_stats.num_send_completion - g_debug_stats.num_send_completion_ok) > 0) {
      fprintf(stderr, "%-52s : %lu\n", "num_send_completion_err (ERR)",
              g_debug_stats.num_send_completion - g_debug_stats.num_send_completion_ok);
  }
  fprintf(stderr, "=======\n");
}

void anp_reinit_debug_log (void) {
  setenv("NCCL_DEBUG", "INFO", true);
  setenv("NCCL_DEBUG_SUBSYS", "ALL", true);
}

void* json_thread_init(void* arg) {
    ANP_LOG_VERBOSE("Process ID: %d, Thread ID: %lu", getpid(), pthread_self());
    anp_state* snapshot = static_cast<anp_state*>(arg);
    // destructor will trigger the dump json function
    delete snapshot;
    active_threads--;
    ANP_LOG_VERBOSE("Thread %lu completed. active threads %d", pthread_self(),
		    active_threads.load());
    return nullptr;
}

void anp_create_json_thread(void) {
#ifdef ANP_TELEMETRY_ENABLED
    pthread_t thread_id;
    pthread_attr_t attr;
    struct sched_param param;
    anp_state* snapshot = new anp_state(g_anp_state);

    pthread_attr_init(&attr);
    // detached thread
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    // set scheduling policy as default and lowest priority
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    param.sched_priority = 0;
    pthread_attr_setschedparam(&attr, &param);
    active_threads++;

    if (pthread_create(&thread_id, &attr, json_thread_init, snapshot) != 0) {
        ANP_LOG_ERROR("Failed to create json thread");
        active_threads--;
        delete snapshot;
    } else {
        ANP_LOG_VERBOSE("Thread %lu created. Active threads %d", thread_id, active_threads.load());
    }

    // Cleanup thread attributes
    pthread_attr_destroy(&attr);
#endif
}

void anp_sig_handler(int signum) {
  ANP_LOG_VERBOSE("Process ID: %d, Thread ID: %lu, signal: %s (%u)",
                  getpid(), pthread_self(), strsignal(signum), signum);

  if (signum == SIGUSR1) {
    anp_create_json_thread();
    return;
  } else if (signum == SIGUSR2) {
    anp_reinit_debug_log();
  }
  exit (-1);
}

void anp_register_signal_hdl(void) {
    std::vector<int> signalsToCatch = {SIGUSR1, SIGUSR2};

    for (auto signum : signalsToCatch) {
      if (signal(signum, anp_sig_handler) == SIG_ERR)
        WARN("NET/IB : unable to register signal handler for %s (%u)\n", strsignal(signum), signum);
    }
}

void anp_deregister_signal_hdl(void) {
    std::vector<int> signalsToCatch = {SIGUSR1, SIGUSR2};

    for (auto signum : signalsToCatch) {
      if (signal(signum, SIG_IGN) == SIG_ERR)
        WARN("NET/IB : unable to deregister signal handler for %s (%u)\n", strsignal(signum), signum);
    }
}

void wait_for_threads_before_exit(void) {
    // Restore default signal handling
    anp_deregister_signal_hdl();

    while (active_threads > 0) {
        ANP_LOG_VERBOSE("Waiting for threads to complete...");
        sleep(1);
    }
    ANP_LOG_VERBOSE("All threads completed. Safe to exit.");
}

static void* ncclIbAsyncThreadMain(void* args) {
  ANP_LOG_VERBOSE("Process ID: %d, Thread ID: %lu", getpid(), pthread_self());
  struct ncclIbDev* dev = (struct ncclIbDev*)args;
  while (1) {
    struct ibv_async_event event;
    if (ncclSuccess != wrap_ibv_get_async_event(dev->context, &event)) { break; }
    char *str;
    if (ncclSuccess != wrap_ibv_event_type_str(&str, event.event_type)) { break; }
    if (event.event_type != IBV_EVENT_COMM_EST)
      WARN("NET/IB : %s:%d Got async event : %s", dev->devName, dev->portNum, str);
    if (ncclSuccess != wrap_ibv_ack_async_event(&event)) { break; }
  }
  return NULL;
}

static inline uint64_t gettime_ns(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec)*1000*1000*1000 + ts.tv_nsec;
}

static sa_family_t envIbAddrFamily(void) {
  sa_family_t family = AF_INET;
  const char* env = getenv("NCCL_IB_ADDR_FAMILY");
  if (env == NULL || strlen(env) == 0) {
    return family;
  }

  INFO(NCCL_ENV, "NCCL_IB_ADDR_FAMILY set by environment to %s", env);

  if (strcmp(env, "AF_INET") == 0) {
    family = AF_INET;
  } else if (strcmp(env, "AF_INET6") == 0) {
    family = AF_INET6;
  }

  return family;
}

static void* envIbAddrRange(sa_family_t af, int* mask) {
  *mask = 0;
  static struct in_addr addr;
  static struct in6_addr addr6;
  void *ret = (af == AF_INET) ? (void *)&addr : (void *)&addr6;

  const char* env = getenv("NCCL_IB_ADDR_RANGE");
  if (NULL == env || strlen(env) == 0) {
    return NULL;
  }

  INFO(NCCL_ENV, "NCCL_IB_ADDR_RANGE set by environment to %s", env);

  char addrString[128] = { 0 };
  snprintf(addrString, 128, "%s", env);
  char *addrStrPtr = addrString;
  char *maskStrPtr = strstr(addrString, "/") + 1;
  if (NULL == maskStrPtr) {
    return NULL;
  }
  *(maskStrPtr - 1) = '\0';

  if (inet_pton(af, addrStrPtr, ret) == 0) {
    WARN("NET/IB: Ip address '%s' is invalid for family %s, ignoring address", addrStrPtr, (af == AF_INET) ? "AF_INET" : "AF_INET6");
    return NULL;
  }

  *mask = (int)strtol(maskStrPtr, NULL, 10);
  if (af == AF_INET && *mask > 32) {
    WARN("NET/IB: Ip address mask '%d' is invalid for family %s, ignoring mask", *mask, (af == AF_INET) ? "AF_INET" : "AF_INET6");
    *mask = 0;
    ret = NULL;
  } else if (af == AF_INET6 && *mask > 128) {
    WARN("NET/IB: Ip address mask '%d' is invalid for family %s, ignoring mask", *mask, (af == AF_INET) ? "AF_INET" : "AF_INET6");
    *mask = 0;
    ret = NULL;
  }

  return ret;
}

static sa_family_t getGidAddrFamily(union ibv_gid* gid) {
  const struct in6_addr *a = (struct in6_addr *)gid->raw;
  bool isIpV4Mapped = ((a->s6_addr32[0] | a->s6_addr32[1]) | (a->s6_addr32[2] ^ htonl(0x0000ffff))) == 0UL;
  bool isIpV4MappedMulticast = (a->s6_addr32[0] == htonl(0xff0e0000) && ((a->s6_addr32[1] | (a->s6_addr32[2] ^ htonl(0x0000ffff))) == 0UL));
  return (isIpV4Mapped || isIpV4MappedMulticast) ? AF_INET : AF_INET6;
}

static bool matchGidAddrPrefix(sa_family_t af, void* prefix, int prefixlen, union ibv_gid* gid) {
  struct in_addr *base = NULL;
  struct in6_addr *base6 = NULL;
  struct in6_addr *addr6 = NULL;;
  if (af == AF_INET) {
    base = (struct in_addr *)prefix;
  } else {
    base6 = (struct in6_addr *)prefix;
  }
  addr6 = (struct in6_addr *)gid->raw;

#define NETMASK(bits) (htonl(0xffffffff ^ ((1 << (32 - bits)) - 1)))

  int i = 0;
  while (prefixlen > 0 && i < 4) {
    if (af == AF_INET) {
      int mask = NETMASK(prefixlen);
      if ((base->s_addr & mask) ^ (addr6->s6_addr32[3] & mask)) {
        break;
      }
      prefixlen = 0;
      break;
    } else {
      if (prefixlen >= 32) {
        if (base6->s6_addr32[i] ^ addr6->s6_addr32[i]) {
          break;
        }
        prefixlen -= 32;
        ++i;
      } else {
        int mask = NETMASK(prefixlen);
        if ((base6->s6_addr32[i] & mask) ^ (addr6->s6_addr32[i] & mask)) {
          break;
        }
        prefixlen = 0;
      }
    }
  }

  return (prefixlen == 0) ? true : false;
}

static bool configuredGid(union ibv_gid* gid) {
  const struct in6_addr *a = (struct in6_addr *)gid->raw;
  int trailer = (a->s6_addr32[1] | a->s6_addr32[2] | a->s6_addr32[3]);
  if (((a->s6_addr32[0] | trailer) == 0UL) || ((a->s6_addr32[0] == htonl(0xfe800000)) && (trailer == 0UL))) {
    return false;
  }
  return true;
}

static bool linkLocalGid(union ibv_gid* gid) {
  const struct in6_addr *a = (struct in6_addr *)gid->raw;
  if (a->s6_addr32[0] == htonl(0xfe800000) && a->s6_addr32[1] == 0UL) {
    return true;
  }
  return false;
}

static bool validGid(union ibv_gid* gid) {
  return (configuredGid(gid) && !linkLocalGid(gid));
}

static ncclResult_t ncclIbRoceGetVersionNum(const char* deviceName, int portNum, int gidIndex, int* version) {
  char gidRoceVerStr[16] = { 0 };
  char roceTypePath[PATH_MAX] = { 0 };
  sprintf(roceTypePath, "/sys/class/infiniband/%s/ports/%d/gid_attrs/types/%d", deviceName, portNum, gidIndex);

  int fd = open(roceTypePath, O_RDONLY);
  if (fd == -1) {
    return ncclSystemError;
  }
  int ret = read(fd, gidRoceVerStr, 15);
  close(fd);

  if (ret == -1) {
    return ncclSystemError;
  }

  if (strlen(gidRoceVerStr)) {
    if (strncmp(gidRoceVerStr, "IB/RoCE v1", strlen("IB/RoCE v1")) == 0 || strncmp(gidRoceVerStr, "RoCE v1", strlen("RoCE v1")) == 0) {
      *version = 1;
    } else if (strncmp(gidRoceVerStr, "RoCE v2", strlen("RoCE v2")) == 0) {
      *version = 2;
    }
  }

  return ncclSuccess;
}

static ncclResult_t ncclUpdateGidIndex(struct ibv_context* context, uint8_t portNum, sa_family_t af, void* prefix, int prefixlen, int roceVer, int gidIndexCandidate, int* gidIndex) {
  union ibv_gid gid, gidCandidate;
  NCCLCHECK(wrap_ibv_query_gid(context, portNum, *gidIndex, &gid));
  NCCLCHECK(wrap_ibv_query_gid(context, portNum, gidIndexCandidate, &gidCandidate));

  sa_family_t usrFam = af;
  sa_family_t gidFam = getGidAddrFamily(&gid);
  sa_family_t gidCandidateFam = getGidAddrFamily(&gidCandidate);
  bool gidCandidateMatchSubnet = matchGidAddrPrefix(usrFam, prefix, prefixlen, &gidCandidate);

  if (gidCandidateFam != gidFam && gidCandidateFam == usrFam && gidCandidateMatchSubnet) {
    *gidIndex = gidIndexCandidate;
  } else {
    if (gidCandidateFam != usrFam || !validGid(&gidCandidate) || !gidCandidateMatchSubnet) {
      return ncclSuccess;
    }
    int usrRoceVer = roceVer;
    int gidRoceVerNum, gidRoceVerNumCandidate;
    const char* deviceName = wrap_ibv_get_device_name(context->device);
    NCCLCHECK(ncclIbRoceGetVersionNum(deviceName, portNum, *gidIndex, &gidRoceVerNum));
    NCCLCHECK(ncclIbRoceGetVersionNum(deviceName, portNum, gidIndexCandidate, &gidRoceVerNumCandidate));
    if ((gidRoceVerNum != gidRoceVerNumCandidate || !validGid(&gid)) && gidRoceVerNumCandidate == usrRoceVer) {
      *gidIndex = gidIndexCandidate;
    }
  }

  return ncclSuccess;
}

// GID Format
// global:  |              64b  - subnet-prefix                |                 64b - EUI                          |
// raw   :  | 10b fixed | 22b 0 | 16b FLID | 16b subnet-prefix |                 64b - EUI                          |
static uint16_t ncclIbExtractLocalSubnetPrefix(uint64_t subnet_prefix)
{
  return (be64toh(subnet_prefix) & 0xffff);
}

static int ncclIbExtractFlid (union ibv_gid *gid)
{
  return ntohs(*((uint16_t*)((uintptr_t)(gid->raw) + 4)));
}

static ncclResult_t ncclIbGetGidIndex(struct ibv_context *context, uint8_t portNum, struct ibv_port_attr* portAttr, int *gidIndex) {
  int gidTblLen = portAttr->gid_tbl_len;

  //for IB, choose GID Index that will have routable FLID if present
  if (portAttr->link_layer == IBV_LINK_LAYER_INFINIBAND) {
    union ibv_gid gid;
    int routableGidIndex = ncclParamIbRoutableFlidIbGidIndex();
    if (routableGidIndex < gidTblLen) {
      NCCLCHECK(wrap_ibv_query_gid(context, portNum, routableGidIndex, &gid));
      if (ncclIbExtractFlid(&gid) != 0) {
        *gidIndex = routableGidIndex;
        return ncclSuccess;
      }
    }
    *gidIndex = 0;
    return ncclSuccess;
  }

  //for ROCE
  *gidIndex = ncclParamIbGidIndex();
  if (*gidIndex >= 0) {
    return ncclSuccess;
  }

  sa_family_t userAddrFamily = envIbAddrFamily();
  int userRoceVersion = ncclParamIbRoceVersionNum();
  int prefixlen;
  void *prefix = envIbAddrRange(userAddrFamily, &prefixlen);

  *gidIndex = 0;
  for (int gidIndexNext = 1; gidIndexNext < gidTblLen; ++gidIndexNext) {
    NCCLCHECK(ncclUpdateGidIndex(context, portNum, userAddrFamily, prefix, prefixlen, userRoceVersion, gidIndexNext, gidIndex));
  }

  return ncclSuccess;
}

NCCL_PARAM(IbDisable, "IB_DISABLE", 0);
NCCL_PARAM(IbMergeVfs, "IB_MERGE_VFS", 1);
NCCL_PARAM(IbMergeNics, "IB_MERGE_NICS", 1);

static ncclResult_t ncclIbGetPciPath(char* devName, char** path, int* realPort) {
  char devicePath[PATH_MAX];
  snprintf(devicePath, PATH_MAX, "/sys/class/infiniband/%s/device", devName);
  char* p = realpath(devicePath, NULL);
  if (p == NULL) {
    WARN("Could not find real path of %s (%s)", devName, devicePath);
  } else {
    // Merge multi-port NICs into the same PCI device
    p[strlen(p)-1] = '0';
    // Also merge virtual functions (VF) into the same device
    if (ncclParamIbMergeVfs()) p[strlen(p)-3] = p[strlen(p)-4] = '0';
    // And keep the real port aside (the ibv port is always 1 on recent cards)
    *realPort = 0;
    for (int d=0; d<ncclNIbDevs; d++) {
      if (strcmp(p, ncclIbDevs[d].pciPath) == 0) (*realPort)++;
    }
  }
  *path = p;
  return ncclSuccess;
}

static int ibvWidths[] = { 1, 4, 8, 12, 2 };
static int ibvSpeeds[] = {
  2500,  /* SDR */
  5000,  /* DDR */
  10000, /* QDR */
  10000, /* QDR */
  14000, /* FDR */
  25000, /* EDR */
  50000, /* HDR */
  100000 /* NDR */ };

static int firstBitSet(int val, int max) {
  int i = 0;
  while (i<max && ((val & (1<<i)) == 0)) i++;
  return i;
}
static int ncclIbWidth(int width) {
  return ibvWidths[firstBitSet(width, sizeof(ibvWidths)/sizeof(int)-1)];
}
static int ncclIbSpeed(int speed) {
  return ibvSpeeds[firstBitSet(speed, sizeof(ibvSpeeds)/sizeof(int)-1)];
}

// Determine whether RELAXED_ORDERING is enabled and possible
static int ncclIbRelaxedOrderingCapable(void) {
  int roMode = ncclParamIbPciRelaxedOrdering();
  ncclResult_t r = ncclInternalError;
  if (roMode == 1 || roMode == 2) {
    // Query IBVERBS_1.8 API - needed for IBV_ACCESS_RELAXED_ORDERING support
    r = wrap_ibv_reg_mr_iova2(NULL, NULL, NULL, 0, 0, 0);
  }
  return r == ncclInternalError ? 0 : 1;
}

// Compare ncclIbDev[dev] to all stored mergedIbDevs
int ncclIbFindMatchingDev(int dev) {
  for (int i = 0; i < ncclNMergedIbDevs; i++) {
    if (ncclIbMergedDevs[i].ndevs < NCCL_IB_MAX_DEVS_PER_NIC) {
      int compareDev = ncclIbMergedDevs[i].devs[0];
      if (strcmp(ncclIbDevs[dev].pciPath, ncclIbDevs[compareDev].pciPath) == 0 &&
          (ncclIbDevs[dev].guid == ncclIbDevs[compareDev].guid) &&
          (ncclIbDevs[dev].link == ncclIbDevs[compareDev].link)) {
          TRACE(NCCL_NET, "NET/IB: Matched name1=%s pciPath1=%s guid1=0x%lx link1=%u name2=%s pciPath2=%s guid2=0x%lx link2=%u",
            ncclIbDevs[dev].devName, ncclIbDevs[dev].pciPath, ncclIbDevs[dev].guid, ncclIbDevs[dev].link,
            ncclIbDevs[compareDev].devName, ncclIbDevs[compareDev].pciPath, ncclIbDevs[compareDev].guid, ncclIbDevs[compareDev].link);
          return i;
      }
    }
  }

  return ncclNMergedIbDevs;
}

int convert_hostname_to_ip(const char *hostname, char *ip_str, size_t ip_str_size) {
    struct addrinfo hints, *res, *p;
    int status;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;  // Force IPv4
    hints.ai_socktype = SOCK_STREAM;

    if ((status = getaddrinfo(hostname, NULL, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo error for %s: %s\n", hostname, gai_strerror(status));
        return -1;
    }

    // Loop through the result and pick the first IPv4 address.
    for(p = res; p != NULL; p = p->ai_next) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
        if (inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, ip_str_size) != NULL) {
            freeaddrinfo(res);
            return 0;
        }
    }
    freeaddrinfo(res);
    return -1;
}

void do_host_level_bootstrapping(bool is_root, const char *root_ip, int total_hosts) {
    pthread_t thread;
    RCCLBootstrapArgs args;

    args.is_root = is_root;
    args.total_hosts = total_hosts;
    strncpy(args.root_ip, root_ip, INET_ADDRSTRLEN);

    int ret = pthread_create(&thread, NULL, anp_rccl_bootstrap_handler, (void *)&args);
    if (ret != 0) {
    	fprintf(stderr, "Error creating thread: %s\n", strerror(ret));
        return;
    }
    pthread_join(thread, NULL);
    return;
}

void do_bootstrap() {
    int local_rank;
    int global_rank;
    bool local_has_root;
    int name_len;
    char processor_name[MPI_MAX_PROCESSOR_NAME];
    static char root_host[MPI_MAX_PROCESSOR_NAME] = {0};
    char host_ip_buf[16];

    // Assume MPI_Init has already been called.
    MPI_Comm local_comm;
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &local_comm);

    // Get local rank (on this host)
    MPI_Comm_rank(local_comm, &local_rank);

    MPI_Comm_rank(MPI_COMM_WORLD, &global_rank);

    // Get the name of the host where this process is running.
    MPI_Get_processor_name(processor_name, &name_len);

    /*
     * Each process sets a flag if it is global rank 0.
     * Typically, only one process among all hosts is global rank 0.
     */
    local_has_root = (global_rank == 0) ? 1 : 0;

    // Each process is a host leader if its local_rank is 0.
    int is_host_leader = (local_rank == 0) ? 1 : 0;
    int total_hosts = 0;

    // Allreduce across MPI_COMM_WORLD to sum the leader flags.
    MPI_Allreduce(&is_host_leader, &total_hosts, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    // Host leader (local_rank == 0) does the bootstrapping.
    if(local_rank == 0) {
    	WARN("initiating bootstrap from local rank [%d] one of %d hosts", local_rank, total_hosts);
	    /*
	     * The global process with rank 0 gets its host name.
	     * Then, we broadcast this host name to all processes in MPI_COMM_WORLD.
	     * This way, every process knows the host name of the process that is rank 0.
	     */
	    if (local_has_root == 1) {
		strncpy(root_host, processor_name, MPI_MAX_PROCESSOR_NAME);
		WARN("copying root_host from [%s] global rank [%d]", processor_name, global_rank);
	     }
		MPI_Bcast(root_host, MPI_MAX_PROCESSOR_NAME, MPI_CHAR, 0, MPI_COMM_WORLD);

	    WARN("The host with global rank 0 is: %s\n", root_host);
	    if (convert_hostname_to_ip(root_host, host_ip_buf, sizeof(host_ip_buf)) == 0) {
		WARN("[%s]The ip of the host with global rank 0 is: %s\n", processor_name, host_ip_buf);
	    }
       do_host_level_bootstrapping(local_has_root, host_ip_buf, total_hosts);
    }

   // All processes on the same host wait here until bootstrapping is done by their host leader
   MPI_Barrier(local_comm);
   WARN("continue regular processing for rank [%d]", global_rank);
}

static void showVersion() {
  // retrieve librccl path
  Dl_info pathInfo;

  if (dladdr((void*)ncclIbAsyncThreadMain, &pathInfo)) {
    strncpy(libPathInfo, pathInfo.dli_fname, sizeof(libPathInfo)-1);
  } else {
    // sets libPath to Unknown if the above function call is not successful
    strncpy(libPathInfo, "Unknown", sizeof(libPathInfo)-1);
  }
}

// Plugin implementations of the required functions
ncclResult_t anpNetInit(ncclDebugLogger_t logFunction) {
  ncclResult_t ret;
  if (ncclParamIbDisable()) return ncclInternalError;
  static int shownIbHcaEnv = 0;
  if(wrap_ibv_symbols() != ncclSuccess) { return ncclInternalError; }

  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, showVersion);

  WARN("ANP plugin loaded successfully with telemetry %s : %s", TELEMETRY_STATUS, libPathInfo);
  // TODO
  //do_bootstrap();
  // register exit handler
#ifdef ANP_TELEMETRY_ENABLED
  std::atexit(wait_for_threads_before_exit);
#endif
  anp_register_signal_hdl();

  // Detect IB cards
  int nIbDevs = 0;
  struct ibv_device** devices = NULL;

  if (ncclNIbDevs == -1) {
    pthread_mutex_lock(&ncclIbLock);
    wrap_ibv_fork_init();
    if (ncclNIbDevs == -1) {
      ncclNIbDevs = 0;
      ncclNMergedIbDevs = 0;
      if (ncclFindInterfaces(ncclIbIfName, &ncclIbIfAddr, MAX_IF_NAME_SIZE, 1) != 1) {
        WARN("NET/IB : No IP interface found.");
        ret = ncclInternalError;
        goto fail;
      }

      // Check if user defined which IB device:port to use
      char* userIbEnv = getenv("NCCL_IB_HCA");
      if (userIbEnv != NULL && shownIbHcaEnv++ == 0) INFO(NCCL_NET|NCCL_ENV, "NCCL_IB_HCA set to %s", userIbEnv);
      struct netIf userIfs[MAX_IB_DEVS];
      bool searchNot = userIbEnv && userIbEnv[0] == '^';
      if (searchNot) userIbEnv++;
      bool searchExact = userIbEnv && userIbEnv[0] == '=';
      if (searchExact) userIbEnv++;
      int nUserIfs = parseStringList(userIbEnv, userIfs, MAX_IB_DEVS);

      if (ncclSuccess != wrap_ibv_get_device_list(&devices, &nIbDevs)) { ret = ncclInternalError; goto fail; }

      // Should NCCL merge multi-port devices into one?
      int mergeNics = ncclParamIbMergeNics();

build_ib_list:
      for (int d=0; d<nIbDevs && ncclNIbDevs<MAX_IB_DEVS; d++) {
        struct ibv_context * context;
        if (ncclSuccess != wrap_ibv_open_device(&context, devices[d]) || context == NULL) {
          WARN("NET/IB : Unable to open device %s", devices[d]->name);
          continue;
        }
        int nPorts = 0;
        struct ibv_device_attr devAttr;
        memset(&devAttr, 0, sizeof(devAttr));
        if (ncclSuccess != wrap_ibv_query_device(context, &devAttr)) {
          WARN("NET/IB : Unable to query device %s", devices[d]->name);
          if (ncclSuccess != wrap_ibv_close_device(context))
          {
            ret = ncclInternalError;
            goto fail;
          }
          continue;
        }
        for (int port_num = 1; port_num <= devAttr.phys_port_cnt; port_num++) {
          struct ibv_port_attr portAttr;
          if (ncclSuccess != wrap_ibv_query_port(context, port_num, &portAttr)) {
            WARN("NET/IB : Unable to query port_num %d", port_num);
            continue;
          }
          if (portAttr.state != IBV_PORT_ACTIVE) continue;
          if (portAttr.link_layer != IBV_LINK_LAYER_INFINIBAND
              && portAttr.link_layer != IBV_LINK_LAYER_ETHERNET) continue;

          // check against user specified HCAs/ports
          if (! (matchIfList(devices[d]->name, port_num, userIfs, nUserIfs, searchExact) ^ searchNot)) {
            continue;
          }
          pthread_mutex_init(&ncclIbDevs[ncclNIbDevs].lock, NULL);
          ncclIbDevs[ncclNIbDevs].device = d;
          ncclIbDevs[ncclNIbDevs].guid = devAttr.sys_image_guid;
          ncclIbDevs[ncclNIbDevs].portAttr = portAttr;
          ncclIbDevs[ncclNIbDevs].portNum = port_num;
          ncclIbDevs[ncclNIbDevs].link = portAttr.link_layer;
          ncclIbDevs[ncclNIbDevs].speed = ncclIbSpeed(portAttr.active_speed) * ncclIbWidth(portAttr.active_width);
          ncclIbDevs[ncclNIbDevs].context = context;
          ncclIbDevs[ncclNIbDevs].pdRefs = 0;
          ncclIbDevs[ncclNIbDevs].pd = NULL;
          strncpy(ncclIbDevs[ncclNIbDevs].devName, devices[d]->name, MAXNAMESIZE);
          NCCLCHECK(ncclIbGetPciPath(ncclIbDevs[ncclNIbDevs].devName, &ncclIbDevs[ncclNIbDevs].pciPath, &ncclIbDevs[ncclNIbDevs].realPort));
          ncclIbDevs[ncclNIbDevs].maxQp = devAttr.max_qp;
          ncclIbDevs[ncclNIbDevs].mrCache.capacity = 0;
          ncclIbDevs[ncclNIbDevs].mrCache.population = 0;
          ncclIbDevs[ncclNIbDevs].mrCache.slots = NULL;

          // Enable ADAPTIVE_ROUTING by default on IB networks
          // But allow it to be overloaded by an env parameter
          ncclIbDevs[ncclNIbDevs].ar = (portAttr.link_layer == IBV_LINK_LAYER_INFINIBAND) ? 1 : 0;
          if (ncclParamIbAdaptiveRouting() != -2) ncclIbDevs[ncclNIbDevs].ar = ncclParamIbAdaptiveRouting();

          TRACE(NCCL_NET,"NET/IB: [%d] %s:%s:%d/%s speed=%d context=%p pciPath=%s ar=%d", d, devices[d]->name, devices[d]->dev_name, ncclIbDevs[ncclNIbDevs].portNum,
              portAttr.link_layer == IBV_LINK_LAYER_INFINIBAND ? "IB" : "RoCE", ncclIbDevs[ncclNIbDevs].speed, context, ncclIbDevs[ncclNIbDevs].pciPath, ncclIbDevs[ncclNIbDevs].ar);

          pthread_create(&ncclIbAsyncThread, NULL, ncclIbAsyncThreadMain, ncclIbDevs + ncclNIbDevs);
          ncclSetThreadName(ncclIbAsyncThread, "NCCL IbAsync %2d", ncclNIbDevs);
          pthread_detach(ncclIbAsyncThread); // will not be pthread_join()'d

          int mergedDev = ncclNMergedIbDevs;
          if (mergeNics) {
            mergedDev = ncclIbFindMatchingDev(ncclNIbDevs);
          }

          // No matching dev found, create new mergedDev entry (it's okay if there's only one dev inside)
          if (mergedDev == ncclNMergedIbDevs) {
            // Set ndevs to 1, assign first ibDevN to the current IB device
            ncclIbMergedDevs[mergedDev].ndevs = 1;
            ncclIbMergedDevs[mergedDev].devs[0] = ncclNIbDevs;
            ncclNMergedIbDevs++;
            strncpy(ncclIbMergedDevs[mergedDev].devName, ncclIbDevs[ncclNIbDevs].devName, MAXNAMESIZE);
          // Matching dev found, edit name
          } else {
            // Set next device in this array to the current IB device
            int ndevs = ncclIbMergedDevs[mergedDev].ndevs;
            ncclIbMergedDevs[mergedDev].devs[ndevs] = ncclNIbDevs;
            ncclIbMergedDevs[mergedDev].ndevs++;
            snprintf(ncclIbMergedDevs[mergedDev].devName + strlen(ncclIbMergedDevs[mergedDev].devName), MAXNAMESIZE+1, "+%s", ncclIbDevs[ncclNIbDevs].devName);
          }

          // Aggregate speed
          ncclIbMergedDevs[mergedDev].speed += ncclIbDevs[ncclNIbDevs].speed;
          ncclNIbDevs++;
          nPorts++;
        }
        if (nPorts == 0 && ncclSuccess != wrap_ibv_close_device(context)) { ret = ncclInternalError; goto fail; }
      }

      // Detect if there are both multi-port and single-port NICs in the system. If so, disable port merging and build the list again
      if (mergeNics) {
        for (int d = 0; d < ncclNMergedIbDevs; d++) {
          if (ncclIbMergedDevs[d].ndevs != ncclIbMergedDevs[0].ndevs) {
            INFO(NCCL_NET, "Detected a mix of single and multiple-port NICs. Force-disabling NCCL_IB_MERGE_NICS");
            mergeNics = 0;
            ncclNIbDevs = 0;
            ncclNMergedIbDevs = 0;
            memset(ncclIbMergedDevs, 0, sizeof(ncclIbMergedDevs));
            goto build_ib_list;
          }
        }
      }
      if (ncclSuccess != wrap_ibv_free_device_list(devices)) { ret = ncclInternalError; goto fail;}
    }
    if (ncclNIbDevs == 0) {
      INFO(NCCL_INIT|NCCL_NET, "NET/IB : No device found.");
    } else {
      char line[2048];
      line[0] = '\0';
      // Determine whether RELAXED_ORDERING is enabled and possible
      ncclIbRelaxedOrderingEnabled = ncclIbRelaxedOrderingCapable();
      for (int d = 0; d < ncclNMergedIbDevs; d++) {
        struct ncclIbMergedDev* mergedDev = ncclIbMergedDevs + d;
        if (mergedDev->ndevs > 1) {
          // Print out merged dev info
          snprintf(line+strlen(line), 2047-strlen(line), " [%d]={", d);
          for (int i = 0; i < mergedDev->ndevs; i++) {
            int ibDev = mergedDev->devs[i];
            snprintf(line+strlen(line), 2047-strlen(line), "[%d] %s:%d/%s%s", ibDev, ncclIbDevs[ibDev].devName,
              ncclIbDevs[ibDev].portNum, ncclIbDevs[ibDev].link == IBV_LINK_LAYER_INFINIBAND ? "IB" : "RoCE",
              // Insert comma to delineate
              i == (mergedDev->ndevs - 1) ? "" : ", ");
          }
          snprintf(line+strlen(line), 2047-strlen(line), "}");
        } else {
          int ibDev = mergedDev->devs[0];
          snprintf(line+strlen(line), 2047-strlen(line), " [%d]%s:%d/%s", ibDev, ncclIbDevs[ibDev].devName,
            ncclIbDevs[ibDev].portNum, ncclIbDevs[ibDev].link == IBV_LINK_LAYER_INFINIBAND ? "IB" : "RoCE");
        }
      }
      line[2047] = '\0';
      char addrline[SOCKET_NAME_MAXLEN+1];
      INFO(NCCL_INIT|NCCL_NET, "NET/IB : Using%s %s; OOB %s", line, ncclIbRelaxedOrderingEnabled ? "[RO]" : "",
           ncclIbIfName); /*  socketToString(&ncclIbIfAddr, addrline)); TODO: Make this work*/
    }
    pthread_mutex_unlock(&ncclIbLock);
  }
  return ncclSuccess;
fail:
  if(ncclSuccess != wrap_ibv_free_device_list(devices)){WARN("NET/IB : Unable to free device list");}
  pthread_mutex_unlock(&ncclIbLock);
  return ret;
}

ncclResult_t anpNetDevices(int* ndev) {
  // Implement logic to determine the number of network devices
  *ndev = ncclNMergedIbDevs;
  return ncclSuccess;
}

// Detect whether GDR can work on a given NIC with the current CUDA device
// Returns :
// ncclSuccess : GDR works
// ncclSystemError : no module or module loaded but not supported by GPU
ncclResult_t ncclIbGdrSupport() {
    // TODO
  return ncclSuccess;
}

// Detect whether DMA-BUF support is present in the kernel
// Returns :
// ncclSuccess : DMA-BUF support is available
// ncclSystemError : DMA-BUF is not supported by the kernel
ncclResult_t ncclIbDmaBufSupport(int dev) {
  if (ncclParamDmaBufEnable() == 1) {
    WARN("DMABUF Enabled");
    return ncclSuccess;
  }
  return ncclSystemError;
}

#define NCCL_NET_IB_MAX_RECVS 8

ncclResult_t anpNetGetProperties(int dev, ncclNetProperties_t* props) {
  // Implement logic to get properties of the specified device
  struct ncclIbMergedDev* mergedDev = ncclIbMergedDevs+dev;
  props->name = mergedDev->devName;
  props->speed = mergedDev->speed;

  // Take the rest of the properties from an arbitrary sub-device (should be the same)
  struct ncclIbDev* ibDev = ncclIbDevs + mergedDev->devs[0];
  props->pciPath = ibDev->pciPath;
  props->guid = ibDev->guid;
  props->ptrSupport = NCCL_PTR_HOST;
  if (ncclIbGdrSupport() == ncclSuccess) {
    props->ptrSupport |= NCCL_PTR_CUDA; // GDR support via nv_peermem
  }
  props->regIsGlobal = 1;
  if (ncclIbDmaBufSupport(dev) == ncclSuccess) {
    props->ptrSupport |= NCCL_PTR_DMABUF; // GDR support via DMA-BUF
  }
  props->latency = 0; // Not set
  props->port = ibDev->portNum + ibDev->realPort;
  props->maxComms = ibDev->maxQp;
  props->maxRecvs = NCCL_NET_IB_MAX_RECVS;
  props->netDeviceType    = NCCL_NET_DEVICE_HOST;
  props->netDeviceVersion = NCCL_NET_DEVICE_INVALID_VERSION;
  props->maxP2pBytes = NCCL_MAX_NET_SIZE_BYTES;
  return ncclSuccess;
}

// We need to support NCCL_NET_MAX_REQUESTS for each concurrent receive
#define MAX_REQUESTS (NCCL_NET_MAX_REQUESTS*NCCL_NET_IB_MAX_RECVS)
static_assert(MAX_REQUESTS <= 256, "request id are encoded in wr_id and we need up to 8 requests ids per completion");

#define NCCL_IB_MAX_QPS 128

// Per-QP connection metatdata
struct ncclIbQpInfo {
  uint32_t qpn;

  // Fields needed for ece (enhanced connection establishment)
  struct ibv_ece ece;
  int ece_supported;
  int devIndex;
};

// Per-Dev connection metadata
struct ncclIbDevInfo {
  uint32_t lid;
  uint8_t ib_port;
  enum ibv_mtu mtu;
  uint8_t link_layer;

  // For RoCE and IB Rounter
  union ibv_gid gid;

  // FIFO RDMA info
  uint32_t fifoRkey;

  //remote dev info
  union ibv_gid remoteGid;

  int ibv_dev_index;
};

// Struct containing everything needed to establish connections
struct ncclIbConnectionMetadata {
  struct ncclIbQpInfo qpInfo[NCCL_IB_MAX_QPS];
  struct ncclIbDevInfo devs[NCCL_IB_MAX_DEVS_PER_NIC];
  char devName[MAX_MERGED_DEV_NAME];
  uint64_t fifoAddr;
  int ndevs;
};

enum ncclIbCommState {
  ncclIbCommStateStart = 0,
  ncclIbCommStateConnect = 1,
  ncclIbCommStateAccept = 3,
  ncclIbCommStateSend = 4,
  ncclIbCommStateRecv = 5,
  ncclIbCommStateConnecting = 6,
  ncclIbCommStateConnected = 7,
  ncclIbCommStatePendingReady = 8,
};

struct ncclIbCommStage {
  enum ncclIbCommState state;
  int offset;
  void* buffer;
  void* comm;
};

struct ncclIbHandle {
  union ncclSocketAddress connectAddr; // Filled by the target
  uint64_t magic; // random number to help debugging
  struct ncclIbCommStage stage; // Used by the other side when connecting
};

// Retain local RoCE address for error logging
struct ncclIbGidInfo {
  uint8_t link_layer;
  union ibv_gid localGid;
  int32_t localGidIndex;
};

#define NCCL_NET_IB_REQ_UNUSED 0
#define NCCL_NET_IB_REQ_SEND 1
#define NCCL_NET_IB_REQ_RECV 2
#define NCCL_NET_IB_REQ_FLUSH 3
const char* reqTypeStr[] = { "Unused", "Send", "Recv", "Flush" };

struct ncclIbRequest {
  struct ncclIbNetCommBase* base;
  int type;
  struct ncclSocket* sock;
  int events[NCCL_IB_MAX_DEVS_PER_NIC];
  struct ncclIbNetCommDevBase* devBases[NCCL_IB_MAX_DEVS_PER_NIC];
  int nreqs;
  union {
    struct {
      int size;
      void* data;
      uint32_t lkeys[NCCL_IB_MAX_DEVS_PER_NIC];
      int offset;
    } send;
    struct {
      int* sizes;
    } recv;
  };
};

struct ncclIbNetCommDevBase {
  int ibDevN;
  struct ibv_pd* pd;
  struct ibv_cq* cq;
  uint64_t pad[2];
  struct ncclIbGidInfo gidInfo;
};

struct ncclIbListenComm {
  int dev;
  struct ncclSocket sock;
  struct ncclIbCommStage stage;
};

struct alignas(32) ncclIbSendFifo {
  uint64_t addr;
  uint32_t rkeys[1];
  int      size;
  uint8_t  nreqs;
  uint16_t tag;
  uint32_t idx;
  char padding[9];
} __attribute__((packed));

struct ncclIbQp {
  struct ibv_qp* qp;
  int devIndex;
  int remDevIdx;
  int8_t ctsQpSlot;
#ifdef ANP_DEBUG_TRACE_EN
  uint16_t channelId;
  uint8_t data;
#endif
};

struct ncclIbRemSizesFifo {
  int elems[MAX_REQUESTS][NCCL_NET_IB_MAX_RECVS];
  uint64_t fifoTail;
  uint64_t addr;
  uint32_t rkeys[NCCL_IB_MAX_DEVS_PER_NIC];
  uint32_t flags;
  struct ibv_mr* mrs[NCCL_IB_MAX_DEVS_PER_NIC];
  struct ibv_sge sge;
};

// A per-dev struct for netIbSendComm
struct alignas(8) ncclIbSendCommDev {
  struct ncclIbNetCommDevBase base;
  struct ibv_mr* fifoMr;
};


// Wrapper to track an MR per-device, if needed
struct ncclIbMrHandle {
  ibv_mr* mrs[NCCL_IB_MAX_DEVS_PER_NIC];
};

struct alignas(32) ncclIbNetCommBase {
  int ndevs;
  bool isSend;
  struct ncclIbRequest reqs[MAX_REQUESTS];
  struct ncclIbQp qps[NCCL_IB_MAX_QPS];
  int nqps;
  int qpIndex;
  int devIndex;
  struct ncclSocket sock;
  int ready;
  // Track necessary remDevInfo here
  int nRemDevs;
  struct ncclIbDevInfo remDevs[NCCL_IB_MAX_DEVS_PER_NIC];
};

struct ncclIbSendComm {
  struct ncclIbNetCommBase base;
  struct ncclIbSendFifo fifo[MAX_REQUESTS][NCCL_NET_IB_MAX_RECVS];
  // Each dev correlates to a mergedIbDev
  struct ncclIbSendCommDev devs[NCCL_IB_MAX_DEVS_PER_NIC];
  struct ncclIbRequest* fifoReqs[MAX_REQUESTS][NCCL_NET_IB_MAX_RECVS];
  struct ibv_sge sges[NCCL_NET_IB_MAX_RECVS];
  struct ibv_send_wr wrs[NCCL_NET_IB_MAX_RECVS+1];
  struct ncclIbRemSizesFifo remSizesFifo;
  uint64_t fifoHead;
  int ar; // Use adaptive routing when all merged devices have it enabled
};
// The SendFifo needs to be 32-byte aligned and each element needs
// to be a 32-byte multiple, so that an entry does not get split and
// written out of order when IB Relaxed Ordering is enabled
static_assert((sizeof(struct ncclIbNetCommBase) % 32) == 0, "ncclIbNetCommBase size must be 32-byte multiple to ensure fifo is at proper offset");
static_assert((offsetof(struct ncclIbSendComm, fifo) % 32) == 0, "ncclIbSendComm fifo must be 32-byte aligned");
static_assert((sizeof(struct ncclIbSendFifo) % 32) == 0, "ncclIbSendFifo element size must be 32-byte multiples");
static_assert((offsetof(struct ncclIbSendComm, sges) % 32) == 0, "sges must be 32-byte aligned");
static_assert((offsetof(struct ncclIbSendComm, wrs) % 32) == 0, "wrs must be 32-byte aligned");

struct ncclIbGpuFlush {
  struct ibv_mr* hostMr;
  struct ibv_mr* gpuMr;
  int* gpuFlushGpuMem;
  struct ibv_sge sge;
  struct ncclIbQp qp;
};

struct ncclIbRemFifo {
  struct ncclIbSendFifo elems[MAX_REQUESTS][NCCL_NET_IB_MAX_RECVS];
  uint64_t fifoTail;
  uint64_t addr;
  uint32_t flags;
};

struct alignas(16) ncclIbRecvCommDev {
  struct ncclIbNetCommDevBase base;
  struct ncclIbGpuFlush gpuFlush;
  uint32_t fifoRkey;
  struct ibv_mr* fifoMr;
  struct ibv_sge fifoSge;
  struct ibv_mr* sizesFifoMr;
};

struct ncclIbRecvComm {
  struct ncclIbNetCommBase base;
  struct ncclIbRecvCommDev    devs[NCCL_IB_MAX_DEVS_PER_NIC];
  struct ncclIbRemFifo remFifo;
  int sizesFifo[MAX_REQUESTS][NCCL_NET_IB_MAX_RECVS];
  int gpuFlushHostMem;
  int flushEnabled;
};
static_assert((offsetof(struct ncclIbRecvComm, remFifo) % 32) == 0, "ncclIbRecvComm fifo must be 32-byte aligned");

NCCL_PARAM(IbQpsPerConn, "IB_QPS_PER_CONNECTION", 1);

static void ncclIbAddEvent(struct ncclIbRequest* req, int devIndex, struct ncclIbNetCommDevBase* base) {
  req->events[devIndex]++;
  req->devBases[devIndex] = base;
}

ncclResult_t ncclIbInitCommDevBase(int ibDevN, struct ncclIbNetCommDevBase* base) {
  base->ibDevN = ibDevN;
  ncclIbDev* ibDev = ncclIbDevs + ibDevN;
  pthread_mutex_lock(&ibDev->lock);
  if (0 == ibDev->pdRefs++) {
    ncclResult_t res;
    NCCLCHECKGOTO(wrap_ibv_alloc_pd(&ibDev->pd, ibDev->context), res, failure);
    if (0) {
    failure:
      pthread_mutex_unlock(&ibDev->lock);
      return res;
    }
  }
  base->pd = ibDev->pd;
  pthread_mutex_unlock(&ibDev->lock);

  // CQ is sized to accommodate the max SQ + RQ WQE completions. If each SQ WQE could be signaled, then,
  // for each QP, there can be 2*MAX_REQUESTS completions for SQ and MAX_REQUESTS completions for RQ.
  NCCLCHECK(wrap_ibv_create_cq(&base->cq, ibDev->context, 3*MAX_REQUESTS*ncclParamIbQpsPerConn(), NULL, NULL, 0));
#ifdef ANP_DEBUG_TRACE_EN
  INFO(NCCL_NET, "[ANP_TRACE] Created cq, ibDevN %d, handle %u, fd %d, refcount %d, cqe %d", ibDevN, base->cq->handle,
       base->cq->channel ? base->cq->channel->fd : -1,
       base->cq->channel ? base->cq->channel->refcnt : -1, base->cq->cqe);
#endif

  return ncclSuccess;
}

ncclResult_t ncclIbDestroyBase(struct ncclIbNetCommDevBase* base) {
  ncclResult_t res;
  NCCLCHECK(wrap_ibv_destroy_cq(base->cq));

  pthread_mutex_lock(&ncclIbDevs[base->ibDevN].lock);
  if (0 == --ncclIbDevs[base->ibDevN].pdRefs) {
    NCCLCHECKGOTO(wrap_ibv_dealloc_pd(ncclIbDevs[base->ibDevN].pd), res, returning);
  }
  res = ncclSuccess;
returning:
  pthread_mutex_unlock(&ncclIbDevs[base->ibDevN].lock);
  return res;
}

typedef struct channel_ud_s_ {
    int channelId;
    bool ud_id;
    bool ud_allocated;
} channel_ud_t;
static channel_ud_t data_channel_ud[128];
static bool data_last_ud[128];
static channel_ud_t channel_ud[128];
static bool last_ud[128];

ncclResult_t ncclIbCreateQp(uint8_t ib_port, struct ncclIbNetCommDevBase* base,
                            int access_flags, struct ncclIbQp* qp, int channelId,
                            bool dataQP, int8_t qp_idx) {
  struct ibv_qp_init_attr qpInitAttr;
  memset(&qpInitAttr, 0, sizeof(struct ibv_qp_init_attr));
  qpInitAttr.send_cq = base->cq;
  qpInitAttr.recv_cq = base->cq;
  qpInitAttr.qp_type = IBV_QPT_RC;
  if (dataQP) {
    if (!data_channel_ud[channelId].ud_allocated) {
      bool lud = data_last_ud[base->ibDevN];
      data_channel_ud[channelId].ud_id = lud;
      data_last_ud[base->ibDevN] = !(data_last_ud[base->ibDevN]);
      data_channel_ud[channelId].ud_allocated = true;
    }
    if (data_channel_ud[channelId].ud_id) {
        wrap_ibv_pd_set_udma_mask(base->pd, IONIC_UDMA_MASK_HIGH);
    } else {
        wrap_ibv_pd_set_udma_mask(base->pd, IONIC_UDMA_MASK_LOW);
    }
  } else {
    if (!channel_ud[channelId].ud_allocated) {
      bool lud = last_ud[base->ibDevN];
      channel_ud[channelId].ud_id = lud;
      last_ud[base->ibDevN] = !(last_ud[base->ibDevN]);
      channel_ud[channelId].ud_allocated = true;
    }
    if (channel_ud[channelId].ud_id) {
        wrap_ibv_pd_set_udma_mask(base->pd, IONIC_UDMA_MASK_HIGH);
    } else {
        wrap_ibv_pd_set_udma_mask(base->pd, IONIC_UDMA_MASK_LOW);
    }
  }
  qpInitAttr.sq_sig_all |= (1 << 16);
  if (dataQP) {
    qpInitAttr.sq_sig_all |= (1 << 17);
  } else {
    qpInitAttr.sq_sig_all &= (~(1 << 17));
  }
  qpInitAttr.sq_sig_all |= (1 << 18);
#if !defined(CTS_RCVR_OFFLOAD_ENABLED)
  qpInitAttr.sq_sig_all &= (~(1 << 19));
#else
  qpInitAttr.sq_sig_all |= (1 << 19);
#endif
  // We might send 2 messages per send (RDMA and RDMA_WITH_IMM)
  qpInitAttr.cap.max_send_wr = 2*MAX_REQUESTS;
  qpInitAttr.cap.max_recv_wr = MAX_REQUESTS;
  qpInitAttr.cap.max_send_sge = 1;
  qpInitAttr.cap.max_recv_sge = 1;
#if defined(CTS_INLINE_ENABLED)
  qpInitAttr.cap.max_inline_data = MAX_INLINE_DATA_SIZE;
#else
  qpInitAttr.cap.max_inline_data = ncclParamIbUseInline() ? sizeof(struct ncclIbSendFifo) : 0;
#endif
  NCCLCHECK(wrap_ibv_create_qp(&qp->qp, base->pd, &qpInitAttr));
  ANP_TELEMETRY_EXECUTE(
      g_anp_state.add_queue_pair(base->ibDevN, channelId, qp->qp->qp_num, dataQP);
  );
  wrap_ionic_dv_qp_set_gda(qp->qp, false, true);
  struct ibv_qp_attr qpAttr;
  memset(&qpAttr, 0, sizeof(struct ibv_qp_attr));
  qpAttr.qp_state = IBV_QPS_INIT;
  qpAttr.pkey_index = ncclParamIbPkey();
  qpAttr.port_num = ib_port;
  qpAttr.qp_access_flags = access_flags;
  NCCLCHECK(wrap_ibv_modify_qp(qp->qp, &qpAttr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS));
  ANP_TELEMETRY_EXECUTE(
      anp_create_json_thread();
  );
  if (dataQP == false) {
    qp->ctsQpSlot = qp_idx;
  } else {
    qp->ctsQpSlot = ANP_CTS_QP_SLOT_INVALID;
  }
#ifdef ANP_DEBUG_TRACE_EN
  qp->channelId = channelId;
  qp->data = (dataQP == true) ? 1 : 0;
  INFO(NCCL_NET, "[ANP_TRACE] Created %s qp %d, ch %d, cq handle %d, src nic %d",
       dataQP ? "data" : "CTS", qp->qp->qp_num, channelId, base->cq->handle, base->ibDevN);
#endif

  return ncclSuccess;
}

ncclResult_t ncclIbRtrQp(struct ibv_qp* qp, struct ncclIbGidInfo* sGidInfo, uint32_t dest_qp_num, struct ncclIbDevInfo* info, bool override_tc) {
  struct ibv_qp_attr qpAttr;
  memset(&qpAttr, 0, sizeof(struct ibv_qp_attr));
  qpAttr.qp_state = IBV_QPS_RTR;
  qpAttr.path_mtu = info->mtu;
  qpAttr.dest_qp_num = dest_qp_num;
  qpAttr.rq_psn = 0;
  qpAttr.max_dest_rd_atomic = 1;
  qpAttr.min_rnr_timer = 12;
  if (info->link_layer == IBV_LINK_LAYER_ETHERNET) {
    qpAttr.ah_attr.is_global = 1;
    qpAttr.ah_attr.grh.dgid.global.subnet_prefix = info->gid.global.subnet_prefix;
    qpAttr.ah_attr.grh.dgid.global.interface_id = info->gid.global.interface_id;
    qpAttr.ah_attr.grh.flow_label = 0;
    qpAttr.ah_attr.grh.sgid_index = sGidInfo->localGidIndex;
    qpAttr.ah_attr.grh.hop_limit = 255;
    if(ncclParamIbFifoTc() && override_tc) {
      qpAttr.ah_attr.grh.traffic_class = ncclParamIbFifoTc();
    } else {
      qpAttr.ah_attr.grh.traffic_class = ncclParamIbTc();
    }
  } else {
    //pick lid if subnet prefixs are same, FLID if they are not
    if (ncclIbExtractLocalSubnetPrefix(sGidInfo->localGid.global.subnet_prefix) ==
		    ncclIbExtractLocalSubnetPrefix(info->gid.global.subnet_prefix)) {
        qpAttr.ah_attr.is_global = 0;
        qpAttr.ah_attr.dlid = info->lid;
    } else {
	uint16_t flid = ncclIbExtractFlid(&info->gid);
        if (flid == 0) {
          WARN("Warning: remote FLID configured as zero even when endpoints are on different subnets, using dlid as fallback");
          qpAttr.ah_attr.dlid = info->lid;
	} else {
          qpAttr.ah_attr.dlid = ncclIbExtractFlid(&info->gid);
	}
        qpAttr.ah_attr.is_global = 1;
        qpAttr.ah_attr.grh.dgid.global.subnet_prefix = info->gid.global.subnet_prefix;
        qpAttr.ah_attr.grh.dgid.global.interface_id = info->gid.global.interface_id;
        qpAttr.ah_attr.grh.sgid_index = sGidInfo->localGidIndex;
	qpAttr.ah_attr.grh.hop_limit = 255;
    }
  }
  qpAttr.ah_attr.sl = ncclParamIbSl();
  qpAttr.ah_attr.src_path_bits = 0;
  qpAttr.ah_attr.port_num = info->ib_port;
  NCCLCHECK(wrap_ibv_modify_qp(qp, &qpAttr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER));
  return ncclSuccess;
}

ncclResult_t ncclIbRtsQp(struct ibv_qp* qp) {
  struct ibv_qp_attr qpAttr;
  memset(&qpAttr, 0, sizeof(struct ibv_qp_attr));
  qpAttr.qp_state = IBV_QPS_RTS;
  qpAttr.timeout = ncclParamIbTimeout();
  qpAttr.retry_cnt = ncclParamIbRetryCnt();
  qpAttr.rnr_retry = 7;
  qpAttr.sq_psn = 0;
  qpAttr.max_rd_atomic = 1;
  NCCLCHECK(wrap_ibv_modify_qp(qp, &qpAttr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC));
  return ncclSuccess;
}

ncclResult_t anpNetListen(int dev, void* opaqueHandle, void** listenComm) {
  // Implement listening logic
  struct ncclIbListenComm* comm;
  NCCLCHECK(ncclCalloc(&comm, 1));
  struct ncclIbHandle* handle = (struct ncclIbHandle*) opaqueHandle;
  static_assert(sizeof(struct ncclIbHandle) < NCCL_NET_HANDLE_MAXSIZE, "ncclIbHandle size too large");
  memset(handle, 0, sizeof(struct ncclIbHandle));
  comm->dev = dev;
  handle->magic = NCCL_SOCKET_MAGIC;
  NCCLCHECK(ncclSocketInit(&comm->sock, &ncclIbIfAddr, handle->magic, ncclSocketTypeNetIb, NULL, 1));
  NCCLCHECK(ncclSocketListen(&comm->sock));
  NCCLCHECK(ncclSocketGetAddr(&comm->sock, &handle->connectAddr));
  *listenComm = comm;
  return ncclSuccess;
}

ncclResult_t anpNetConnect(int dev, void* opaqueHandle, void** sendComm, ncclNetDeviceHandle_t** chId) {
  struct ncclIbHandle* handle = (struct ncclIbHandle*) opaqueHandle;
  struct ncclIbCommStage* stage = &handle->stage;
  struct ncclIbSendComm* comm = (struct ncclIbSendComm*)stage->comm;
  int ready;
  *sendComm = NULL;

  int myChId = (int)(uintptr_t)chId;
  int channelId = myChId;
  if (stage->state == ncclIbCommStateConnect)    goto ib_connect_check;
  if (stage->state == ncclIbCommStateSend)       goto ib_send;
  if (stage->state == ncclIbCommStateConnecting) goto ib_connect;
  if (stage->state == ncclIbCommStateConnected)  goto ib_send_ready;
  if (stage->state != ncclIbCommStateStart) {
    WARN("Error: trying to connect already connected sendComm");
    return ncclInternalError;
  }

  NCCLCHECK(ncclIbMalloc((void**)&comm, sizeof(struct ncclIbSendComm)));
  NCCLCHECK(ncclSocketInit(&comm->base.sock, &handle->connectAddr, handle->magic, ncclSocketTypeNetIb, NULL, 1));
  stage->comm = comm;
  stage->state = ncclIbCommStateConnect;
  NCCLCHECK(ncclSocketConnect(&comm->base.sock));

ib_connect_check:
  /* since ncclSocketConnect is async, we must check if connection is complete */
  NCCLCHECK(ncclSocketReady(&comm->base.sock, &ready));
  if (!ready) return ncclSuccess;

  // IB Setup
  struct ncclIbMergedDev* mergedDev;
  mergedDev = ncclIbMergedDevs + dev;
  comm->base.ndevs = mergedDev->ndevs;
  comm->base.nqps = ncclParamIbQpsPerConn() * comm->base.ndevs; // We must have at least 1 qp per-device
  comm->base.isSend = true;

  ANP_TELEMETRY_EXECUTE(
    g_anp_state.set_device_name(dev, "", mergedDev->devName);
  );
  // Init PD, Ctx for each IB device
  comm->ar = 1; // Set to 1 for logic
  for (int i = 0; i < mergedDev->ndevs; i++) {
    int ibDevN = mergedDev->devs[i];
    NCCLCHECK(ncclIbInitCommDevBase(ibDevN, &comm->devs[i].base));
    comm->ar = comm->ar && ncclIbDevs[dev].ar; // ADAPTIVE_ROUTING - if all merged devs have it enabled
  }

  struct ncclIbConnectionMetadata meta;
  meta.ndevs = comm->base.ndevs;

  // Alternate QPs between devices
  int devIndex;
  devIndex = 0;
  for (int q = 0; q < comm->base.nqps; q++) {
    ncclIbSendCommDev* commDev = comm->devs + devIndex;
    ncclIbDev* ibDev = ncclIbDevs + commDev->base.ibDevN;
    NCCLCHECK(ncclIbCreateQp(ibDev->portNum, &commDev->base, IBV_ACCESS_REMOTE_WRITE, comm->base.qps+q, channelId, true, q));
    comm->base.qps[q].devIndex = devIndex;
    meta.qpInfo[q].qpn      = comm->base.qps[q].qp->qp_num;
    meta.qpInfo[q].devIndex = comm->base.qps[q].devIndex;
#ifdef ANP_DEBUG_TRACE_EN
    INFO(NCCL_NET, "[ANP_TRACE] Created CTS QP %d, ch %d, dev index %d",
         comm->base.qps[q].qp->qp_num, channelId, comm->base.qps[q].devIndex);
#endif

    // Query ece capabilities (enhanced connection establishment)
    NCCLCHECK(wrap_ibv_query_ece(comm->base.qps[q].qp, &meta.qpInfo[q].ece, &meta.qpInfo[q].ece_supported));
    devIndex = (devIndex + 1) % comm->base.ndevs;
  }

  for (int i = 0; i < comm->base.ndevs; i++) {
    ncclIbSendCommDev* commDev = comm->devs + i;
    ncclIbDev* ibDev = ncclIbDevs + commDev->base.ibDevN;

    // Write to the metadata struct via this pointer
    ncclIbDevInfo* devInfo = meta.devs + i;
    devInfo->ib_port       = ibDev->portNum;
    devInfo->mtu           = ibDev->portAttr.active_mtu;
    devInfo->lid           = ibDev->portAttr.lid;
    devInfo->ibv_dev_index = commDev->base.ibDevN;
    // Prepare my fifo
    NCCLCHECK(wrap_ibv_reg_mr(&commDev->fifoMr, commDev->base.pd, comm->fifo, sizeof(struct ncclIbSendFifo)*MAX_REQUESTS*NCCL_NET_IB_MAX_RECVS, IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ));
    devInfo->fifoRkey = commDev->fifoMr->rkey;

    // Pack local GID info
    devInfo->link_layer = commDev->base.gidInfo.link_layer = ibDev->portAttr.link_layer;
    if (devInfo->link_layer == IBV_LINK_LAYER_ETHERNET) {
      NCCLCHECK(ncclIbGetGidIndex(ibDev->context, ibDev->portNum, &ibDev->portAttr, &commDev->base.gidInfo.localGidIndex));
      NCCLCHECK(wrap_ibv_query_gid(ibDev->context, ibDev->portNum, commDev->base.gidInfo.localGidIndex, &commDev->base.gidInfo.localGid));
      devInfo->gid.global.subnet_prefix = commDev->base.gidInfo.localGid.global.subnet_prefix;
      devInfo->gid.global.interface_id = commDev->base.gidInfo.localGid.global.interface_id;
    }

    // info logging
    if (devInfo->link_layer == IBV_LINK_LAYER_INFINIBAND) { // IB
      for (int q = 0; q < comm->base.nqps; q++) {
        // Print just the QPs for this dev
        if (comm->base.qps[q].devIndex == i)
          INFO(NCCL_NET,"NET/IB: %s %d IbDev %d Port %d qpn %d mtu %d LID %d subnet-prefix %llu  FLID %d fifoRkey=0x%x fifoLkey=0x%x",
            comm->base.ndevs > 2 ? "NCCL MergedDev" : "NCCL Dev",
            dev, commDev->base.ibDevN, ibDev->portNum, meta.qpInfo[q].qpn, devInfo->mtu, devInfo->lid,
	    devInfo->gid.global.subnet_prefix, ncclIbExtractFlid(&devInfo->gid), devInfo->fifoRkey, commDev->fifoMr->lkey);
      }
    } else { // RoCE
      for (int q = 0; q < comm->base.nqps; q++) {
        // Print just the QPs for this dev
        if (comm->base.qps[q].devIndex == i)
          INFO(NCCL_NET,"NET/IB: %s %d IbDev %d Port %d qpn %d mtu %d query_ece={supported=%d, vendor_id=0x%x, options=0x%x, comp_mask=0x%x} GID %ld (%llX/%llX) fifoRkey=0x%x fifoLkey=0x%x",
            comm->base.ndevs > 2 ? "NCCL MergedDev" : "NCCL Dev", dev,
            commDev->base.ibDevN, ibDev->portNum, meta.qpInfo[q].qpn, devInfo->mtu, meta.qpInfo[q].ece_supported, meta.qpInfo[q].ece.vendor_id, meta.qpInfo[q].ece.options, meta.qpInfo[q].ece.comp_mask, (int64_t)commDev->base.gidInfo.localGidIndex,
            devInfo->gid.global.subnet_prefix, devInfo->gid.global.interface_id, devInfo->fifoRkey, commDev->fifoMr->lkey);
      }
    }
  }
  meta.fifoAddr = (uint64_t)comm->fifo;
  strncpy(meta.devName, mergedDev->devName, MAX_MERGED_DEV_NAME);

  stage->state = ncclIbCommStateSend;
  stage->offset = 0;
  NCCLCHECK(ncclIbMalloc((void**)&stage->buffer, sizeof(meta)));

  memcpy(stage->buffer, &meta, sizeof(meta));

ib_send:
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_SEND, &comm->base.sock, stage->buffer, sizeof(meta), &stage->offset));
  if (stage->offset != sizeof(meta)) return ncclSuccess;

  stage->state = ncclIbCommStateConnecting;
  stage->offset = 0;
  // Clear the staging buffer for re-use
  memset(stage->buffer, 0, sizeof(meta));

ib_connect:
  struct ncclIbConnectionMetadata remMeta;
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_RECV, &comm->base.sock, stage->buffer, sizeof(ncclIbConnectionMetadata), &stage->offset));
  if (stage->offset != sizeof(remMeta)) return ncclSuccess;

  memcpy(&remMeta, stage->buffer, sizeof(ncclIbConnectionMetadata));

  comm->base.nRemDevs = remMeta.ndevs;
  if (comm->base.nRemDevs != comm->base.ndevs) {
    mergedDev = ncclIbMergedDevs + dev;
    WARN("NET/IB : Local mergedDev=%s has a different number of devices=%d as remoteDev=%s nRemDevs=%d",
      mergedDev->devName, comm->base.ndevs, remMeta.devName, comm->base.nRemDevs);
  }

  int link_layer;
  link_layer = remMeta.devs[0].link_layer;
  for (int i = 1; i < remMeta.ndevs; i++) {
    if (remMeta.devs[i].link_layer != link_layer) {
      WARN("NET/IB : Can't merge net devices with different link_layer. i=%d remMeta.ndevs=%d link_layer=%d rem_link_layer=%d",
      i, remMeta.ndevs, link_layer, remMeta.devs[i].link_layer);
      return ncclInternalError;
    }
  }

  // Copy remDevInfo for things like remGidInfo, remFifoAddr, etc.
  for (int i = 0; i < remMeta.ndevs; i++) {
    comm->base.remDevs[i] = remMeta.devs[i];
    comm->base.remDevs[i].remoteGid.global.interface_id = comm->base.remDevs[i].gid.global.interface_id;
    comm->base.remDevs[i].remoteGid.global.subnet_prefix = comm->base.remDevs[i].gid.global.subnet_prefix;
    // Retain remote sizes fifo info and prepare RDMA ops
    comm->remSizesFifo.rkeys[i] = remMeta.devs[i].fifoRkey;
    comm->remSizesFifo.addr = remMeta.fifoAddr;
  }

  for (int i=0; i < comm->base.ndevs; i++) {
    NCCLCHECK(wrap_ibv_reg_mr(comm->remSizesFifo.mrs+i, comm->devs[i].base.pd, &comm->remSizesFifo.elems, sizeof(int)*MAX_REQUESTS*NCCL_NET_IB_MAX_RECVS, IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ));
  }
  comm->base.nRemDevs = remMeta.ndevs;

  for (int q = 0; q < comm->base.nqps; q++) {
    struct ncclIbQpInfo* remQpInfo   = remMeta.qpInfo + q;
    struct ncclIbDevInfo* remDevInfo = remMeta.devs + remQpInfo->devIndex;

    // Assign per-QP remDev
    comm->base.qps[q].remDevIdx = remQpInfo->devIndex;
    int devIndex = comm->base.qps[q].devIndex;
    ncclIbSendCommDev* commDev = comm->devs + devIndex;

    struct ibv_qp* qp = comm->base.qps[q].qp;
    if (remQpInfo->ece_supported)
      NCCLCHECK(wrap_ibv_set_ece(qp, &remQpInfo->ece, &remQpInfo->ece_supported));

    NCCLCHECK(ncclIbRtrQp(qp, &commDev->base.gidInfo, remQpInfo->qpn, remDevInfo, false));
    NCCLCHECK(ncclIbRtsQp(qp));
#ifdef ANP_DEBUG_TRACE_EN
    INFO(NCCL_NET, "[ANP_TRACE] sendcomm %p, ch %d, %s qp %d, local nic %d, peer nic %d",
         comm, comm->base.qps[q].channelId, comm->base.qps[q].data ? "data" : "cts",
         comm->base.qps[q].qp->qp_num,
         comm->devs[comm->base.qps[q].devIndex].base.ibDevN,
         comm->base.remDevs[comm->base.qps[q].remDevIdx].ibv_dev_index);
#endif
  }

  if (link_layer == IBV_LINK_LAYER_ETHERNET ) { // RoCE
    for (int q = 0; q < comm->base.nqps; q++) {
      struct ncclIbQp* qp = comm->base.qps + q;
      int ibDevN = comm->devs[qp->devIndex].base.ibDevN;
      struct ncclIbDev* ibDev = ncclIbDevs + ibDevN;
      INFO(NCCL_NET,"NET/IB: IbDev %d Port %d qpn %d set_ece={supported=%d, vendor_id=0x%x, options=0x%x, comp_mask=0x%x}",
        ibDevN, ibDev->portNum, remMeta.qpInfo[q].qpn, remMeta.qpInfo[q].ece_supported, remMeta.qpInfo[q].ece.vendor_id, remMeta.qpInfo[q].ece.options, remMeta.qpInfo[q].ece.comp_mask);
    }
  }

  comm->base.ready = 1;
  stage->state = ncclIbCommStateConnected;
  stage->offset = 0;

ib_send_ready:
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_SEND, &comm->base.sock, &comm->base.ready, sizeof(int), &stage->offset));
  if (stage->offset != sizeof(int)) return ncclSuccess;

  free(stage->buffer);
  stage->state = ncclIbCommStateStart;

  *sendComm = comm;
  return ncclSuccess;
}

NCCL_PARAM(IbGdrFlushDisable, "GDR_FLUSH_DISABLE", 0);
NCCL_PARAM(IbGdrFlushGpuMemNoRelaxedOrdering, "GDR_FLUSH_GPU_MEM_NO_RELAXED_ORDERING", 1);

//ncclResult_t anpNetAccept(void* listenComm, void** recvComm, ncclNetDeviceHandle_t** /*recvDevComm*/) {
ncclResult_t anpNetAccept(void* listenComm, void** recvComm, ncclNetDeviceHandle_t** chId) {
  struct ncclIbListenComm* lComm = (struct ncclIbListenComm*)listenComm;
  struct ncclIbCommStage* stage = &lComm->stage;
  struct ncclIbRecvComm* rComm = (struct ncclIbRecvComm*)stage->comm;
  int ready;
  *recvComm = NULL;

  int myChId = (int)(uintptr_t)chId;
  int channelId = myChId;
  if (stage->state == ncclIbCommStateAccept) goto ib_accept_check;
  if (stage->state == ncclIbCommStateRecv) goto ib_recv;
  if (stage->state == ncclIbCommStateSend) goto ib_send;
  if (stage->state == ncclIbCommStatePendingReady) goto ib_recv_ready;
  if (stage->state != ncclIbCommStateStart) {
    WARN("Listencomm in unknown state %d", stage->state);
    return ncclInternalError;
  }

  NCCLCHECK(ncclIbMalloc((void**)&rComm, sizeof(struct ncclIbRecvComm)));
  stage->comm = rComm;
  stage->state = ncclIbCommStateAccept;
  NCCLCHECK(ncclSocketInit(&rComm->base.sock));
  NCCLCHECK(ncclSocketAccept(&rComm->base.sock, &lComm->sock));

ib_accept_check:
  NCCLCHECK(ncclSocketReady(&rComm->base.sock, &ready));
  if (!ready) return ncclSuccess;

  struct ncclIbConnectionMetadata remMeta;
  stage->state = ncclIbCommStateRecv;
  stage->offset = 0;
  NCCLCHECK(ncclIbMalloc((void**)&stage->buffer, sizeof(remMeta)));

ib_recv:
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_RECV, &rComm->base.sock, stage->buffer, sizeof(remMeta), &stage->offset));
  if (stage->offset != sizeof(remMeta)) return ncclSuccess;

  /* copy back the received info */
  memcpy(&remMeta, stage->buffer, sizeof(struct ncclIbConnectionMetadata));

  // IB setup
  // Pre-declare variables because of goto
  struct ncclIbMergedDev* mergedDev;
  struct ncclIbDev* ibDev;
  int ibDevN;
  struct ncclIbRecvCommDev* rCommDev;
  struct ncclIbDevInfo* remDevInfo;
  struct ncclIbQp* qp;

  mergedDev = ncclIbMergedDevs + lComm->dev;
  rComm->base.ndevs = mergedDev->ndevs;
  rComm->base.nqps  = ncclParamIbQpsPerConn() * rComm->base.ndevs; // We must have at least 1 qp per-device
  rComm->base.isSend = false;

  rComm->base.nRemDevs = remMeta.ndevs;
  if (rComm->base.nRemDevs != rComm->base.ndevs) {
    WARN("NET/IB : Local mergedDev %s has a different number of devices=%d as remote %s %d",
      mergedDev->devName, rComm->base.ndevs, remMeta.devName, rComm->base.nRemDevs);
  }

  // Metadata to send back to requestor (sender)
  struct ncclIbConnectionMetadata meta;
  for (int i = 0; i < rComm->base.ndevs; i++) {
    rCommDev = rComm->devs + i;
    ibDevN = mergedDev->devs[i];
    NCCLCHECK(ncclIbInitCommDevBase(ibDevN, &rCommDev->base));
    ibDev = ncclIbDevs + ibDevN;
    NCCLCHECK(ncclIbGetGidIndex(ibDev->context, ibDev->portNum, &ibDev->portAttr, &rCommDev->base.gidInfo.localGidIndex));
    NCCLCHECK(wrap_ibv_query_gid(ibDev->context, ibDev->portNum, rCommDev->base.gidInfo.localGidIndex, &rCommDev->base.gidInfo.localGid));
  }

  // Copy remDevInfo for things like remGidInfo, remFifoAddr, etc.
  for (int i = 0; i < remMeta.ndevs; i++) {
    rComm->base.remDevs[i] = remMeta.devs[i];
    rComm->base.remDevs[i].remoteGid.global.interface_id  = rComm->base.remDevs[i].gid.global.interface_id;
    rComm->base.remDevs[i].remoteGid.global.subnet_prefix = rComm->base.remDevs[i].gid.global.subnet_prefix;
  }

  // Stripe QP creation across merged devs
  // Make sure to get correct remote peer dev and QP info
  int remDevIndex;
  int devIndex;
  devIndex = 0;
  for (int q = 0; q < rComm->base.nqps; q++) {
    remDevIndex = remMeta.qpInfo[q].devIndex;
    remDevInfo = remMeta.devs + remDevIndex;
    qp = rComm->base.qps+q;
    rCommDev = rComm->devs + devIndex;
    qp->remDevIdx = remDevIndex;

    // Local ibDevN
    ibDevN = rComm->devs[devIndex].base.ibDevN;
    ibDev = ncclIbDevs + ibDevN;
    NCCLCHECK(ncclIbCreateQp(ibDev->portNum, &rCommDev->base, IBV_ACCESS_REMOTE_WRITE, qp, channelId, false, q));
    qp->devIndex = devIndex;
    devIndex = (devIndex + 1) % rComm->base.ndevs;

    // Set the ece (enhanced connection establishment) on this QP before RTR
    if (remMeta.qpInfo[q].ece_supported) {
      NCCLCHECK(wrap_ibv_set_ece(qp->qp, &remMeta.qpInfo[q].ece, &meta.qpInfo[q].ece_supported));

      // Query the reduced ece for this QP (matching enhancements between the requestor and the responder)
      // Store this in our own qpInfo for returning to the requestor
      if (meta.qpInfo[q].ece_supported)
        NCCLCHECK(wrap_ibv_query_ece(qp->qp, &meta.qpInfo[q].ece, &meta.qpInfo[q].ece_supported));
    }

    bool override_tc = (q == 0) ? true : false;
    NCCLCHECK(ncclIbRtrQp(qp->qp, &rCommDev->base.gidInfo, remMeta.qpInfo[q].qpn, remDevInfo, override_tc));
    NCCLCHECK(ncclIbRtsQp(qp->qp));
#ifdef ANP_DEBUG_TRACE_EN
    INFO(NCCL_NET, "[ANP_TRACE] recvcomm %p, ch %d, %s qp %d, local nic %d, peer nic %d",
         rComm, qp->channelId, qp->data ? "data" : "cts", qp->qp->qp_num,
         ibDevN, rComm->base.remDevs[qp->remDevIdx].ibv_dev_index);
#endif
  }

  rComm->flushEnabled = ((ncclIbGdrSupport() == ncclSuccess || ncclIbDmaBufSupport(lComm->dev) == ncclSuccess)
                            && (ncclParamIbGdrFlushDisable() == 0)) ? 1 : 0;

  for (int i = 0; i < mergedDev->ndevs; i++) {
    rCommDev = rComm->devs + i;
    ibDevN = rCommDev->base.ibDevN;
    ibDev = ncclIbDevs + ibDevN;

    // Retain remote fifo info and prepare my RDMA ops
    rCommDev->fifoRkey = remMeta.devs[i].fifoRkey;
    rComm->remFifo.addr = remMeta.fifoAddr;
    NCCLCHECK(wrap_ibv_reg_mr(&rCommDev->fifoMr, rCommDev->base.pd, &rComm->remFifo.elems, sizeof(struct ncclIbSendFifo)*MAX_REQUESTS*NCCL_NET_IB_MAX_RECVS, IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ));
    rCommDev->fifoSge.lkey = rCommDev->fifoMr->lkey;
    if (ncclParamIbUseInline()) rComm->remFifo.flags = IBV_SEND_INLINE;

    // Allocate Flush dummy buffer for GPU Direct RDMA
    if (rComm->flushEnabled) {
      if (ncclParamIbGdrFlushGpuMemNoRelaxedOrdering()) {
#if defined(HIP_UNCACHED_MEMORY)
        NCCLCHECK(ncclCudaCalloc(&rCommDev->gpuFlush.gpuFlushGpuMem, sizeof(int), nullptr, hipDeviceMallocUncached));
#else
        NCCLCHECK(ncclCudaCalloc(&rCommDev->gpuFlush.gpuFlushGpuMem, sizeof(int), nullptr, hipDeviceMallocFinegrained));
#endif
        NCCLCHECK(wrap_ibv_reg_mr(&rCommDev->gpuFlush.gpuMr, rCommDev->base.pd, rCommDev->gpuFlush.gpuFlushGpuMem, sizeof(int), IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ));
      } else {
        rCommDev->gpuFlush.gpuFlushGpuMem = nullptr;
        rCommDev->gpuFlush.gpuMr = nullptr;
      }
      NCCLCHECK(wrap_ibv_reg_mr(&rCommDev->gpuFlush.hostMr, rCommDev->base.pd, &rComm->gpuFlushHostMem, sizeof(int), IBV_ACCESS_LOCAL_WRITE));
      rCommDev->gpuFlush.sge.addr = (uint64_t)&rComm->gpuFlushHostMem;
      rCommDev->gpuFlush.sge.length = 1;
      rCommDev->gpuFlush.sge.lkey = rCommDev->gpuFlush.hostMr->lkey;
      NCCLCHECK(ncclIbCreateQp(ibDev->portNum, &rCommDev->base, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE, &rCommDev->gpuFlush.qp, channelId, true, 0xFF));
      struct ncclIbDevInfo devInfo;
      devInfo.lid         = ibDev->portAttr.lid;
      devInfo.link_layer  = ibDev->portAttr.link_layer;
      devInfo.ib_port     = ibDev->portNum;
      devInfo.gid.global.subnet_prefix        = rCommDev->base.gidInfo.localGid.global.subnet_prefix;
      devInfo.gid.global.interface_id         = rCommDev->base.gidInfo.localGid.global.interface_id;
      devInfo.mtu         = ibDev->portAttr.active_mtu;
      NCCLCHECK(ncclIbRtrQp(rCommDev->gpuFlush.qp.qp, &rCommDev->base.gidInfo, rCommDev->gpuFlush.qp.qp->qp_num, &devInfo, false));
      NCCLCHECK(ncclIbRtsQp(rCommDev->gpuFlush.qp.qp));
    }

    // Fill Handle
    meta.devs[i].lid        = ibDev->portAttr.lid;
    meta.devs[i].link_layer = rCommDev->base.gidInfo.link_layer = ibDev->portAttr.link_layer;
    meta.devs[i].ib_port    = ibDev->portNum;
    meta.devs[i].gid.global.subnet_prefix       = rCommDev->base.gidInfo.localGid.global.subnet_prefix;
    meta.devs[i].gid.global.interface_id        = rCommDev->base.gidInfo.localGid.global.interface_id;
    meta.devs[i].ibv_dev_index                  = rCommDev->base.ibDevN;

    // Adjust the MTU
    remMeta.devs[i].mtu    = (enum ibv_mtu) std::min(remMeta.devs[i].mtu, ibDev->portAttr.active_mtu);
    meta.devs[i].mtu      = remMeta.devs[i].mtu;

    // Prepare sizes fifo
    NCCLCHECK(wrap_ibv_reg_mr(&rComm->devs[i].sizesFifoMr, rComm->devs[i].base.pd, rComm->sizesFifo, sizeof(int)*MAX_REQUESTS*NCCL_NET_IB_MAX_RECVS, IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ));
    meta.devs[i].fifoRkey = rComm->devs[i].sizesFifoMr->rkey;
  }
  meta.fifoAddr = (uint64_t)rComm->sizesFifo;

  for (int q = 0; q < rComm->base.nqps; q++) {
    meta.qpInfo[q].qpn      = rComm->base.qps[q].qp->qp_num;
    meta.qpInfo[q].devIndex = rComm->base.qps[q].devIndex;
  }

  meta.ndevs = rComm->base.ndevs;
  strncpy(meta.devName, mergedDev->devName, MAX_MERGED_DEV_NAME);

  stage->state = ncclIbCommStateSend;
  stage->offset = 0;
  if (stage->buffer) free(stage->buffer);
  NCCLCHECK(ncclIbMalloc((void**)&stage->buffer, sizeof(struct ncclIbConnectionMetadata)));
  memcpy(stage->buffer, &meta, sizeof(struct ncclIbConnectionMetadata));

ib_send:
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_SEND, &rComm->base.sock, stage->buffer, sizeof(struct ncclIbConnectionMetadata), &stage->offset));
  if (stage->offset < sizeof(struct ncclIbConnectionMetadata)) return ncclSuccess;

  stage->offset = 0;
  stage->state = ncclIbCommStatePendingReady;

ib_recv_ready:
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_RECV,  &rComm->base.sock, &rComm->base.ready, sizeof(int), &stage->offset));
  if (stage->offset != sizeof(int)) return ncclSuccess;

  free(stage->buffer);
  *recvComm = rComm;

  /* reset lComm stage */
  stage->state = ncclIbCommStateStart;
  stage->offset = 0;
  stage->comm = NULL;
  stage->buffer = NULL;
  return ncclSuccess;
}

ncclResult_t ncclIbGetRequest(struct ncclIbNetCommBase* base, struct ncclIbRequest** req) {
  for (int i=0; i<MAX_REQUESTS; i++) {
    struct ncclIbRequest* r = base->reqs+i;
    if (r->type == NCCL_NET_IB_REQ_UNUSED) {
      r->base = base;
      r->sock = NULL;
      r->devBases[0] = NULL;
      r->devBases[1] = NULL;
      r->events[0] = r->events[1] = 0;
      *req = r;
      return ncclSuccess;
    }
  }
  WARN("NET/IB : unable to allocate requests");
  *req = NULL;
  return ncclInternalError;
}

ncclResult_t ncclIbFreeRequest(struct ncclIbRequest* r) {
  r->type = NCCL_NET_IB_REQ_UNUSED;
  return ncclSuccess;
}

ncclResult_t ncclIbTest(void* request, int* done, int* size);

ncclResult_t ncclIbRegMrDmaBufInternal(ncclIbNetCommDevBase* base, void* data, size_t size, int type, uint64_t offset, int fd, ibv_mr** mhandle) {
  static __thread uintptr_t pageSize = 0;
  if (pageSize == 0) pageSize = sysconf(_SC_PAGESIZE);
  struct ncclIbMrCache* cache = &ncclIbDevs[base->ibDevN].mrCache;
  uintptr_t addr = (uintptr_t)data & -pageSize;
  size_t pages = ((uintptr_t)data + size - addr + pageSize-1)/pageSize;
  ncclResult_t res;
  pthread_mutex_lock(&ncclIbDevs[base->ibDevN].lock);
  for (int slot=0; /*true*/; slot++) {
    if (slot == cache->population || addr < cache->slots[slot].addr) { // didn't find in cache
      if (cache->population == cache->capacity) { // must grow cache
        cache->capacity = cache->capacity < 32 ? 32 : 2*cache->capacity;
        NCCLCHECKGOTO(ncclRealloc(&cache->slots, cache->population, cache->capacity), res, returning);
      }
      // Deregister / register
      struct ibv_mr* mr;
      unsigned int flags = IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ;
      if (ncclIbRelaxedOrderingEnabled) flags |= IBV_ACCESS_RELAXED_ORDERING;
      if (fd != -1) {
        /* DMA-BUF support */
        NCCLCHECKGOTO(wrap_ibv_reg_dmabuf_mr(&mr, base->pd, offset, pages*pageSize, addr, fd, flags), res, returning);
      } else {
        if (ncclIbRelaxedOrderingEnabled) {
          // Use IBVERBS_1.8 API - needed for IBV_ACCESS_RELAXED_ORDERING support
          NCCLCHECKGOTO(wrap_ibv_reg_mr_iova2(&mr, base->pd, (void*)addr, pages*pageSize, addr, flags), res, returning);
        }
        else {
          NCCLCHECKGOTO(wrap_ibv_reg_mr(&mr, base->pd, (void*)addr, pages*pageSize, flags), res, returning);
        }
      }
      TRACE(NCCL_INIT|NCCL_NET,"regAddr=0x%lx size=%lld rkey=0x%x lkey=0x%x fd=%d", (unsigned long)addr, (long long)pages*pageSize, mr->rkey, mr->lkey, fd);
      if (slot != cache->population) memmove(cache->slots+slot+1, cache->slots+slot, (cache->population-slot)*sizeof(struct ncclIbMr));
      cache->slots[slot].addr = addr;
      cache->slots[slot].pages = pages;
      cache->slots[slot].refs = 1;
      cache->slots[slot].mr = mr;
      cache->population += 1;
      *mhandle = mr;
      res = ncclSuccess;
      goto returning;
    } else if ((addr >= cache->slots[slot].addr) &&
        ((addr-cache->slots[slot].addr)/pageSize+pages) <= cache->slots[slot].pages) {
      cache->slots[slot].refs += 1;
      *mhandle = cache->slots[slot].mr;
      res = ncclSuccess;
      goto returning;
    }
  }
returning:
  pthread_mutex_unlock(&ncclIbDevs[base->ibDevN].lock);
  return res;
}

struct ncclIbNetCommDevBase* ncclIbGetNetCommDevBase(ncclIbNetCommBase* base, int devIndex) {
  if (base->isSend) {
    struct ncclIbSendComm* sComm = (struct ncclIbSendComm*) base;
    return &sComm->devs[devIndex].base;
  } else {
    struct ncclIbRecvComm* rComm = (struct ncclIbRecvComm*) base;
    return &rComm->devs[devIndex].base;
  }
}

/* DMA-BUF support */
ncclResult_t ncclIbRegMrDmaBuf(void* comm, void* data, size_t size, int type, uint64_t offset, int fd, void** mhandle) {
  assert(size > 0);
  struct ncclIbNetCommBase* base = (struct ncclIbNetCommBase*) comm;
  struct ncclIbMrHandle* mhandleWrapper = (struct ncclIbMrHandle*) malloc(sizeof(struct ncclIbMrHandle));
  for (int i = 0; i < base->ndevs; i++) {
    // Each ncclIbNetCommDevBase is at different offset in send and recv netComms
    struct ncclIbNetCommDevBase* devComm = ncclIbGetNetCommDevBase(base, i);
    NCCLCHECK(ncclIbRegMrDmaBufInternal(devComm, data, size, type, offset, fd, mhandleWrapper->mrs + i));
  }
  *mhandle = (void*) mhandleWrapper;
  return ncclSuccess;
}

ncclResult_t anpNetRegMr(void* comm, void* data, size_t size, int type, void** mhandle) {
  return ncclIbRegMrDmaBuf(comm, data, size, type, 0ULL, -1, mhandle);
}

ncclResult_t ncclIbDeregMrInternal(ncclIbNetCommDevBase* base, ibv_mr* mhandle) {
  struct ncclIbMrCache* cache = &ncclIbDevs[base->ibDevN].mrCache;
  ncclResult_t res;
  pthread_mutex_lock(&ncclIbDevs[base->ibDevN].lock);
  for (int i=0; i < cache->population; i++) {
    if (mhandle == cache->slots[i].mr) {
      if (0 == --cache->slots[i].refs) {
        memmove(&cache->slots[i], &cache->slots[--cache->population], sizeof(struct ncclIbMr));
        if (cache->population == 0) {
          free(cache->slots);
          cache->slots = NULL;
          cache->capacity = 0;
        }
        NCCLCHECKGOTO(wrap_ibv_dereg_mr(mhandle), res, returning);
      }
      res = ncclSuccess;
      goto returning;
    }
  }
  WARN("NET/IB: could not find mr %p inside cache of %d entries", mhandle, cache->population);
  res = ncclInternalError;
returning:
  pthread_mutex_unlock(&ncclIbDevs[base->ibDevN].lock);
  return res;
}

ncclResult_t anpNetDeregMr(void* comm, void* mhandle) {
  struct ncclIbMrHandle* mhandleWrapper = (struct ncclIbMrHandle*) mhandle;
  struct ncclIbNetCommBase* base = (struct ncclIbNetCommBase*) comm;
  for (int i = 0; i < base->ndevs; i++) {
    // Each ncclIbNetCommDevBase is at different offset in send and recv netComms
    struct ncclIbNetCommDevBase* devComm = ncclIbGetNetCommDevBase(base, i);
    NCCLCHECK(ncclIbDeregMrInternal(devComm, mhandleWrapper->mrs[i]));
  }
  free(mhandleWrapper);
  return ncclSuccess;
}

NCCL_PARAM(IbSplitDataOnQps, "IB_SPLIT_DATA_ON_QPS", 0);

ncclResult_t ncclIbMultiSend(struct ncclIbSendComm* comm, int slot, bool use_write_op) {
  uint32_t num_write = 0;
  struct ncclIbRequest** reqs = comm->fifoReqs[slot];
#if !defined(CTS_RCVR_OFFLOAD_ENABLED)
  volatile struct ncclIbSendFifo* slots = comm->fifo[slot];
  int nreqs = slots[0].nreqs;
#else
  int nreqs = 1;
#endif
  assert(nreqs == 1);
  if (nreqs > NCCL_NET_IB_MAX_RECVS) return ncclInternalError;

  uint64_t wr_id = 0ULL;
  for (int r=0; r<nreqs; r++) {
    struct ibv_send_wr* wr = comm->wrs+r;
    memset(wr, 0, sizeof(struct ibv_send_wr));

    struct ibv_sge* sge = comm->sges+r;
    sge->addr=(uintptr_t)reqs[r]->send.data;
    wr->opcode = IBV_WR_RDMA_WRITE;
    wr->send_flags = 0;
#if !defined(CTS_RCVR_OFFLOAD_ENABLED)
    wr->wr.rdma.remote_addr = slots[r].addr;
#else
    wr->wr.rdma.remote_addr = 0xdeadbeef;
#endif
    wr->next = wr + 1;
    wr_id += (reqs[r] - comm->base.reqs) << (r*8);
    num_write++;
  }

  // Write size as immediate data. In the case of multi-send, only write
  // 0 or 1 as size to indicate whether there was data sent or received.
  uint32_t immData = 0;
  if ((nreqs == 1) && (use_write_op == false)) {
    immData = reqs[0]->send.size;
  } else {
    int* sizes = comm->remSizesFifo.elems[slot];
    for (int r=0; r<nreqs; r++) sizes[r] = reqs[r]->send.size;
    comm->remSizesFifo.sge.addr = (uint64_t)sizes;
    comm->remSizesFifo.sge.length = nreqs*sizeof(int);
  }

  struct ibv_send_wr* lastWr = comm->wrs+nreqs-1;
  if (use_write_op == false) {
      if (nreqs > 1 || (comm->ar && reqs[0]->send.size > ncclParamIbArThreshold())) {
        // When using ADAPTIVE_ROUTING, send the bulk of the data first as an
        // RDMA_WRITE, then a 0-byte RDMA_WRITE_WITH_IMM to trigger a remote
        // completion.
        lastWr++;
        memset(lastWr, 0, sizeof(struct ibv_send_wr));
        if (nreqs > 1) {
          // Write remote sizes Fifo
          lastWr->wr.rdma.remote_addr = comm->remSizesFifo.addr + slot*NCCL_NET_IB_MAX_RECVS*sizeof(int);
          lastWr->num_sge = 1;
          lastWr->sg_list = &comm->remSizesFifo.sge;
        }
      } else {
          num_write--;
      }
      lastWr->wr_id = wr_id;
      lastWr->opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
      lastWr->imm_data = immData;
  } else {
      lastWr->wr_id = wr_id;
  }
  lastWr->next = NULL;
  lastWr->send_flags = IBV_SEND_SIGNALED;

  // Multi-QP: make sure IB writes are multiples of 128B so that LL and LL128 protocols still work
  const int align = 128;
  int nqps = ncclParamIbSplitDataOnQps() ? comm->base.nqps : comm->base.ndevs;
  assert(nqps == 1);
  for (int i = 0; i < nqps; i++) {
    int qpIndex = comm->base.qpIndex;
    ncclIbQp* qp = comm->base.qps + qpIndex;
    int devIndex = qp->devIndex;
    for (int r=0; r<nreqs; r++) {
      // Track this event for completion
      //ncclIbAddEvent(reqs[r], devIndex, &comm->devs[devIndex].base);

      // Select proper rkey (needed even for 0-size send)
#if !defined(CTS_RCVR_OFFLOAD_ENABLED)
      comm->wrs[r].wr.rdma.rkey = slots[r].rkeys[qp->remDevIdx];
#else
      comm->wrs[r].wr.rdma.rkey = 0xbade;
#endif
      int chunkSize = DIVUP(DIVUP(reqs[r]->send.size, nqps), align) * align;
      int length = std::min(reqs[r]->send.size-reqs[r]->send.offset, chunkSize);
      if (length <= 0) {
        comm->wrs[r].sg_list = NULL;
        comm->wrs[r].num_sge = 0;
      } else {
        // Select proper lkey
        comm->sges[r].lkey = reqs[r]->send.lkeys[devIndex];
        comm->sges[r].length = length;
        ANP_TELEMETRY_EXECUTE(
            g_anp_state.update_wqe_size_metrics(length);
        );
        comm->wrs[r].sg_list = comm->sges+r;
        comm->wrs[r].num_sge = 1;
      }
    }

    if ((use_write_op == false) && (nreqs > 1)) {
      // Also make sure lastWr writes remote sizes using the right lkey
      comm->remSizesFifo.sge.lkey = comm->remSizesFifo.mrs[devIndex]->lkey;
      lastWr->wr.rdma.rkey = comm->remSizesFifo.rkeys[devIndex];
    }

    struct ibv_send_wr* bad_wr;
    uint64_t start_time;

    ANP_TELEMETRY_EXECUTE(
        start_time = gettime_ns();
    );
    NCCLCHECK(wrap_ibv_post_send(qp->qp, comm->wrs, &bad_wr));
    ANP_TELEMETRY_EXECUTE(
        if (use_write_op) {
          g_debug_stats.num_wr_wqe++;
        } else {
          g_debug_stats.num_wi_wqe++;
        }
        g_anp_state.increment_num_write_wqe(qp->qp->qp_num, num_write);
        g_anp_state.increment_num_write_imm_wqe(qp->qp->qp_num);
        g_anp_state.update_wqe_send_metrics(qp->qp->qp_num, wr_id, start_time);
    );
    for (int r=0; r<nreqs; r++) {
      int chunkSize = DIVUP(DIVUP(reqs[r]->send.size, nqps), align) * align;
      reqs[r]->send.offset += chunkSize;
      comm->sges[r].addr += chunkSize;
      comm->wrs[r].wr.rdma.remote_addr += chunkSize;

#ifdef ANP_DEBUG_TRACE_EN
      INFO(NCCL_VERBS, "Posted send wr_id=%lu, wr_indx=%d, ch %d, qp_num=%d, src_nic=%d, dst_nic=%d, dlid=%d, opcode=%d, send_flags=%d, imm_data=%d, remote_addr=%lx, rkey=%x, length=%d, lkey=%x",
          comm->wrs[r].wr_id, r, qp->channelId, qp->qp->qp_num, comm->devs[qp->devIndex].base.ibDevN , comm->base.remDevs[qp->remDevIdx].ibv_dev_index, comm->base.remDevs[qp->remDevIdx].lid,
          comm->wrs[r].opcode, comm->wrs[r].send_flags, comm->wrs[r].imm_data, comm->wrs[r].wr.rdma.remote_addr,
          comm->wrs[r].wr.rdma.rkey,comm->wrs[r].sg_list ? comm->wrs[r].sg_list->length : 0, comm->wrs[r].sg_list ? comm->wrs[r].sg_list->lkey : 0);
#else
      TRACE(NCCL_VERBS, "Posted send wr_id=%lu, wr_indx=%d, qp_num=%d, src_nic=%d, dst_nic=%d, dlid=%d, opcode=%d, send_flags=%d, imm_data=%d, remote_addr=%lx, rkey=%x, length=%d, lkey=%x",
         comm->wrs[r].wr_id, r, qp->qp->qp_num, comm->devs[qp->devIndex].base.ibDevN , comm->base.remDevs[qp->remDevIdx].ibv_dev_index, comm->base.remDevs[qp->remDevIdx].lid,
         comm->wrs[r].opcode, comm->wrs[r].send_flags, comm->wrs[r].imm_data, comm->wrs[r].wr.rdma.remote_addr,
         comm->wrs[r].wr.rdma.rkey,comm->wrs[r].sg_list ? comm->wrs[r].sg_list->length : 0, comm->wrs[r].sg_list ? comm->wrs[r].sg_list->lkey : 0);
#endif
    }

    // Select the next qpIndex
    comm->base.qpIndex = (comm->base.qpIndex+1) % comm->base.nqps;
  }
  return ncclSuccess;
}

ncclResult_t anpNetIsend(void* sendComm, void* data, size_t size, int tag, void* mhandle, void** request) {
  struct ncclIbSendComm* comm = (struct ncclIbSendComm*)sendComm;
  if (comm->base.ready == 0) { WARN("NET/IB: ncclIbIsend() called when comm->base.ready == 0"); return ncclInternalError; }
  if (comm->base.ready == 0) { *request = NULL; return ncclSuccess; }

  bool use_write_op = (*request == NCCL_NET_USE_WRITE_OP) ? true : false;
  struct ncclIbMrHandle* mhandleWrapper = (struct ncclIbMrHandle*) mhandle;

#ifdef ANP_DEBUG_TRACE_EN
  INFO(NCCL_NET, "Processing send, sendComm %p, size %d, tag %d, use_write_op %d", sendComm, size, tag, use_write_op);
#endif
  // Wait for the receiver to have posted the corresponding receive
#if !defined(CTS_RCVR_OFFLOAD_ENABLED)
  int nreqs = 0;
  volatile struct ncclIbSendFifo* slots;
#else
  int nreqs = 1;
#endif
  int slot = (comm->fifoHead) % MAX_REQUESTS;
  struct ncclIbRequest** reqs = comm->fifoReqs[slot];
#if !defined(CTS_RCVR_OFFLOAD_ENABLED)
  slots = comm->fifo[slot];
  uint64_t idx = comm->fifoHead+1;
  if (slots[0].idx != idx) {
      *request = NULL;
      ANP_TELEMETRY_EXECUTE(
          g_anp_state.update_slot_miss_metrics(comm->base.qpIndex);
      );
      return ncclSuccess;
  }
  nreqs = slots[0].nreqs;
  // Wait until all data has arrived
  for (int r=1; r<nreqs; r++) while(slots[r].idx != idx);
  __sync_synchronize(); // order the nreqsPtr load against tag/rkey/addr loads below
#endif
  for (int r=0; r<nreqs; r++) {
#if !defined(CTS_RCVR_OFFLOAD_ENABLED)
    if (reqs[r] != NULL || slots[r].tag != tag) continue;

    if (size > slots[r].size) size = slots[r].size;
    // Sanity checks
    if (slots[r].size < 0 || slots[r].addr == 0 || slots[r].rkeys[0] == 0) {
      char line[SOCKET_NAME_MAXLEN + 1];
      union ncclSocketAddress addr;
      ncclSocketGetAddr(&comm->base.sock, &addr);
      WARN("NET/IB : req %d/%d tag %x peer %s posted incorrect receive info: size %d addr %lx rkeys[0]=%x",
        r, nreqs, tag, ncclSocketToString(&addr, line), slots[r].size, slots[r].addr, slots[r].rkeys[0]);
      return ncclInternalError;
    }
#else
    if (reqs[r] != NULL) continue;
#endif

    struct ncclIbRequest* req;
    NCCLCHECK(ncclIbGetRequest(&comm->base, &req));
    req->type = NCCL_NET_IB_REQ_SEND;
    req->sock = &comm->base.sock;
    req->base = &comm->base;
    req->nreqs = nreqs;
    req->send.size = size;
    req->send.data = data;
    req->send.offset = 0;

    // Populate events
    int nEvents = ncclParamIbSplitDataOnQps() ? comm->base.nqps : comm->base.ndevs;
    int qpIndex = comm->base.qpIndex;
    // Count down
    while (nEvents > 0) {
      ncclIbQp* qp = comm->base.qps + qpIndex;
      int devIndex = qp->devIndex;
      ncclIbAddEvent(req, devIndex, &comm->devs[devIndex].base);
      // Track the valid lkey for this RDMA_Write
      req->send.lkeys[devIndex] = mhandleWrapper->mrs[devIndex]->lkey;
      nEvents--;
      // Don't update comm->base.qpIndex yet, we need to run through this same set of QPs inside ncclIbMultiSend()
      qpIndex = (qpIndex+1)%comm->base.nqps;
    }

    // Store all lkeys
    for (int i = 0; i < comm->base.ndevs; i++) {
      req->send.lkeys[i] = mhandleWrapper->mrs[i]->lkey;
    }

    *request = reqs[r] = req;

    // If this is a multi-recv, send only when all requests have matched.
    for (int r=0; r<nreqs; r++) {
      if (reqs[r] == NULL) return ncclSuccess;
    }

    TIME_START(0);
    NCCLCHECK(ncclIbMultiSend(comm, slot, use_write_op));

    // Clear slots[0]->nreqs, as well as other fields to help debugging and sanity checks
#if !defined(CTS_RCVR_OFFLOAD_ENABLED)
    memset((void*)slots, 0, sizeof(struct ncclIbSendFifo));
#endif
    memset(reqs, 0, NCCL_NET_IB_MAX_RECVS*sizeof(struct ncclIbRequest*));
    comm->fifoHead++;
    TIME_STOP(0);
    return ncclSuccess;
  }

  *request = NULL;
  return ncclSuccess;
}

ncclResult_t ncclIbPostFifo(struct ncclIbRecvComm* comm, int n, void** data, size_t* sizes, int* tags, void** mhandles, struct ncclIbRequest* req) {
  bool signalled = false;
  struct ibv_send_wr wr;
  memset(&wr, 0, sizeof(wr));

  int slot = comm->remFifo.fifoTail%MAX_REQUESTS;
  req->recv.sizes = comm->sizesFifo[slot];
  for (int i=0; i<n; i++) req->recv.sizes[i] = 0;
  struct ncclIbSendFifo* localElem = comm->remFifo.elems[slot];

  // Select the next devIndex (local) and QP to use for posting this CTS message
  // Since QPs are initialized by striping across devIndex, we can simply assign this to the same value
  //ncclIbQp* ctsQp = comm->base.qps + comm->base.devIndex;
  //comm->base.devIndex = (comm->base.devIndex + 1) % comm->base.ndevs;
  int qpIndex = comm->base.qpIndex;
  ncclIbQp* ctsQp = comm->base.qps + qpIndex;

  for (int i=0; i<n; i++) {
    localElem[i].addr = (uint64_t)data[i];
    struct ncclIbMrHandle* mhandleWrapper = (struct ncclIbMrHandle*) mhandles[i];

    // Send all applicable rkeys
    for (int j = 0; j < comm->base.ndevs; j++)
      localElem[i].rkeys[j] = mhandleWrapper->mrs[j]->rkey;

    localElem[i].nreqs = n;
    localElem[i].size = sizes[i]; // Sanity/Debugging
    localElem[i].tag = tags[i];
    localElem[i].idx = comm->remFifo.fifoTail+1;
  }
  wr.wr.rdma.remote_addr = comm->remFifo.addr + slot*NCCL_NET_IB_MAX_RECVS*sizeof(struct ncclIbSendFifo);

  // Lookup the correct fifoRkey
  wr.wr.rdma.rkey = comm->base.remDevs[ctsQp->remDevIdx].fifoRkey;

  // Set the correct sge properties
  comm->devs[ctsQp->devIndex].fifoSge.addr   = (uint64_t)localElem;
  comm->devs[ctsQp->devIndex].fifoSge.length = MAX_INLINE_DATA_SIZE;
  wr.sg_list = &comm->devs[ctsQp->devIndex].fifoSge;
  wr.num_sge = 1;

  wr.opcode = IBV_WR_RDMA_WRITE;
  wr.send_flags = comm->remFifo.flags; // IBV_SEND_INLINE

  // We need to occasionally post a request with the IBV_SEND_SIGNALED flag, otherwise
  // the send queue will never empty.
  //
  // From https://www.rdmamojo.com/2014/06/30/working-unsignaled-completions/
  // "How to use Unsignaled Completion?" / "Gotchas and Pitfalls"
  // All posted Send Requested, Signaled and Unsignaled, are considered outstanding until
  // a Work Completion that they, or Send Requests that were posted after them, was polled
  // from the Completion Queue associated with the Send Queue. This means if one works with
  // a Queue Pair that was configured to work with Unsignaled Completions, he must make
  // sure that occasionally (before the Send Queue is full with outstanding Send Requests)
  // a Send Request that generate Work Completion will be posted.
  //
  // Not following this rule may lead to a case that the Send Queue is full with Send
  // Requests that won't generate Work Completion:
  //
  //  - The Send Queue is full, so no new Send Requests can be posted to it
  //  - The Send Queue can't be emptied, since no Work Completion can be generated anymore
  //    (the reason is that no Work Completion, that can generate Work Completion that
  //    polling it will empty the Send Queue, can be posted)
  //  - The status of all posted Send Request is considered unknown
  //
  // slot == devIndex - When writing to fifo slot N, and this QP lives on device index N, it should send signalled.
  // This works out that each fifo posting QP gets drained
  //if (slot == ctsQp->devIndex) {
  if (slot == ctsQp->ctsQpSlot) {
#ifdef ANP_DEBUG_TRACE_EN
    INFO(NCCL_NET, "Need to send signalled CTS, slot %d, dev idx %d, qp %d",
         slot, ctsQp->devIndex, ctsQp->qp->qp_num);
#endif
    signalled = true;
    wr.send_flags |= IBV_SEND_SIGNALED;
    wr.wr_id = req - comm->base.reqs;
    ncclIbAddEvent(req, ctsQp->devIndex, &comm->devs[ctsQp->devIndex].base);
  }

  struct ibv_send_wr* bad_wr;
  NCCLCHECK(wrap_ibv_post_send(ctsQp->qp, &wr, &bad_wr));

#ifdef ANP_DEBUG_TRACE_EN
  INFO(NCCL_VERBS,
       "Posted CTS send %s, slot %d, fifoTail %lu, wr_id=%lu, wr_indx=%d, ch %d, qp_num=%d, src_nic=%d, dst_nic=%d, dlid=%u, opcode=%d, send_flags=%d, imm_data=%d, "
       "remote_addr=%lx, rkey=%x, length=%d, lkey=%x",
       (wr.send_flags & IBV_SEND_SIGNALED) ? "signaled" : "unsignaled", slot, comm->remFifo.fifoTail,
       wr.wr_id, 0, ctsQp->channelId, ctsQp->qp->qp_num, comm->devs[ctsQp->devIndex].base.ibDevN, comm->base.remDevs[ctsQp->remDevIdx].ibv_dev_index,
       comm->base.remDevs[ctsQp->remDevIdx].lid, wr.opcode, wr.send_flags, wr.imm_data, wr.wr.rdma.remote_addr, wr.wr.rdma.rkey, wr.sg_list ? wr.sg_list->length : 0,
       wr.sg_list ? wr.sg_list->lkey : 0);
#else
  TRACE(NCCL_VERBS, "Posted send wr_id=%lu, wr_indx=%d, qp_num=%d, src_nic=%d, dst_nic=%d, dlid=%u, opcode=%d, send_flags=%d, imm_data=%d, remote_addr=%lx, rkey=%x, length=%d, lkey=%x",
        wr.wr_id, 0, ctsQp->qp->qp_num, comm->devs[ctsQp->devIndex].base.ibDevN, comm->base.remDevs[ctsQp->remDevIdx].ibv_dev_index, comm->base.remDevs[ctsQp->remDevIdx].lid,
        wr.opcode, wr.send_flags, wr.imm_data, wr.wr.rdma.remote_addr, wr.wr.rdma.rkey, wr.sg_list ? wr.sg_list->length : 0, wr.sg_list ? wr.sg_list->lkey : 0);
#endif

  ANP_TELEMETRY_EXECUTE(
    g_anp_state.update_cts_send_metrics(ctsQp->qp->qp_num);
    g_debug_stats.num_cts_sent++;
    if (signalled) {
        g_debug_stats.num_signalled_cts_sent++;
    }
  );
  ANP_TELEMETRY_EXECUTE(
      if (signalled) {
          g_anp_state.increment_num_cts_sent_signalled(ctsQp->qp->qp_num);
      } else {
          g_anp_state.increment_num_cts_sent_unsignalled(ctsQp->qp->qp_num);
      }
  );
  comm->remFifo.fifoTail++;

  // Select the next qpIndex
  comm->base.qpIndex = (comm->base.qpIndex+1) % comm->base.nqps;
  return ncclSuccess;
}

ncclResult_t anpNetIrecvDefault(void* recvComm, int n, void** data, size_t* sizes, int* tags, void** mhandles, void** request) {
  ncclResult_t res = ncclSuccess;
  struct ncclIbRecvComm* comm = (struct ncclIbRecvComm*)recvComm;
  if (comm->base.ready == 0) { WARN("NET/IB: ncclIbIrecv() called when comm->base.ready == 0"); return ncclInternalError; }
  if (comm->base.ready == 0) { *request = NULL; return ncclSuccess; }
  if (n > NCCL_NET_IB_MAX_RECVS) return ncclInternalError;

  struct ncclIbRequest* req = NULL;
  NCCLCHECK(ncclIbGetRequest(&comm->base, &req));
  req->type = NCCL_NET_IB_REQ_RECV;
  req->sock = &comm->base.sock;
  req->nreqs = n;

  for (int i = 0; i < comm->base.ndevs; i++) {
    req->devBases[i] = &comm->devs[i].base;
  }

  struct ibv_recv_wr wr;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = req - comm->base.reqs;
  wr.sg_list = NULL;
  wr.num_sge = 0;

  TIME_START(1);
  // Select either all QPs, or one qp per-device
  const int nqps = ncclParamIbSplitDataOnQps() ? comm->base.nqps : comm->base.ndevs;

  // Post recvs
  struct ibv_recv_wr* bad_wr;
  int qpIndex = comm->base.qpIndex;
  for (int i = 0; i < nqps; i++) {
    struct ncclIbQp* qp = comm->base.qps + comm->base.qpIndex;
    ncclIbAddEvent(req, qp->devIndex, &comm->devs[qp->devIndex].base);
    if (wrap_ibv_post_recv(qp->qp, &wr, &bad_wr) != ncclSuccess)  {
        goto err;
    }
#ifdef ANP_DEBUG_TRACE_EN
    INFO(NCCL_NET, "Posted RECV WQE, ch %d, qp %d, nic %d, dev index %d",
         qp->channelId, qp->qp->qp_num, comm->devs[qp->devIndex].base.ibDevN, qp->devIndex);
#endif
    ANP_TELEMETRY_EXECUTE(
        g_debug_stats.num_recv_wqe++;
        g_anp_state.increment_num_recv_wqe(qp->qp->qp_num);
    );
    // Don't update comm->base.qpIndex yet, we need to run through this same set of QPs
    // inside ncclIbPostFifo()
    //comm->base.qpIndex = (comm->base.qpIndex+1)%comm->base.nqps;
    qpIndex = (qpIndex+1)%comm->base.nqps;
  }

  TIME_STOP(1);

  // Post to FIFO to notify sender
  TIME_START(2);
  NCCLCHECKGOTO(ncclIbPostFifo(comm, n, data, sizes, tags, mhandles, req), res, err);
  TIME_STOP(2);

  *request = req;
  return ncclSuccess;
err:
  if (req) {
      ncclIbFreeRequest(req);
  }
  return res;
}

static ncclResult_t anpNetIrecvPostCTS(void* recvComm, int n, void** data, size_t* sizes, int* tags, void** mhandles, void** request) {
  struct ncclIbRecvComm* comm = (struct ncclIbRecvComm*)recvComm;
  if (comm->base.ready == 0) { WARN("NET/IB: ncclIbIrecv() called when comm->base.ready == 0"); return ncclInternalError; }
  if (comm->base.ready == 0) { *request = NULL; return ncclSuccess; }
  if (n > NCCL_NET_IB_MAX_RECVS) return ncclInternalError;

  struct ncclIbRequest* req;
  NCCLCHECK(ncclIbGetRequest(&comm->base, &req));
  req->type = NCCL_NET_IB_REQ_RECV;
  req->sock = &comm->base.sock;
  req->nreqs = n;

  for (int i = 0; i < comm->base.ndevs; i++) {
    req->devBases[i] = &comm->devs[i].base;
  }

  // Post to FIFO to notify sender
  TIME_START(2);
  NCCLCHECK(ncclIbPostFifo(comm, n, data, sizes, tags, mhandles, req));
  TIME_STOP(2);

  *request = req;
  return ncclSuccess;
}

ncclResult_t anpNetIrecv(void* recvComm, int n, void** data, size_t* sizes, int* tags, void** mhandles, void** request) {
#ifdef ANP_DEBUG_TRACE_EN
    INFO(NCCL_NET, "Processing recv, recvComm %p, n %d", recvComm, n);
#endif
    if (*request == (void *)NCCL_NET_OPTIONAL_RECV_COMPLETION) {
        // for LL & LL128, post only CTS (no need to post RECV WQE in this case)
        INFO(NCCL_NET, "Optional RECV completion set, posting CTS");
        return anpNetIrecvPostCTS(recvComm, n, data, sizes, tags, mhandles, request);
    }
    INFO(NCCL_NET, "Optional RECV completion NOT set, posting RECV WQE & CTS");
    return anpNetIrecvDefault(recvComm, n, data, sizes, tags, mhandles, request);
}

ncclResult_t anpNetFlush(void* recvComm, int n, void** data, int* sizes, void** mhandles, void** request) {
  struct ncclIbRecvComm* comm = (struct ncclIbRecvComm*)recvComm;
  int last = -1;
  for (int i=0; i<n; i++) if (sizes[i]) last = i;
  if (comm->flushEnabled == 0 || last == -1) return ncclSuccess;

  // Only flush once using the last non-zero receive
  struct ncclIbRequest* req;
  NCCLCHECK(ncclIbGetRequest(&comm->base, &req));
  req->type = NCCL_NET_IB_REQ_FLUSH;
  req->sock = &comm->base.sock;
  struct ncclIbMrHandle* mhandle = (struct ncclIbMrHandle*) mhandles[last];

  // We don't know which devIndex the recv was on, so we flush on all devices
  for (int i = 0; i < comm->base.ndevs; i++) {
    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = req - comm->base.reqs;
    if (ncclParamIbGdrFlushGpuMemNoRelaxedOrdering()) {
      wr.wr.rdma.remote_addr = (uint64_t)(comm->devs[i].gpuFlush.gpuFlushGpuMem);
      wr.wr.rdma.rkey = comm->devs[i].gpuFlush.gpuMr->rkey;
      wr.sg_list = &comm->devs[i].gpuFlush.sge;
      wr.num_sge = 1;
      wr.opcode = IBV_WR_RDMA_WRITE;
      wr.send_flags = 0;
      struct ibv_send_wr* bad_wr;
      NCCLCHECK(wrap_ibv_post_send(comm->devs[i].gpuFlush.qp.qp, &wr, &bad_wr));
    }
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = req - comm->base.reqs;
    if (ncclParamIbGdrFlushGpuMemNoRelaxedOrdering()) {
      wr.wr.rdma.remote_addr = (uint64_t)(comm->devs[i].gpuFlush.gpuFlushGpuMem);
      wr.wr.rdma.rkey = comm->devs[i].gpuFlush.gpuMr->rkey;
    } else {
      wr.wr.rdma.remote_addr = (uint64_t)data[last];
      wr.wr.rdma.rkey = mhandle->mrs[i]->rkey;
    }
    wr.sg_list = &comm->devs[i].gpuFlush.sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.send_flags = IBV_SEND_SIGNALED;

    TIME_START(4);
    struct ibv_send_wr* bad_wr;
    NCCLCHECK(wrap_ibv_post_send(comm->devs[i].gpuFlush.qp.qp, &wr, &bad_wr));
    TIME_STOP(4);

    ncclIbAddEvent(req, i, &comm->devs[i].base);
  }

  *request = req;
  return ncclSuccess;
}

#define ANP_CQ_POLL_MAX_EVENT        16
ncclResult_t anpNetTest(void* request, int* done, int* sizes) {
  struct ncclIbRequest *r = (struct ncclIbRequest*)request;
  *done = 0;
  while (1) {
    if (r->events[0] == 0 && r->events[1] == 0) {
      TRACE(NCCL_NET, "r=%p done", r);
      *done = 1;
      if (sizes && r->type == NCCL_NET_IB_REQ_RECV) {
        for (int i=0; i<r->nreqs; i++) sizes[i] = r->recv.sizes[i];
      }
      if (sizes && r->type == NCCL_NET_IB_REQ_SEND) {
        sizes[0] = r->send.size;
      }
      NCCLCHECK(ncclIbFreeRequest(r));
      return ncclSuccess;
    }

    int totalWrDone = 0;
    int wrDone = 0;
    struct ibv_wc wcs[ANP_CQ_POLL_MAX_EVENT];

    for (int i = 0; i < NCCL_IB_MAX_DEVS_PER_NIC; i++) {
      TIME_START(3);
      // If we expect any completions from this device's CQ
      if (r->events[i]) {
        NCCLCHECK(wrap_ibv_poll_cq(r->devBases[i]->cq, ANP_CQ_POLL_MAX_EVENT,
                                   wcs, &wrDone));
        totalWrDone += wrDone;
        ANP_TELEMETRY_EXECUTE(
            g_anp_state.update_cq_poll_metrics();
        );
        if (wrDone == 0) { TIME_CANCEL(3); } else { TIME_STOP(3); }
        if (wrDone == 0) continue;
        for (int w=0; w<wrDone; w++) {
          struct ibv_wc *wc = wcs+w;
          if (wc->status != IBV_WC_SUCCESS) {
            union ncclSocketAddress addr;
            ncclSocketGetAddr(r->sock, &addr);
            char localGidString[INET6_ADDRSTRLEN] = "";
            char remoteGidString[INET6_ADDRSTRLEN] = "";
            const char* localGidStr = NULL, *remoteGidStr = NULL;
            if (r->devBases[i]->gidInfo.link_layer == IBV_LINK_LAYER_ETHERNET) {
              localGidStr = inet_ntop(AF_INET6, &r->devBases[i]->gidInfo.localGid, localGidString, sizeof(localGidString));
              remoteGidStr = inet_ntop(AF_INET6, &r->base->remDevs[i].remoteGid, remoteGidString, sizeof(remoteGidString));
            }

            char line[SOCKET_NAME_MAXLEN+1];
            char *hcaName = r->devBases[i]->pd->context->device->name;
            WARN("NET/IB: Got completion from peer %s with status=%d opcode=%d len=%d vendor err %d (%s)%s%s%s%s hca %s",
                ncclSocketToString(&addr, line), wc->status, wc->opcode, wc->byte_len, wc->vendor_err, reqTypeStr[r->type],
                localGidStr ?  " localGid ":"", localGidString, remoteGidStr ? " remoteGids":"", remoteGidString, hcaName);
            return ncclRemoteError;
          }

          union ncclSocketAddress addr;
          ncclSocketGetAddr(r->sock, &addr);
          struct ncclIbRequest* req = r->base->reqs+(wc->wr_id & 0xff);

          #ifdef ENABLE_TRACE
          char line[SOCKET_NAME_MAXLEN+1];
          TRACE(NCCL_NET, "Got completion from peer %s with status=%d opcode=%d len=%d wr_id=%ld r=%p type=%d events={%d,%d}, i=%d",
              ncclSocketToString(&addr, line), wc->status, wc->opcode,wc->byte_len, wc->wr_id, req, req->type, req->events[0], req->events[1], i);
          #endif
          if (req->type == NCCL_NET_IB_REQ_SEND) {
            ANP_TELEMETRY_EXECUTE(
                g_debug_stats.num_send_completion++;
                g_anp_state.update_wqe_rcvd_metrics(wc->qp_num, wc->wr_id, gettime_ns());
            );
            for (int j = 0; j < req->nreqs; j++) {
              struct ncclIbRequest* sendReq = r->base->reqs+((wc->wr_id >> (j*8)) & 0xff);
              if ((sendReq->events[i] <= 0)) {
                WARN("NET/IB: sendReq(%p)->events={%d,%d}, i=%d, j=%d <= 0", sendReq, sendReq->events[0], sendReq->events[1], i, j);
                return ncclInternalError;
              }
              sendReq->events[i]--;
              ANP_TELEMETRY_EXECUTE(
                  g_debug_stats.num_send_completion_ok++;
              );
            }
          } else {
            ANP_TELEMETRY_EXECUTE(
                g_anp_state.update_wqe_rcvd_metrics(wc->qp_num, wc->wr_id, gettime_ns());
                g_debug_stats.num_recv_completion++;
            );
            if (req && wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
              if (req->type != NCCL_NET_IB_REQ_RECV) {
                WARN("NET/IB: wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM and req->type=%d", req->type);
                return ncclInternalError;
              }
              if (req->nreqs == 1) {
                req->recv.sizes[0] = wc->imm_data;
              }
            }
            ANP_TELEMETRY_EXECUTE(
                g_debug_stats.num_recv_completion_ok++;
            );
            req->events[i]--;
          }
        }
      }
    }

    // If no CQEs found on any device, return and come back later
    if (totalWrDone == 0) return ncclSuccess;
  }
}

ncclResult_t anpNetCloseSend(void* sendComm) {
  struct ncclIbSendComm* comm = (struct ncclIbSendComm*)sendComm;
  if (comm) {
    NCCLCHECK(ncclSocketClose(&comm->base.sock));

    for (int q = 0; q < comm->base.nqps; q++)
      if (comm->base.qps[q].qp != NULL) NCCLCHECK(wrap_ibv_destroy_qp(comm->base.qps[q].qp));

    for (int i = 0; i < comm->base.ndevs; i++) {
      struct ncclIbSendCommDev* commDev = comm->devs + i;
      if (commDev->fifoMr != NULL) NCCLCHECK(wrap_ibv_dereg_mr(commDev->fifoMr));
      if (comm->remSizesFifo.mrs[i] != NULL) NCCLCHECK(wrap_ibv_dereg_mr(comm->remSizesFifo.mrs[i]));
      NCCLCHECK(ncclIbDestroyBase(&commDev->base));
    }
    free(comm);
  }
  TIME_PRINT("IB");
#if 0
  static bool anp_stats_dumped = false;
  if (anp_stats_dumped == false) {
    fprintf(stderr, "Dumping ANP debug stats at the end of the run\n");
    anp_debug_stats_dump();
    anp_stats_dumped = true;
  }
#endif
  return ncclSuccess;
}

ncclResult_t anpNetCloseRecv(void* recvComm) {
  struct ncclIbRecvComm* comm = (struct ncclIbRecvComm*)recvComm;
  if (comm) {
    NCCLCHECK(ncclSocketClose(&comm->base.sock));

    for (int q = 0; q < comm->base.nqps; q++)
      if (comm->base.qps[q].qp != NULL) NCCLCHECK(wrap_ibv_destroy_qp(comm->base.qps[q].qp));

    for (int i = 0; i < comm->base.ndevs; i++) {
      struct ncclIbRecvCommDev* commDev = comm->devs + i;
      if (comm->flushEnabled) {
#if 0
        if (commDev->gpuFlush.gpuFlushGpuMem != nullptr) {
          NCCLCHECK(ncclCudaFree(commDev->gpuFlush.gpuFlushGpuMem));
          commDev->gpuFlush.gpuFlushGpuMem = nullptr;
          if (commDev->gpuFlush.gpuMr != nullptr) NCCLCHECK(wrap_ibv_dereg_mr(commDev->gpuFlush.gpuMr));
          commDev->gpuFlush.gpuMr = nullptr;
        }
#endif
        if (commDev->gpuFlush.qp.qp != NULL) NCCLCHECK(wrap_ibv_destroy_qp(commDev->gpuFlush.qp.qp));
        if (commDev->gpuFlush.hostMr != NULL) NCCLCHECK(wrap_ibv_dereg_mr(commDev->gpuFlush.hostMr));
      }
      if (commDev->fifoMr != NULL) NCCLCHECK(wrap_ibv_dereg_mr(commDev->fifoMr));
      if (commDev->sizesFifoMr != NULL) NCCLCHECK(wrap_ibv_dereg_mr(commDev->sizesFifoMr));
      NCCLCHECK(ncclIbDestroyBase(&commDev->base));
    }
    free(comm);
  }
  return ncclSuccess;
}

ncclResult_t anpNetCloseListen(void* listenComm) {
  struct ncclIbListenComm* comm = (struct ncclIbListenComm*)listenComm;
  if (comm) {
    NCCLCHECK(ncclSocketClose(&comm->sock));
    free(comm);
  }
  return ncclSuccess;
}

// Define the plugin's ncclNet_v1 symbol
ncclNet_t NCCL_NET_PLUGIN_SYMBOL = {
    .name = "RCCL-ANP",
    .init = anpNetInit,
    .devices = anpNetDevices,
    .getProperties = anpNetGetProperties,
    .listen = anpNetListen,
    .connect = anpNetConnect,
    .accept = anpNetAccept,
    .regMr = anpNetRegMr,
    .regMrDmaBuf = ncclIbRegMrDmaBuf,
    .deregMr = anpNetDeregMr,
    .isend = anpNetIsend,
    .irecv = anpNetIrecv,
    .iflush = anpNetFlush,
    .test = anpNetTest,
    .closeSend = anpNetCloseSend,
    .closeRecv = anpNetCloseRecv,
    .closeListen = anpNetCloseListen,
};

#undef NCCL_BUILD_RDMA_CORE
