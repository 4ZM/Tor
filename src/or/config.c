/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2011, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file config.c
 * \brief Code to parse and interpret configuration files.
 **/

#define CONFIG_PRIVATE

#include "or.h"
#include "circuitbuild.h"
#include "circuitlist.h"
#include "config.h"
#include "connection.h"
#include "connection_edge.h"
#include "connection_or.h"
#include "control.h"
#include "cpuworker.h"
#include "dirserv.h"
#include "dirvote.h"
#include "dns.h"
#include "geoip.h"
#include "hibernate.h"
#include "main.h"
#include "networkstatus.h"
#include "policies.h"
#include "relay.h"
#include "rendclient.h"
#include "rendservice.h"
#include "rephist.h"
#include "router.h"
#include "util.h"
#include "routerlist.h"
#include "transports.h"
#ifdef MS_WINDOWS
#include <shlobj.h>
#endif

#include "procmon.h"

/* From main.c */
extern int quiet_level;

/** Enumeration of types which option values can take */
typedef enum config_type_t {
  CONFIG_TYPE_STRING = 0,   /**< An arbitrary string. */
  CONFIG_TYPE_FILENAME,     /**< A filename: some prefixes get expanded. */
  CONFIG_TYPE_UINT,         /**< A non-negative integer less than MAX_INT */
  CONFIG_TYPE_PORT,         /**< A port from 1...65535, 0 for "not set", or
                             * "auto".  */
  CONFIG_TYPE_INTERVAL,     /**< A number of seconds, with optional units*/
  CONFIG_TYPE_MSEC_INTERVAL,/**< A number of milliseconds, with optional
                              * units */
  CONFIG_TYPE_MEMUNIT,      /**< A number of bytes, with optional units*/
  CONFIG_TYPE_DOUBLE,       /**< A floating-point value */
  CONFIG_TYPE_BOOL,         /**< A boolean value, expressed as 0 or 1. */
  CONFIG_TYPE_AUTOBOOL,     /**< A boolean+auto value, expressed 0 for false,
                             * 1 for true, and -1 for auto  */
  CONFIG_TYPE_ISOTIME,      /**< An ISO-formatted time relative to GMT. */
  CONFIG_TYPE_CSV,          /**< A list of strings, separated by commas and
                              * optional whitespace. */
  CONFIG_TYPE_LINELIST,     /**< Uninterpreted config lines */
  CONFIG_TYPE_LINELIST_S,   /**< Uninterpreted, context-sensitive config lines,
                             * mixed with other keywords. */
  CONFIG_TYPE_LINELIST_V,   /**< Catch-all "virtual" option to summarize
                             * context-sensitive config lines when fetching.
                             */
  CONFIG_TYPE_ROUTERSET,    /**< A list of router names, addrs, and fps,
                             * parsed into a routerset_t. */
  CONFIG_TYPE_OBSOLETE,     /**< Obsolete (ignored) option. */
} config_type_t;

/** An abbreviation for a configuration option allowed on the command line. */
typedef struct config_abbrev_t {
  const char *abbreviated;
  const char *full;
  int commandline_only;
  int warn;
} config_abbrev_t;

/* Handy macro for declaring "In the config file or on the command line,
 * you can abbreviate <b>tok</b>s as <b>tok</b>". */
#define PLURAL(tok) { #tok, #tok "s", 0, 0 }

/** A list of abbreviations and aliases to map command-line options, obsolete
 * option names, or alternative option names, to their current values. */
static config_abbrev_t _option_abbrevs[] = {
  PLURAL(ExitNode),
  PLURAL(EntryNode),
  PLURAL(ExcludeNode),
  PLURAL(FirewallPort),
  PLURAL(LongLivedPort),
  PLURAL(HiddenServiceNode),
  PLURAL(HiddenServiceExcludeNode),
  PLURAL(NumCPU),
  PLURAL(RendNode),
  PLURAL(RendExcludeNode),
  PLURAL(StrictEntryNode),
  PLURAL(StrictExitNode),
  PLURAL(StrictNode),
  { "l", "Log", 1, 0},
  { "AllowUnverifiedNodes", "AllowInvalidNodes", 0, 0},
  { "AutomapHostSuffixes", "AutomapHostsSuffixes", 0, 0},
  { "AutomapHostOnResolve", "AutomapHostsOnResolve", 0, 0},
  { "BandwidthRateBytes", "BandwidthRate", 0, 0},
  { "BandwidthBurstBytes", "BandwidthBurst", 0, 0},
  { "DirFetchPostPeriod", "StatusFetchPeriod", 0, 0},
  { "MaxConn", "ConnLimit", 0, 1},
  { "ORBindAddress", "ORListenAddress", 0, 0},
  { "DirBindAddress", "DirListenAddress", 0, 0},
  { "SocksBindAddress", "SocksListenAddress", 0, 0},
  { "UseHelperNodes", "UseEntryGuards", 0, 0},
  { "NumHelperNodes", "NumEntryGuards", 0, 0},
  { "UseEntryNodes", "UseEntryGuards", 0, 0},
  { "NumEntryNodes", "NumEntryGuards", 0, 0},
  { "ResolvConf", "ServerDNSResolvConfFile", 0, 1},
  { "SearchDomains", "ServerDNSSearchDomains", 0, 1},
  { "ServerDNSAllowBrokenResolvConf", "ServerDNSAllowBrokenConfig", 0, 0},
  { "PreferTunnelledDirConns", "PreferTunneledDirConns", 0, 0},
  { "BridgeAuthoritativeDirectory", "BridgeAuthoritativeDir", 0, 0},
  { "HashedControlPassword", "__HashedControlSessionPassword", 1, 0},
  { "StrictEntryNodes", "StrictNodes", 0, 1},
  { "StrictExitNodes", "StrictNodes", 0, 1},
  { NULL, NULL, 0, 0},
};

/** A list of state-file "abbreviations," for compatibility. */
static config_abbrev_t _state_abbrevs[] = {
  { "AccountingBytesReadInterval", "AccountingBytesReadInInterval", 0, 0 },
  { "HelperNode", "EntryGuard", 0, 0 },
  { "HelperNodeDownSince", "EntryGuardDownSince", 0, 0 },
  { "HelperNodeUnlistedSince", "EntryGuardUnlistedSince", 0, 0 },
  { "EntryNode", "EntryGuard", 0, 0 },
  { "EntryNodeDownSince", "EntryGuardDownSince", 0, 0 },
  { "EntryNodeUnlistedSince", "EntryGuardUnlistedSince", 0, 0 },
  { NULL, NULL, 0, 0},
};
#undef PLURAL

/** A variable allowed in the configuration file or on the command line. */
typedef struct config_var_t {
  const char *name; /**< The full keyword (case insensitive). */
  config_type_t type; /**< How to interpret the type and turn it into a
                       * value. */
  off_t var_offset; /**< Offset of the corresponding member of or_options_t. */
  const char *initvalue; /**< String (or null) describing initial value. */
} config_var_t;

/** An entry for config_vars: "The option <b>name</b> has type
 * CONFIG_TYPE_<b>conftype</b>, and corresponds to
 * or_options_t.<b>member</b>"
 */
#define VAR(name,conftype,member,initvalue)                             \
  { name, CONFIG_TYPE_ ## conftype, STRUCT_OFFSET(or_options_t, member), \
      initvalue }
/** As VAR, but the option name and member name are the same. */
#define V(member,conftype,initvalue)                                    \
  VAR(#member, conftype, member, initvalue)
/** An entry for config_vars: "The option <b>name</b> is obsolete." */
#define OBSOLETE(name) { name, CONFIG_TYPE_OBSOLETE, 0, NULL }

/** Array of configuration options.  Until we disallow nonstandard
 * abbreviations, order is significant, since the first matching option will
 * be chosen first.
 */
static config_var_t _option_vars[] = {
  OBSOLETE("AccountingMaxKB"),
  V(AccountingMax,               MEMUNIT,  "0 bytes"),
  V(AccountingStart,             STRING,   NULL),
  V(Address,                     STRING,   NULL),
  V(AllowDotExit,                BOOL,     "0"),
  V(AllowInvalidNodes,           CSV,      "middle,rendezvous"),
  V(AllowNonRFC953Hostnames,     BOOL,     "0"),
  V(AllowSingleHopCircuits,      BOOL,     "0"),
  V(AllowSingleHopExits,         BOOL,     "0"),
  V(AlternateBridgeAuthority,    LINELIST, NULL),
  V(AlternateDirAuthority,       LINELIST, NULL),
  V(AlternateHSAuthority,        LINELIST, NULL),
  V(AssumeReachable,             BOOL,     "0"),
  V(AuthDirBadDir,               LINELIST, NULL),
  V(AuthDirBadExit,              LINELIST, NULL),
  V(AuthDirInvalid,              LINELIST, NULL),
  V(AuthDirFastGuarantee,        MEMUNIT,  "100 KB"),
  V(AuthDirGuardBWGuarantee,     MEMUNIT,  "250 KB"),
  V(AuthDirReject,               LINELIST, NULL),
  V(AuthDirRejectUnlisted,       BOOL,     "0"),
  V(AuthDirListBadDirs,          BOOL,     "0"),
  V(AuthDirListBadExits,         BOOL,     "0"),
  V(AuthDirMaxServersPerAddr,    UINT,     "2"),
  V(AuthDirMaxServersPerAuthAddr,UINT,     "5"),
  VAR("AuthoritativeDirectory",  BOOL, AuthoritativeDir,    "0"),
  V(AutomapHostsOnResolve,       BOOL,     "0"),
  V(AutomapHostsSuffixes,        CSV,      ".onion,.exit"),
  V(AvoidDiskWrites,             BOOL,     "0"),
  V(BandwidthBurst,              MEMUNIT,  "10 MB"),
  V(BandwidthRate,               MEMUNIT,  "5 MB"),
  V(BridgeAuthoritativeDir,      BOOL,     "0"),
  VAR("Bridge",                  LINELIST, Bridges,    NULL),
  V(BridgePassword,              STRING,   NULL),
  V(BridgeRecordUsageByCountry,  BOOL,     "1"),
  V(BridgeRelay,                 BOOL,     "0"),
  V(CellStatistics,              BOOL,     "0"),
  V(LearnCircuitBuildTimeout,    BOOL,     "1"),
  V(CircuitBuildTimeout,         INTERVAL, "0"),
  V(CircuitIdleTimeout,          INTERVAL, "1 hour"),
  V(CircuitStreamTimeout,        INTERVAL, "0"),
  V(CircuitPriorityHalflife,     DOUBLE,  "-100.0"), /*negative:'Use default'*/
  V(ClientDNSRejectInternalAddresses, BOOL,"1"),
  V(ClientOnly,                  BOOL,     "0"),
  V(ClientRejectInternalAddresses, BOOL,   "1"),
  V(ClientTransportPlugin,       LINELIST, NULL),
  V(ConsensusParams,             STRING,   NULL),
  V(ConnLimit,                   UINT,     "1000"),
  V(ConnDirectionStatistics,     BOOL,     "0"),
  V(ConstrainedSockets,          BOOL,     "0"),
  V(ConstrainedSockSize,         MEMUNIT,  "8192"),
  V(ContactInfo,                 STRING,   NULL),
  V(ControlListenAddress,        LINELIST, NULL),
  V(ControlPort,                 LINELIST, NULL),
  V(ControlPortFileGroupReadable,BOOL,     "0"),
  V(ControlPortWriteToFile,      FILENAME, NULL),
  V(ControlSocket,               LINELIST, NULL),
  V(ControlSocketsGroupWritable, BOOL,     "0"),
  V(CookieAuthentication,        BOOL,     "0"),
  V(CookieAuthFileGroupReadable, BOOL,     "0"),
  V(CookieAuthFile,              STRING,   NULL),
  V(CountPrivateBandwidth,       BOOL,     "0"),
  V(DataDirectory,               FILENAME, NULL),
  OBSOLETE("DebugLogFile"),
  V(DisableNetwork,              BOOL,     "0"),
  V(DirAllowPrivateAddresses,    BOOL,     "0"),
  V(TestingAuthDirTimeToLearnReachability, INTERVAL, "30 minutes"),
  V(DirListenAddress,            LINELIST, NULL),
  OBSOLETE("DirFetchPeriod"),
  V(DirPolicy,                   LINELIST, NULL),
  V(DirPort,                     LINELIST, NULL),
  V(DirPortFrontPage,            FILENAME, NULL),
  OBSOLETE("DirPostPeriod"),
  OBSOLETE("DirRecordUsageByCountry"),
  OBSOLETE("DirRecordUsageGranularity"),
  OBSOLETE("DirRecordUsageRetainIPs"),
  OBSOLETE("DirRecordUsageSaveInterval"),
  V(DirReqStatistics,            BOOL,     "1"),
  VAR("DirServer",               LINELIST, DirServers, NULL),
  V(DisableAllSwap,              BOOL,     "0"),
  V(DisableDebuggerAttachment,   BOOL,     "1"),
  V(DisableIOCP,                 BOOL,     "1"),
  V(DynamicDHGroups,             BOOL,     "1"),
  V(DNSPort,                     LINELIST, NULL),
  V(DNSListenAddress,            LINELIST, NULL),
  V(DownloadExtraInfo,           BOOL,     "0"),
  V(EnforceDistinctSubnets,      BOOL,     "1"),
  V(EntryNodes,                  ROUTERSET,   NULL),
  V(EntryStatistics,             BOOL,     "0"),
  V(TestingEstimatedDescriptorPropagationTime, INTERVAL, "10 minutes"),
  V(ExcludeNodes,                ROUTERSET, NULL),
  V(ExcludeExitNodes,            ROUTERSET, NULL),
  V(ExcludeSingleHopRelays,      BOOL,     "1"),
  V(ExitNodes,                   ROUTERSET, NULL),
  V(ExitPolicy,                  LINELIST, NULL),
  V(ExitPolicyRejectPrivate,     BOOL,     "1"),
  V(ExitPortStatistics,          BOOL,     "0"),
  V(ExtraInfoStatistics,         BOOL,     "1"),

#if defined (WINCE)
  V(FallbackNetworkstatusFile,   FILENAME, "fallback-consensus"),
#else
  V(FallbackNetworkstatusFile,   FILENAME,
    SHARE_DATADIR PATH_SEPARATOR "tor" PATH_SEPARATOR "fallback-consensus"),
#endif
  V(FascistFirewall,             BOOL,     "0"),
  V(FirewallPorts,               CSV,      ""),
  V(FastFirstHopPK,              BOOL,     "1"),
  V(FetchDirInfoEarly,           BOOL,     "0"),
  V(FetchDirInfoExtraEarly,      BOOL,     "0"),
  V(FetchServerDescriptors,      BOOL,     "1"),
  V(FetchHidServDescriptors,     BOOL,     "1"),
  V(FetchUselessDescriptors,     BOOL,     "0"),
  V(FetchV2Networkstatus,        BOOL,     "0"),
#ifdef WIN32
  V(GeoIPFile,                   FILENAME, "<default>"),
#else
  V(GeoIPFile,                   FILENAME,
    SHARE_DATADIR PATH_SEPARATOR "tor" PATH_SEPARATOR "geoip"),
#endif
  V(GiveGuardFlagTo_CVE_2011_2768_VulnerableRelays,
                                 BOOL,     "0"),
  OBSOLETE("Group"),
  V(HardwareAccel,               BOOL,     "0"),
  V(HeartbeatPeriod,             INTERVAL, "6 hours"),
  V(AccelName,                   STRING,   NULL),
  V(AccelDir,                    FILENAME, NULL),
  V(HashedControlPassword,       LINELIST, NULL),
  V(HidServDirectoryV2,          BOOL,     "1"),
  VAR("HiddenServiceDir",    LINELIST_S, RendConfigLines,    NULL),
  OBSOLETE("HiddenServiceExcludeNodes"),
  OBSOLETE("HiddenServiceNodes"),
  VAR("HiddenServiceOptions",LINELIST_V, RendConfigLines,    NULL),
  VAR("HiddenServicePort",   LINELIST_S, RendConfigLines,    NULL),
  VAR("HiddenServiceVersion",LINELIST_S, RendConfigLines,    NULL),
  VAR("HiddenServiceAuthorizeClient",LINELIST_S,RendConfigLines, NULL),
  V(HidServAuth,                 LINELIST, NULL),
  V(HSAuthoritativeDir,          BOOL,     "0"),
  OBSOLETE("HSAuthorityRecordStats"),
  V(HTTPProxy,                   STRING,   NULL),
  V(HTTPProxyAuthenticator,      STRING,   NULL),
  V(HTTPSProxy,                  STRING,   NULL),
  V(HTTPSProxyAuthenticator,     STRING,   NULL),
  VAR("ServerTransportPlugin",   LINELIST, ServerTransportPlugin,  NULL),
  V(Socks4Proxy,                 STRING,   NULL),
  V(Socks5Proxy,                 STRING,   NULL),
  V(Socks5ProxyUsername,         STRING,   NULL),
  V(Socks5ProxyPassword,         STRING,   NULL),
  OBSOLETE("IgnoreVersion"),
  V(KeepalivePeriod,             INTERVAL, "5 minutes"),
  VAR("Log",                     LINELIST, Logs,             NULL),
  V(LogMessageDomains,           BOOL,     "0"),
  OBSOLETE("LinkPadding"),
  OBSOLETE("LogLevel"),
  OBSOLETE("LogFile"),
  V(LogTimeGranularity,          MSEC_INTERVAL, "1 second"),
  V(LongLivedPorts,              CSV,
        "21,22,706,1863,5050,5190,5222,5223,6523,6667,6697,8300"),
  VAR("MapAddress",              LINELIST, AddressMap,           NULL),
  V(MaxAdvertisedBandwidth,      MEMUNIT,  "1 GB"),
  V(MaxCircuitDirtiness,         INTERVAL, "10 minutes"),
  V(MaxClientCircuitsPending,    UINT,     "32"),
  V(MaxOnionsPending,            UINT,     "100"),
  OBSOLETE("MonthlyAccountingStart"),
  V(MyFamily,                    STRING,   NULL),
  V(NewCircuitPeriod,            INTERVAL, "30 seconds"),
  VAR("NamingAuthoritativeDirectory",BOOL, NamingAuthoritativeDir, "0"),
  V(NATDListenAddress,           LINELIST, NULL),
  V(NATDPort,                    LINELIST, NULL),
  V(Nickname,                    STRING,   NULL),
  V(WarnUnsafeSocks,              BOOL,     "1"),
  OBSOLETE("NoPublish"),
  VAR("NodeFamily",              LINELIST, NodeFamilies,         NULL),
  V(NumCPUs,                     UINT,     "0"),
  V(NumEntryGuards,              UINT,     "3"),
  V(ORListenAddress,             LINELIST, NULL),
  V(ORPort,                      LINELIST, NULL),
  V(OutboundBindAddress,         STRING,   NULL),
  OBSOLETE("PathlenCoinWeight"),
  V(PerConnBWBurst,              MEMUNIT,  "0"),
  V(PerConnBWRate,               MEMUNIT,  "0"),
  V(PidFile,                     STRING,   NULL),
  V(TestingTorNetwork,           BOOL,     "0"),
  V(OptimisticData,              AUTOBOOL, "auto"),
  V(PortForwarding,              BOOL,     "0"),
  V(PortForwardingHelper,        FILENAME, "tor-fw-helper"),
  V(PreferTunneledDirConns,      BOOL,     "1"),
  V(ProtocolWarnings,            BOOL,     "0"),
  V(PublishServerDescriptor,     CSV,      "1"),
  V(PublishHidServDescriptors,   BOOL,     "1"),
  V(ReachableAddresses,          LINELIST, NULL),
  V(ReachableDirAddresses,       LINELIST, NULL),
  V(ReachableORAddresses,        LINELIST, NULL),
  V(RecommendedVersions,         LINELIST, NULL),
  V(RecommendedClientVersions,   LINELIST, NULL),
  V(RecommendedServerVersions,   LINELIST, NULL),
  OBSOLETE("RedirectExit"),
  V(RefuseUnknownExits,          AUTOBOOL, "auto"),
  V(RejectPlaintextPorts,        CSV,      ""),
  V(RelayBandwidthBurst,         MEMUNIT,  "0"),
  V(RelayBandwidthRate,          MEMUNIT,  "0"),
  OBSOLETE("RendExcludeNodes"),
  OBSOLETE("RendNodes"),
  V(RendPostPeriod,              INTERVAL, "1 hour"),
  V(RephistTrackTime,            INTERVAL, "24 hours"),
  OBSOLETE("RouterFile"),
  V(RunAsDaemon,                 BOOL,     "0"),
//  V(RunTesting,                  BOOL,     "0"),
  OBSOLETE("RunTesting"), // currently unused
  V(SafeLogging,                 STRING,   "1"),
  V(SafeSocks,                   BOOL,     "0"),
  V(ServerDNSAllowBrokenConfig,  BOOL,     "1"),
  V(ServerDNSAllowNonRFC953Hostnames, BOOL,"0"),
  V(ServerDNSDetectHijacking,    BOOL,     "1"),
  V(ServerDNSRandomizeCase,      BOOL,     "1"),
  V(ServerDNSResolvConfFile,     STRING,   NULL),
  V(ServerDNSSearchDomains,      BOOL,     "0"),
  V(ServerDNSTestAddresses,      CSV,
      "www.google.com,www.mit.edu,www.yahoo.com,www.slashdot.org"),
  V(ShutdownWaitLength,          INTERVAL, "30 seconds"),
  V(SocksListenAddress,          LINELIST, NULL),
  V(SocksPolicy,                 LINELIST, NULL),
  V(SocksPort,                   LINELIST, NULL),
  V(SocksTimeout,                INTERVAL, "2 minutes"),
  OBSOLETE("StatusFetchPeriod"),
  V(StrictNodes,                 BOOL,     "0"),
  OBSOLETE("SysLog"),
  V(TestSocks,                   BOOL,     "0"),
  OBSOLETE("TestVia"),
  V(TokenBucketRefillInterval,   MSEC_INTERVAL, "100 msec"),
  V(Tor2webMode,                 BOOL,     "0"),
  V(TrackHostExits,              CSV,      NULL),
  V(TrackHostExitsExpire,        INTERVAL, "30 minutes"),
  OBSOLETE("TrafficShaping"),
  V(TransListenAddress,          LINELIST, NULL),
  V(TransPort,                   LINELIST, NULL),
  V(TunnelDirConns,              BOOL,     "1"),
  V(UpdateBridgesFromAuthority,  BOOL,     "0"),
  V(UseBridges,                  BOOL,     "0"),
  V(UseEntryGuards,              BOOL,     "1"),
  V(UseMicrodescriptors,         AUTOBOOL, "auto"),
  V(User,                        STRING,   NULL),
  V(UserspaceIOCPBuffers,        BOOL,     "0"),
  VAR("V1AuthoritativeDirectory",BOOL, V1AuthoritativeDir,   "0"),
  VAR("V2AuthoritativeDirectory",BOOL, V2AuthoritativeDir,   "0"),
  VAR("V3AuthoritativeDirectory",BOOL, V3AuthoritativeDir,   "0"),
  V(TestingV3AuthInitialVotingInterval, INTERVAL, "30 minutes"),
  V(TestingV3AuthInitialVoteDelay, INTERVAL, "5 minutes"),
  V(TestingV3AuthInitialDistDelay, INTERVAL, "5 minutes"),
  V(V3AuthVotingInterval,        INTERVAL, "1 hour"),
  V(V3AuthVoteDelay,             INTERVAL, "5 minutes"),
  V(V3AuthDistDelay,             INTERVAL, "5 minutes"),
  V(V3AuthNIntervalsValid,       UINT,     "3"),
  V(V3AuthUseLegacyKey,          BOOL,     "0"),
  V(V3BandwidthsFile,            FILENAME, NULL),
  VAR("VersioningAuthoritativeDirectory",BOOL,VersioningAuthoritativeDir, "0"),
  V(VirtualAddrNetwork,          STRING,   "127.192.0.0/10"),
  V(WarnPlaintextPorts,          CSV,      "23,109,110,143"),
  V(_UseFilteringSSLBufferevents, BOOL,    "0"),
  VAR("__ReloadTorrcOnSIGHUP",   BOOL,  ReloadTorrcOnSIGHUP,      "1"),
  VAR("__AllDirActionsPrivate",  BOOL,  AllDirActionsPrivate,     "0"),
  VAR("__DisablePredictedCircuits",BOOL,DisablePredictedCircuits, "0"),
  VAR("__LeaveStreamsUnattached",BOOL,  LeaveStreamsUnattached,   "0"),
  VAR("__HashedControlSessionPassword", LINELIST, HashedControlSessionPassword,
      NULL),
  VAR("__OwningControllerProcess",STRING,OwningControllerProcess, NULL),
  V(MinUptimeHidServDirectoryV2, INTERVAL, "25 hours"),
  V(VoteOnHidServDirectoriesV2,  BOOL,     "1"),
  V(_UsingTestNetworkDefaults,   BOOL,     "0"),

  { NULL, CONFIG_TYPE_OBSOLETE, 0, NULL }
};

/** Override default values with these if the user sets the TestingTorNetwork
 * option. */
static const config_var_t testing_tor_network_defaults[] = {
  V(ServerDNSAllowBrokenConfig,  BOOL,  "1"),
  V(DirAllowPrivateAddresses,    BOOL,     "1"),
  V(EnforceDistinctSubnets,      BOOL,     "0"),
  V(AssumeReachable,             BOOL,     "1"),
  V(AuthDirMaxServersPerAddr,    UINT,     "0"),
  V(AuthDirMaxServersPerAuthAddr,UINT,     "0"),
  V(ClientDNSRejectInternalAddresses, BOOL,"0"),
  V(ClientRejectInternalAddresses, BOOL,   "0"),
  V(CountPrivateBandwidth,       BOOL,     "1"),
  V(ExitPolicyRejectPrivate,     BOOL,     "0"),
  V(V3AuthVotingInterval,        INTERVAL, "5 minutes"),
  V(V3AuthVoteDelay,             INTERVAL, "20 seconds"),
  V(V3AuthDistDelay,             INTERVAL, "20 seconds"),
  V(TestingV3AuthInitialVotingInterval, INTERVAL, "5 minutes"),
  V(TestingV3AuthInitialVoteDelay, INTERVAL, "20 seconds"),
  V(TestingV3AuthInitialDistDelay, INTERVAL, "20 seconds"),
  V(TestingAuthDirTimeToLearnReachability, INTERVAL, "0 minutes"),
  V(TestingEstimatedDescriptorPropagationTime, INTERVAL, "0 minutes"),
  V(MinUptimeHidServDirectoryV2, INTERVAL, "0 minutes"),
  V(_UsingTestNetworkDefaults,   BOOL,     "1"),

  { NULL, CONFIG_TYPE_OBSOLETE, 0, NULL }
};
#undef VAR

#define VAR(name,conftype,member,initvalue)                             \
  { name, CONFIG_TYPE_ ## conftype, STRUCT_OFFSET(or_state_t, member),  \
      initvalue }

/** Array of "state" variables saved to the ~/.tor/state file. */
static config_var_t _state_vars[] = {
  V(AccountingBytesReadInInterval,    MEMUNIT,  NULL),
  V(AccountingBytesWrittenInInterval, MEMUNIT,  NULL),
  V(AccountingExpectedUsage,          MEMUNIT,  NULL),
  V(AccountingIntervalStart,          ISOTIME,  NULL),
  V(AccountingSecondsActive,          INTERVAL, NULL),
  V(AccountingSecondsToReachSoftLimit,INTERVAL, NULL),
  V(AccountingSoftLimitHitAt,         ISOTIME,  NULL),
  V(AccountingBytesAtSoftLimit,       MEMUNIT,  NULL),

  VAR("EntryGuard",              LINELIST_S,  EntryGuards,             NULL),
  VAR("EntryGuardDownSince",     LINELIST_S,  EntryGuards,             NULL),
  VAR("EntryGuardUnlistedSince", LINELIST_S,  EntryGuards,             NULL),
  VAR("EntryGuardAddedBy",       LINELIST_S,  EntryGuards,             NULL),
  V(EntryGuards,                 LINELIST_V,  NULL),

  VAR("TransportProxy",               LINELIST_S, TransportProxies, NULL),
  V(TransportProxies,                 LINELIST_V, NULL),

  V(BWHistoryReadEnds,                ISOTIME,  NULL),
  V(BWHistoryReadInterval,            UINT,     "900"),
  V(BWHistoryReadValues,              CSV,      ""),
  V(BWHistoryReadMaxima,              CSV,      ""),
  V(BWHistoryWriteEnds,               ISOTIME,  NULL),
  V(BWHistoryWriteInterval,           UINT,     "900"),
  V(BWHistoryWriteValues,             CSV,      ""),
  V(BWHistoryWriteMaxima,             CSV,      ""),
  V(BWHistoryDirReadEnds,             ISOTIME,  NULL),
  V(BWHistoryDirReadInterval,         UINT,     "900"),
  V(BWHistoryDirReadValues,           CSV,      ""),
  V(BWHistoryDirReadMaxima,           CSV,      ""),
  V(BWHistoryDirWriteEnds,            ISOTIME,  NULL),
  V(BWHistoryDirWriteInterval,        UINT,     "900"),
  V(BWHistoryDirWriteValues,          CSV,      ""),
  V(BWHistoryDirWriteMaxima,          CSV,      ""),

  V(TorVersion,                       STRING,   NULL),

  V(LastRotatedOnionKey,              ISOTIME,  NULL),
  V(LastWritten,                      ISOTIME,  NULL),

  V(TotalBuildTimes,                  UINT,     NULL),
  V(CircuitBuildAbandonedCount,       UINT,     "0"),
  VAR("CircuitBuildTimeBin",          LINELIST_S, BuildtimeHistogram, NULL),
  VAR("BuildtimeHistogram",           LINELIST_V, BuildtimeHistogram, NULL),
  { NULL, CONFIG_TYPE_OBSOLETE, 0, NULL }
};

#undef VAR
#undef V
#undef OBSOLETE

/** Represents an English description of a configuration variable; used when
 * generating configuration file comments. */
typedef struct config_var_description_t {
  const char *name;
  const char *description;
} config_var_description_t;

/** Type of a callback to validate whether a given configuration is
 * well-formed and consistent. See options_trial_assign() for documentation
 * of arguments. */
typedef int (*validate_fn_t)(void*,void*,int,char**);

/** Information on the keys, value types, key-to-struct-member mappings,
 * variable descriptions, validation functions, and abbreviations for a
 * configuration or storage format. */
typedef struct {
  size_t size; /**< Size of the struct that everything gets parsed into. */
  uint32_t magic; /**< Required 'magic value' to make sure we have a struct
                   * of the right type. */
  off_t magic_offset; /**< Offset of the magic value within the struct. */
  config_abbrev_t *abbrevs; /**< List of abbreviations that we expand when
                             * parsing this format. */
  config_var_t *vars; /**< List of variables we recognize, their default
                       * values, and where we stick them in the structure. */
  validate_fn_t validate_fn; /**< Function to validate config. */
  /** If present, extra is a LINELIST variable for unrecognized
   * lines.  Otherwise, unrecognized lines are an error. */
  config_var_t *extra;
} config_format_t;

/** Macro: assert that <b>cfg</b> has the right magic field for format
 * <b>fmt</b>. */
#define CHECK(fmt, cfg) STMT_BEGIN                                      \
    tor_assert(fmt && cfg);                                             \
    tor_assert((fmt)->magic ==                                          \
               *(uint32_t*)STRUCT_VAR_P(cfg,fmt->magic_offset));        \
  STMT_END

#ifdef MS_WINDOWS
static char *get_windows_conf_root(void);
#endif
static void config_line_append(config_line_t **lst,
                               const char *key, const char *val);
static void option_clear(const config_format_t *fmt, or_options_t *options,
                         const config_var_t *var);
static void option_reset(const config_format_t *fmt, or_options_t *options,
                         const config_var_t *var, int use_defaults);
static void config_free(const config_format_t *fmt, void *options);
static int config_lines_eq(config_line_t *a, config_line_t *b);
static int option_is_same(const config_format_t *fmt,
                          const or_options_t *o1, const or_options_t *o2,
                          const char *name);
static or_options_t *options_dup(const config_format_t *fmt,
                                 const or_options_t *old);
static int options_validate(or_options_t *old_options,
                            or_options_t *options,
                            int from_setconf, char **msg);
static int options_act_reversible(const or_options_t *old_options, char **msg);
static int options_act(const or_options_t *old_options);
static int options_transition_allowed(const or_options_t *old,
                                      const or_options_t *new,
                                      char **msg);
static int options_transition_affects_workers(
      const or_options_t *old_options, const or_options_t *new_options);
static int options_transition_affects_descriptor(
      const or_options_t *old_options, const or_options_t *new_options);
static int check_nickname_list(const char *lst, const char *name, char **msg);

static int parse_bridge_line(const char *line, int validate_only);
static int parse_client_transport_line(const char *line, int validate_only);

static int parse_server_transport_line(const char *line, int validate_only);
static int parse_dir_server_line(const char *line,
                                 dirinfo_type_t required_type,
                                 int validate_only);
static void port_cfg_free(port_cfg_t *port);
static int parse_ports(const or_options_t *options, int validate_only,
                              char **msg_out, int *n_ports_out);
static int check_server_ports(const smartlist_t *ports,
                              const or_options_t *options);

static int validate_data_directory(or_options_t *options);
static int write_configuration_file(const char *fname,
                                    const or_options_t *options);
static config_line_t *get_assigned_option(const config_format_t *fmt,
                                        const void *options, const char *key,
                                        int escape_val);
static void config_init(const config_format_t *fmt, void *options);
static int or_state_validate(or_state_t *old_options, or_state_t *options,
                             int from_setconf, char **msg);
static int or_state_load(void);
static int options_init_logs(or_options_t *options, int validate_only);

static uint64_t config_parse_memunit(const char *s, int *ok);
static int config_parse_msec_interval(const char *s, int *ok);
static int config_parse_interval(const char *s, int *ok);
static void init_libevent(const or_options_t *options);
static int opt_streq(const char *s1, const char *s2);

/** Magic value for or_options_t. */
#define OR_OPTIONS_MAGIC 9090909

/** Configuration format for or_options_t. */
static config_format_t options_format = {
  sizeof(or_options_t),
  OR_OPTIONS_MAGIC,
  STRUCT_OFFSET(or_options_t, _magic),
  _option_abbrevs,
  _option_vars,
  (validate_fn_t)options_validate,
  NULL
};

/** Magic value for or_state_t. */
#define OR_STATE_MAGIC 0x57A73f57

/** "Extra" variable in the state that receives lines we can't parse. This
 * lets us preserve options from versions of Tor newer than us. */
static config_var_t state_extra_var = {
  "__extra", CONFIG_TYPE_LINELIST, STRUCT_OFFSET(or_state_t, ExtraLines), NULL
};

/** Configuration format for or_state_t. */
static const config_format_t state_format = {
  sizeof(or_state_t),
  OR_STATE_MAGIC,
  STRUCT_OFFSET(or_state_t, _magic),
  _state_abbrevs,
  _state_vars,
  (validate_fn_t)or_state_validate,
  &state_extra_var,
};

/*
 * Functions to read and write the global options pointer.
 */

/** Command-line and config-file options. */
static or_options_t *global_options = NULL;
/** DOCDOC */
static or_options_t *global_default_options = NULL;
/** Name of most recently read torrc file. */
static char *torrc_fname = NULL;
/** DOCDOC */
static char *torrc_defaults_fname;
/** Persistent serialized state. */
static or_state_t *global_state = NULL;
/** Configuration Options set by command line. */
static config_line_t *global_cmdline_options = NULL;
/** Contents of most recently read DirPortFrontPage file. */
static char *global_dirfrontpagecontents = NULL;
/** List of port_cfg_t for all configured ports. */
static smartlist_t *configured_ports = NULL;

/** Return the contents of our frontpage string, or NULL if not configured. */
const char *
get_dirportfrontpage(void)
{
  return global_dirfrontpagecontents;
}

/** Allocate an empty configuration object of a given format type. */
static void *
config_alloc(const config_format_t *fmt)
{
  void *opts = tor_malloc_zero(fmt->size);
  *(uint32_t*)STRUCT_VAR_P(opts, fmt->magic_offset) = fmt->magic;
  CHECK(fmt, opts);
  return opts;
}

/** Return the currently configured options. */
or_options_t *
get_options_mutable(void)
{
  tor_assert(global_options);
  return global_options;
}

/** Returns the currently configured options */
const or_options_t *
get_options(void)
{
  return get_options_mutable();
}

/** Change the current global options to contain <b>new_val</b> instead of
 * their current value; take action based on the new value; free the old value
 * as necessary.  Returns 0 on success, -1 on failure.
 */
int
set_options(or_options_t *new_val, char **msg)
{
  int i;
  smartlist_t *elements;
  config_line_t *line;
  or_options_t *old_options = global_options;
  global_options = new_val;
  /* Note that we pass the *old* options below, for comparison. It
   * pulls the new options directly out of global_options. */
  if (options_act_reversible(old_options, msg)<0) {
    tor_assert(*msg);
    global_options = old_options;
    return -1;
  }
  if (options_act(old_options) < 0) { /* acting on the options failed. die. */
    log_err(LD_BUG,
            "Acting on config options left us in a broken state. Dying.");
    exit(1);
  }
  /* Issues a CONF_CHANGED event to notify controller of the change. If Tor is
   * just starting up then the old_options will be undefined. */
  if (old_options) {
    elements = smartlist_create();
    for (i=0; options_format.vars[i].name; ++i) {
      const config_var_t *var = &options_format.vars[i];
      const char *var_name = var->name;
      if (var->type == CONFIG_TYPE_LINELIST_S ||
          var->type == CONFIG_TYPE_OBSOLETE) {
        continue;
      }
      if (!option_is_same(&options_format, new_val, old_options, var_name)) {
        line = get_assigned_option(&options_format, new_val, var_name, 1);

        if (line) {
          for (; line; line = line->next) {
            smartlist_add(elements, line->key);
            smartlist_add(elements, line->value);
          }
        } else {
          smartlist_add(elements, (char*)options_format.vars[i].name);
          smartlist_add(elements, NULL);
        }
      }
    }
    control_event_conf_changed(elements);
    smartlist_free(elements);
  }
  config_free(&options_format, old_options);

  return 0;
}

extern const char tor_git_revision[]; /* from tor_main.c */

/** The version of this Tor process, as parsed. */
static char *_version = NULL;

/** Return the current Tor version. */
const char *
get_version(void)
{
  if (_version == NULL) {
    if (strlen(tor_git_revision)) {
      size_t len = strlen(VERSION)+strlen(tor_git_revision)+16;
      _version = tor_malloc(len);
      tor_snprintf(_version, len, "%s (git-%s)", VERSION, tor_git_revision);
    } else {
      _version = tor_strdup(VERSION);
    }
  }
  return _version;
}

/** Release additional memory allocated in options
 */
static void
or_options_free(or_options_t *options)
{
  if (!options)
    return;

  routerset_free(options->_ExcludeExitNodesUnion);
  if (options->NodeFamilySets) {
    SMARTLIST_FOREACH(options->NodeFamilySets, routerset_t *,
                      rs, routerset_free(rs));
    smartlist_free(options->NodeFamilySets);
  }
  config_free(&options_format, options);
}

/** Release all memory and resources held by global configuration structures.
 */
void
config_free_all(void)
{
  or_options_free(global_options);
  global_options = NULL;
  or_options_free(global_default_options);
  global_default_options = NULL;

  config_free(&state_format, global_state);
  global_state = NULL;

  config_free_lines(global_cmdline_options);
  global_cmdline_options = NULL;

  if (configured_ports) {
    SMARTLIST_FOREACH(configured_ports,
                      port_cfg_t *, p, tor_free(p));
    smartlist_free(configured_ports);
    configured_ports = NULL;
  }

  tor_free(torrc_fname);
  tor_free(torrc_defaults_fname);
  tor_free(_version);
  tor_free(global_dirfrontpagecontents);
}

/** Make <b>address</b> -- a piece of information related to our operation as
 * a client -- safe to log according to the settings in options->SafeLogging,
 * and return it.
 *
 * (We return "[scrubbed]" if SafeLogging is "1", and address otherwise.)
 */
const char *
safe_str_client(const char *address)
{
  tor_assert(address);
  if (get_options()->_SafeLogging == SAFELOG_SCRUB_ALL)
    return "[scrubbed]";
  else
    return address;
}

/** Make <b>address</b> -- a piece of information of unspecified sensitivity
 * -- safe to log according to the settings in options->SafeLogging, and
 * return it.
 *
 * (We return "[scrubbed]" if SafeLogging is anything besides "0", and address
 * otherwise.)
 */
const char *
safe_str(const char *address)
{
  tor_assert(address);
  if (get_options()->_SafeLogging != SAFELOG_SCRUB_NONE)
    return "[scrubbed]";
  else
    return address;
}

/** Equivalent to escaped(safe_str_client(address)).  See reentrancy note on
 * escaped(): don't use this outside the main thread, or twice in the same
 * log statement. */
const char *
escaped_safe_str_client(const char *address)
{
  if (get_options()->_SafeLogging == SAFELOG_SCRUB_ALL)
    return "[scrubbed]";
  else
    return escaped(address);
}

/** Equivalent to escaped(safe_str(address)).  See reentrancy note on
 * escaped(): don't use this outside the main thread, or twice in the same
 * log statement. */
const char *
escaped_safe_str(const char *address)
{
  if (get_options()->_SafeLogging != SAFELOG_SCRUB_NONE)
    return "[scrubbed]";
  else
    return escaped(address);
}

/** Add the default directory authorities directly into the trusted dir list,
 * but only add them insofar as they share bits with <b>type</b>. */
static void
add_default_trusted_dir_authorities(dirinfo_type_t type)
{
  int i;
  const char *dirservers[] = {
    "moria1 orport=9101 no-v2 "
      "v3ident=D586D18309DED4CD6D57C18FDB97EFA96D330566 "
      "128.31.0.39:9131 9695 DFC3 5FFE B861 329B 9F1A B04C 4639 7020 CE31",
    "tor26 v1 orport=443 v3ident=14C131DFC5C6F93646BE72FA1401C02A8DF2E8B4 "
      "86.59.21.38:80 847B 1F85 0344 D787 6491 A548 92F9 0493 4E4E B85D",
    "dizum orport=443 v3ident=E8A9C45EDE6D711294FADF8E7951F4DE6CA56B58 "
      "194.109.206.212:80 7EA6 EAD6 FD83 083C 538F 4403 8BBF A077 587D D755",
    "Tonga orport=443 bridge no-v2 82.94.251.203:80 "
      "4A0C CD2D DC79 9508 3D73 F5D6 6710 0C8A 5831 F16D",
    "ides orport=9090 no-v2 v3ident=27B6B5996C426270A5C95488AA5BCEB6BCC86956 "
      "216.224.124.114:9030 F397 038A DC51 3361 35E7 B80B D99C A384 4360 292B",
    "gabelmoo orport=443 no-v2 "
      "v3ident=ED03BB616EB2F60BEC80151114BB25CEF515B226 "
      "212.112.245.170:80 F204 4413 DAC2 E02E 3D6B CF47 35A1 9BCA 1DE9 7281",
    "dannenberg orport=443 no-v2 "
      "v3ident=585769C78764D58426B8B52B6651A5A71137189A "
      "193.23.244.244:80 7BE6 83E6 5D48 1413 21C5 ED92 F075 C553 64AC 7123",
    "urras orport=80 no-v2 v3ident=80550987E1D626E3EBA5E5E75A458DE0626D088C "
      "208.83.223.34:443 0AD3 FA88 4D18 F89E EA2D 89C0 1937 9E0E 7FD9 4417",
    "maatuska orport=80 no-v2 "
      "v3ident=49015F787433103580E3B66A1707A00E60F2D15B "
      "213.115.239.118:443 BD6A 8292 55CB 08E6 6FBE 7D37 4836 3586 E46B 3810",
    NULL
  };
  for (i=0; dirservers[i]; i++) {
    if (parse_dir_server_line(dirservers[i], type, 0)<0) {
      log_err(LD_BUG, "Couldn't parse internal dirserver line %s",
              dirservers[i]);
    }
  }
}

/** Look at all the config options for using alternate directory
 * authorities, and make sure none of them are broken. Also, warn the
 * user if we changed any dangerous ones.
 */
static int
validate_dir_authorities(or_options_t *options, or_options_t *old_options)
{
  config_line_t *cl;

  if (options->DirServers &&
      (options->AlternateDirAuthority || options->AlternateBridgeAuthority ||
       options->AlternateHSAuthority)) {
    log_warn(LD_CONFIG,
             "You cannot set both DirServers and Alternate*Authority.");
    return -1;
  }

  /* do we want to complain to the user about being partitionable? */
  if ((options->DirServers &&
       (!old_options ||
        !config_lines_eq(options->DirServers, old_options->DirServers))) ||
      (options->AlternateDirAuthority &&
       (!old_options ||
        !config_lines_eq(options->AlternateDirAuthority,
                         old_options->AlternateDirAuthority)))) {
    log_warn(LD_CONFIG,
             "You have used DirServer or AlternateDirAuthority to "
             "specify alternate directory authorities in "
             "your configuration. This is potentially dangerous: it can "
             "make you look different from all other Tor users, and hurt "
             "your anonymity. Even if you've specified the same "
             "authorities as Tor uses by default, the defaults could "
             "change in the future. Be sure you know what you're doing.");
  }

  /* Now go through the four ways you can configure an alternate
   * set of directory authorities, and make sure none are broken. */
  for (cl = options->DirServers; cl; cl = cl->next)
    if (parse_dir_server_line(cl->value, NO_DIRINFO, 1)<0)
      return -1;
  for (cl = options->AlternateBridgeAuthority; cl; cl = cl->next)
    if (parse_dir_server_line(cl->value, NO_DIRINFO, 1)<0)
      return -1;
  for (cl = options->AlternateDirAuthority; cl; cl = cl->next)
    if (parse_dir_server_line(cl->value, NO_DIRINFO, 1)<0)
      return -1;
  for (cl = options->AlternateHSAuthority; cl; cl = cl->next)
    if (parse_dir_server_line(cl->value, NO_DIRINFO, 1)<0)
      return -1;
  return 0;
}

/** Look at all the config options and assign new dir authorities
 * as appropriate.
 */
static int
consider_adding_dir_authorities(const or_options_t *options,
                                const or_options_t *old_options)
{
  config_line_t *cl;
  int need_to_update =
    !smartlist_len(router_get_trusted_dir_servers()) || !old_options ||
    !config_lines_eq(options->DirServers, old_options->DirServers) ||
    !config_lines_eq(options->AlternateBridgeAuthority,
                     old_options->AlternateBridgeAuthority) ||
    !config_lines_eq(options->AlternateDirAuthority,
                     old_options->AlternateDirAuthority) ||
    !config_lines_eq(options->AlternateHSAuthority,
                     old_options->AlternateHSAuthority);

  if (!need_to_update)
    return 0; /* all done */

  /* Start from a clean slate. */
  clear_trusted_dir_servers();

  if (!options->DirServers) {
    /* then we may want some of the defaults */
    dirinfo_type_t type = NO_DIRINFO;
    if (!options->AlternateBridgeAuthority)
      type |= BRIDGE_DIRINFO;
    if (!options->AlternateDirAuthority)
      type |= V1_DIRINFO | V2_DIRINFO | V3_DIRINFO | EXTRAINFO_DIRINFO |
        MICRODESC_DIRINFO;
    if (!options->AlternateHSAuthority)
      type |= HIDSERV_DIRINFO;
    add_default_trusted_dir_authorities(type);
  }

  for (cl = options->DirServers; cl; cl = cl->next)
    if (parse_dir_server_line(cl->value, NO_DIRINFO, 0)<0)
      return -1;
  for (cl = options->AlternateBridgeAuthority; cl; cl = cl->next)
    if (parse_dir_server_line(cl->value, NO_DIRINFO, 0)<0)
      return -1;
  for (cl = options->AlternateDirAuthority; cl; cl = cl->next)
    if (parse_dir_server_line(cl->value, NO_DIRINFO, 0)<0)
      return -1;
  for (cl = options->AlternateHSAuthority; cl; cl = cl->next)
    if (parse_dir_server_line(cl->value, NO_DIRINFO, 0)<0)
      return -1;
  return 0;
}

/** Fetch the active option list, and take actions based on it. All of the
 * things we do should survive being done repeatedly.  If present,
 * <b>old_options</b> contains the previous value of the options.
 *
 * Return 0 if all goes well, return -1 if things went badly.
 */
static int
options_act_reversible(const or_options_t *old_options, char **msg)
{
  smartlist_t *new_listeners = smartlist_create();
  smartlist_t *replaced_listeners = smartlist_create();
  static int libevent_initialized = 0;
  or_options_t *options = get_options_mutable();
  int running_tor = options->command == CMD_RUN_TOR;
  int set_conn_limit = 0;
  int r = -1;
  int logs_marked = 0;

  /* Daemonize _first_, since we only want to open most of this stuff in
   * the subprocess.  Libevent bases can't be reliably inherited across
   * processes. */
  if (running_tor && options->RunAsDaemon) {
    /* No need to roll back, since you can't change the value. */
    start_daemon();
  }

#ifndef HAVE_SYS_UN_H
  if (options->ControlSocket || options->ControlSocketsGroupWritable) {
    *msg = tor_strdup("Unix domain sockets (ControlSocket) not supported "
                      "on this OS/with this build.");
    goto rollback;
  }
#else
  if (options->ControlSocketsGroupWritable && !options->ControlSocket) {
    *msg = tor_strdup("Setting ControlSocketGroupWritable without setting"
                      "a ControlSocket makes no sense.");
    goto rollback;
  }
#endif

  if (running_tor) {
    int n_ports=0;
    /* We need to set the connection limit before we can open the listeners. */
    if (set_max_file_descriptors((unsigned)options->ConnLimit,
                                 &options->_ConnLimit) < 0) {
      *msg = tor_strdup("Problem with ConnLimit value. See logs for details.");
      goto rollback;
    }
    set_conn_limit = 1;

    /* Set up libevent.  (We need to do this before we can register the
     * listeners as listeners.) */
    if (running_tor && !libevent_initialized) {
      init_libevent(options);
      libevent_initialized = 1;
    }

    /* Adjust the port configuration so we can launch listeners. */
    if (parse_ports(options, 0, msg, &n_ports)) {
      if (!*msg)
        *msg = tor_strdup("Unexpected problem parsing port config");
      goto rollback;
    }

    /* Set the hibernation state appropriately.*/
    consider_hibernation(time(NULL));

    /* Launch the listeners.  (We do this before we setuid, so we can bind to
     * ports under 1024.)  We don't want to rebind if we're hibernating. If
     * networking is disabled, this will close all but the control listeners,
     * but disable those. */
    if (!we_are_hibernating()) {
      if (retry_all_listeners(replaced_listeners, new_listeners) < 0) {
        *msg = tor_strdup("Failed to bind one of the listener ports.");
        goto rollback;
      }
    }
    if (options->DisableNetwork) {
      /* Aggressively close non-controller stuff, NOW */
      log_notice(LD_NET, "DisableNetwork is set. Tor will not make or accept "
                 "non-control network connections. Shutting down all existing "
                 "connections.");
      connection_mark_all_noncontrol_connections();
    }
  }

#if defined(HAVE_NET_IF_H) && defined(HAVE_NET_PFVAR_H)
  /* Open /dev/pf before dropping privileges. */
  if (options->TransPort) {
    if (get_pf_socket() < 0) {
      *msg = tor_strdup("Unable to open /dev/pf for transparent proxy.");
      goto rollback;
    }
  }
#endif

  /* Attempt to lock all current and future memory with mlockall() only once */
  if (options->DisableAllSwap) {
    if (tor_mlockall() == -1) {
      *msg = tor_strdup("DisableAllSwap failure. Do you have proper "
                        "permissions?");
      goto done;
    }
  }

  /* Setuid/setgid as appropriate */
  if (options->User) {
    if (switch_id(options->User) != 0) {
      /* No need to roll back, since you can't change the value. */
      *msg = tor_strdup("Problem with User value. See logs for details.");
      goto done;
    }
  }

  /* Ensure data directory is private; create if possible. */
  if (check_private_dir(options->DataDirectory,
                        running_tor ? CPD_CREATE : CPD_CHECK,
                        options->User)<0) {
    tor_asprintf(msg,
              "Couldn't access/create private data directory \"%s\"",
              options->DataDirectory);
    goto done;
    /* No need to roll back, since you can't change the value. */
  }

  /* Write control ports to disk as appropriate */
  control_ports_write_to_file();

  if (directory_caches_v2_dir_info(options)) {
    size_t len = strlen(options->DataDirectory)+32;
    char *fn = tor_malloc(len);
    tor_snprintf(fn, len, "%s"PATH_SEPARATOR"cached-status",
                 options->DataDirectory);
    if (check_private_dir(fn, running_tor ? CPD_CREATE : CPD_CHECK,
                          options->User) < 0) {
      tor_asprintf(msg,
                "Couldn't access/create private data directory \"%s\"", fn);
      tor_free(fn);
      goto done;
    }
    tor_free(fn);
  }

  /* Bail out at this point if we're not going to be a client or server:
   * we don't run Tor itself. */
  if (!running_tor)
    goto commit;

  mark_logs_temp(); /* Close current logs once new logs are open. */
  logs_marked = 1;
  if (options_init_logs(options, 0)<0) { /* Configure the log(s) */
    *msg = tor_strdup("Failed to init Log options. See logs for details.");
    goto rollback;
  }

 commit:
  r = 0;
  if (logs_marked) {
    log_severity_list_t *severity =
      tor_malloc_zero(sizeof(log_severity_list_t));
    close_temp_logs();
    add_callback_log(severity, control_event_logmsg);
    control_adjust_event_log_severity();
    tor_free(severity);
  }
  SMARTLIST_FOREACH(replaced_listeners, connection_t *, conn,
  {
    log_notice(LD_NET, "Closing old %s on %s:%d",
               conn_type_to_string(conn->type), conn->address, conn->port);
    connection_close_immediate(conn);
    connection_mark_for_close(conn);
  });
  goto done;

 rollback:
  r = -1;
  tor_assert(*msg);

  if (logs_marked) {
    rollback_log_changes();
    control_adjust_event_log_severity();
  }

  if (set_conn_limit && old_options)
    set_max_file_descriptors((unsigned)old_options->ConnLimit,
                             &options->_ConnLimit);

  SMARTLIST_FOREACH(new_listeners, connection_t *, conn,
  {
    log_notice(LD_NET, "Closing partially-constructed listener %s on %s:%d",
               conn_type_to_string(conn->type), conn->address, conn->port);
    connection_close_immediate(conn);
    connection_mark_for_close(conn);
  });

 done:
  smartlist_free(new_listeners);
  smartlist_free(replaced_listeners);
  return r;
}

/** If we need to have a GEOIP ip-to-country map to run with our configured
 * options, return 1 and set *<b>reason_out</b> to a description of why. */
int
options_need_geoip_info(const or_options_t *options, const char **reason_out)
{
  int bridge_usage =
    options->BridgeRelay && options->BridgeRecordUsageByCountry;
  int routerset_usage =
    routerset_needs_geoip(options->EntryNodes) ||
    routerset_needs_geoip(options->ExitNodes) ||
    routerset_needs_geoip(options->ExcludeExitNodes) ||
    routerset_needs_geoip(options->ExcludeNodes);

  if (routerset_usage && reason_out) {
    *reason_out = "We've been configured to use (or avoid) nodes in certain "
      "countries, and we need GEOIP information to figure out which ones they "
      "are.";
  } else if (bridge_usage && reason_out) {
    *reason_out = "We've been configured to see which countries can access "
      "us as a bridge, and we need GEOIP information to tell which countries "
      "clients are in.";
  }
  return bridge_usage || routerset_usage;
}

/** Return the bandwidthrate that we are going to report to the authorities
 * based on the config options. */
uint32_t
get_effective_bwrate(const or_options_t *options)
{
  uint64_t bw = options->BandwidthRate;
  if (bw > options->MaxAdvertisedBandwidth)
    bw = options->MaxAdvertisedBandwidth;
  if (options->RelayBandwidthRate > 0 && bw > options->RelayBandwidthRate)
    bw = options->RelayBandwidthRate;
  /* ensure_bandwidth_cap() makes sure that this cast can't overflow. */
  return (uint32_t)bw;
}

/** Return the bandwidthburst that we are going to report to the authorities
 * based on the config options. */
uint32_t
get_effective_bwburst(const or_options_t *options)
{
  uint64_t bw = options->BandwidthBurst;
  if (options->RelayBandwidthBurst > 0 && bw > options->RelayBandwidthBurst)
    bw = options->RelayBandwidthBurst;
  /* ensure_bandwidth_cap() makes sure that this cast can't overflow. */
  return (uint32_t)bw;
}

/** Return True if any changes from <b>old_options</b> to
 * <b>new_options</b> needs us to refresh our TLS context. */
static int
options_transition_requires_fresh_tls_context(const or_options_t *old_options,
                                              const or_options_t *new_options)
{
  tor_assert(new_options);

  if (!old_options)
    return 0;

  if ((old_options->DynamicDHGroups != new_options->DynamicDHGroups)) {
    return 1;
  }

  return 0;
}

/** Fetch the active option list, and take actions based on it. All of the
 * things we do should survive being done repeatedly.  If present,
 * <b>old_options</b> contains the previous value of the options.
 *
 * Return 0 if all goes well, return -1 if it's time to die.
 *
 * Note: We haven't moved all the "act on new configuration" logic
 * here yet.  Some is still in do_hup() and other places.
 */
static int
options_act(const or_options_t *old_options)
{
  config_line_t *cl;
  or_options_t *options = get_options_mutable();
  int running_tor = options->command == CMD_RUN_TOR;
  char *msg;
  const int transition_affects_workers =
    old_options && options_transition_affects_workers(old_options, options);

   /* disable ptrace and later, other basic debugging techniques */
  if (options->DisableDebuggerAttachment) {
    tor_disable_debugger_attach();
  } else {
    log_notice(LD_CONFIG,"Debugger attachment enabled "
               "for unprivileged users.");
  }

  if (running_tor && !have_lockfile()) {
    if (try_locking(options, 1) < 0)
      return -1;
  }

  if (consider_adding_dir_authorities(options, old_options) < 0)
    return -1;

#ifdef NON_ANONYMOUS_MODE_ENABLED
  log(LOG_WARN, LD_GENERAL, "This copy of Tor was compiled to run in a "
      "non-anonymous mode. It will provide NO ANONYMITY.");
#endif

#ifdef ENABLE_TOR2WEB_MODE
  if (!options->Tor2webMode) {
    log_err(LD_CONFIG, "This copy of Tor was compiled to run in "
            "'tor2web mode'. It can only be run with the Tor2webMode torrc "
            "option enabled.");
    return -1;
  }
#else
  if (options->Tor2webMode) {
    log_err(LD_CONFIG, "This copy of Tor was not compiled to run in "
            "'tor2web mode'. It cannot be run with the Tor2webMode torrc "
            "option enabled. To enable Tor2webMode recompile with the "
            "--enable-tor2webmode option.");
    return -1;
  }
#endif

  if (options->Bridges) {
    mark_bridge_list();
    for (cl = options->Bridges; cl; cl = cl->next) {
      if (parse_bridge_line(cl->value, 0)<0) {
        log_warn(LD_BUG,
                 "Previously validated Bridge line could not be added!");
        return -1;
      }
    }
    sweep_bridge_list();
  }

  if (running_tor && rend_config_services(options, 0)<0) {
    log_warn(LD_BUG,
       "Previously validated hidden services line could not be added!");
    return -1;
  }

  if (running_tor && rend_parse_service_authorization(options, 0) < 0) {
    log_warn(LD_BUG, "Previously validated client authorization for "
                     "hidden services could not be added!");
    return -1;
  }

  /* Load state */
  if (! global_state && running_tor) {
    if (or_state_load())
      return -1;
    rep_hist_load_mtbf_data(time(NULL));
  }

  mark_transport_list();
  pt_prepare_proxy_list_for_config_read();
  if (options->ClientTransportPlugin) {
    for (cl = options->ClientTransportPlugin; cl; cl = cl->next) {
      if (parse_client_transport_line(cl->value, 0)<0) {
        log_warn(LD_BUG,
                 "Previously validated ClientTransportPlugin line "
                 "could not be added!");
        return -1;
      }
    }
  }

  if (options->ServerTransportPlugin) {
    for (cl = options->ServerTransportPlugin; cl; cl = cl->next) {
      if (parse_server_transport_line(cl->value, 0)<0) {
        log_warn(LD_BUG,
                 "Previously validated ServerTransportPlugin line "
                 "could not be added!");
        return -1;
      }
    }
  }
  sweep_transport_list();
  sweep_proxy_list();

  /* Bail out at this point if we're not going to be a client or server:
   * we want to not fork, and to log stuff to stderr. */
  if (!running_tor)
    return 0;

  /* Finish backgrounding the process */
  if (options->RunAsDaemon) {
    /* We may be calling this for the n'th time (on SIGHUP), but it's safe. */
    finish_daemon(options->DataDirectory);
  }

  /* If needed, generate a new TLS DH prime according to the current torrc. */
  if (server_mode(options)) {
    if (!old_options) {
      if (options->DynamicDHGroups) {
        char *fname = get_datadir_fname2("keys", "dynamic_dh_params");
        crypto_set_tls_dh_prime(fname);
        tor_free(fname);
      } else {
        crypto_set_tls_dh_prime(NULL);
      }
    } else {
      if (options->DynamicDHGroups && !old_options->DynamicDHGroups) {
        char *fname = get_datadir_fname2("keys", "dynamic_dh_params");
        crypto_set_tls_dh_prime(fname);
        tor_free(fname);
      } else if (!options->DynamicDHGroups && old_options->DynamicDHGroups) {
        crypto_set_tls_dh_prime(NULL);
      }
    }
  } else { /* clients don't need a dynamic DH prime. */
    crypto_set_tls_dh_prime(NULL);
  }

  /* We want to reinit keys as needed before we do much of anything else:
     keys are important, and other things can depend on them. */
  if (transition_affects_workers ||
      (options->V3AuthoritativeDir && (!old_options ||
                                       !old_options->V3AuthoritativeDir))) {
    if (init_keys() < 0) {
      log_warn(LD_BUG,"Error initializing keys; exiting");
      return -1;
    }
  } else if (old_options &&
             options_transition_requires_fresh_tls_context(old_options,
                                                           options)) {
    if (router_initialize_tls_context() < 0) {
      log_warn(LD_BUG,"Error initializing TLS context.");
      return -1;
    }
  }

  /* Write our PID to the PID file. If we do not have write permissions we
   * will log a warning */
  if (options->PidFile)
    write_pidfile(options->PidFile);

  /* Register addressmap directives */
  config_register_addressmaps(options);
  parse_virtual_addr_network(options->VirtualAddrNetwork, 0, &msg);

  /* Update address policies. */
  if (policies_parse_from_options(options) < 0) {
    /* This should be impossible, but let's be sure. */
    log_warn(LD_BUG,"Error parsing already-validated policy options.");
    return -1;
  }

  if (init_cookie_authentication(options->CookieAuthentication) < 0) {
    log_warn(LD_CONFIG,"Error creating cookie authentication file.");
    return -1;
  }

  monitor_owning_controller_process(options->OwningControllerProcess);

  /* reload keys as needed for rendezvous services. */
  if (rend_service_load_keys()<0) {
    log_warn(LD_GENERAL,"Error loading rendezvous service keys");
    return -1;
  }

  /* Set up accounting */
  if (accounting_parse_options(options, 0)<0) {
    log_warn(LD_CONFIG,"Error in accounting options");
    return -1;
  }
  if (accounting_is_enabled(options))
    configure_accounting(time(NULL));

#ifdef USE_BUFFEREVENTS
  /* If we're using the bufferevents implementation and our rate limits
   * changed, we need to tell the rate-limiting system about it. */
  if (!old_options ||
      old_options->BandwidthRate != options->BandwidthRate ||
      old_options->BandwidthBurst != options->BandwidthBurst ||
      old_options->RelayBandwidthRate != options->RelayBandwidthRate ||
      old_options->RelayBandwidthBurst != options->RelayBandwidthBurst)
    connection_bucket_init();
#endif

  /* Change the cell EWMA settings */
  cell_ewma_set_scale_factor(options, networkstatus_get_latest_consensus());

  /* Check for transitions that need action. */
  if (old_options) {
    int revise_trackexithosts = 0;
    int revise_automap_entries = 0;
    if ((options->UseEntryGuards && !old_options->UseEntryGuards) ||
        options->UseBridges != old_options->UseBridges ||
        (options->UseBridges &&
         !config_lines_eq(options->Bridges, old_options->Bridges)) ||
        !routerset_equal(old_options->ExcludeNodes,options->ExcludeNodes) ||
        !routerset_equal(old_options->ExcludeExitNodes,
                         options->ExcludeExitNodes) ||
        !routerset_equal(old_options->EntryNodes, options->EntryNodes) ||
        !routerset_equal(old_options->ExitNodes, options->ExitNodes) ||
        options->StrictNodes != old_options->StrictNodes) {
      log_info(LD_CIRC,
               "Changed to using entry guards or bridges, or changed "
               "preferred or excluded node lists. "
               "Abandoning previous circuits.");
      circuit_mark_all_unused_circs();
      circuit_expire_all_dirty_circs();
      revise_trackexithosts = 1;
    }

    if (!smartlist_strings_eq(old_options->TrackHostExits,
                              options->TrackHostExits))
      revise_trackexithosts = 1;

    if (revise_trackexithosts)
      addressmap_clear_excluded_trackexithosts(options);

    if (!options->AutomapHostsOnResolve) {
      if (old_options->AutomapHostsOnResolve)
        revise_automap_entries = 1;
    } else {
      if (!smartlist_strings_eq(old_options->AutomapHostsSuffixes,
                                options->AutomapHostsSuffixes))
        revise_automap_entries = 1;
      else if (!opt_streq(old_options->VirtualAddrNetwork,
                          options->VirtualAddrNetwork))
        revise_automap_entries = 1;
    }

    if (revise_automap_entries)
      addressmap_clear_invalid_automaps(options);

/* How long should we delay counting bridge stats after becoming a bridge?
 * We use this so we don't count people who used our bridge thinking it is
 * a relay. If you change this, don't forget to change the log message
 * below. It's 4 hours (the time it takes to stop being used by clients)
 * plus some extra time for clock skew. */
#define RELAY_BRIDGE_STATS_DELAY (6 * 60 * 60)

    if (! bool_eq(options->BridgeRelay, old_options->BridgeRelay)) {
      int was_relay = 0;
      if (options->BridgeRelay) {
        time_t int_start = time(NULL);
        if (config_lines_eq(old_options->ORPort, options->ORPort)) {
          int_start += RELAY_BRIDGE_STATS_DELAY;
          was_relay = 1;
        }
        geoip_bridge_stats_init(int_start);
        log_info(LD_CONFIG, "We are acting as a bridge now.  Starting new "
                 "GeoIP stats interval%s.", was_relay ? " in 6 "
                 "hours from now" : "");
      } else {
        geoip_bridge_stats_term();
        log_info(LD_GENERAL, "We are no longer acting as a bridge.  "
                 "Forgetting GeoIP stats.");
      }
    }

    if (transition_affects_workers) {
      log_info(LD_GENERAL,
               "Worker-related options changed. Rotating workers.");

      if (server_mode(options) && !server_mode(old_options)) {
        ip_address_changed(0);
        if (can_complete_circuit || !any_predicted_circuits(time(NULL)))
          inform_testing_reachability();
      }
      cpuworkers_rotate();
      if (dns_reset())
        return -1;
    } else {
      if (dns_reset())
        return -1;
    }

    if (options->PerConnBWRate != old_options->PerConnBWRate ||
        options->PerConnBWBurst != old_options->PerConnBWBurst)
      connection_or_update_token_buckets(get_connection_array(), options);
  }

  /* Maybe load geoip file */
  if (options->GeoIPFile &&
      ((!old_options || !opt_streq(old_options->GeoIPFile, options->GeoIPFile))
       || !geoip_is_loaded())) {
    /* XXXX Don't use this "<default>" junk; make our filename options
     * understand prefixes somehow. -NM */
    /* XXXX023 Reload GeoIPFile on SIGHUP. -NM */
    char *actual_fname = tor_strdup(options->GeoIPFile);
#ifdef WIN32
    if (!strcmp(actual_fname, "<default>")) {
      const char *conf_root = get_windows_conf_root();
      size_t len = strlen(conf_root)+16;
      tor_free(actual_fname);
      actual_fname = tor_malloc(len+1);
      tor_snprintf(actual_fname, len, "%s\\geoip", conf_root);
    }
#endif
    geoip_load_file(actual_fname, options);
    tor_free(actual_fname);
  }

  if (options->CellStatistics || options->DirReqStatistics ||
      options->EntryStatistics || options->ExitPortStatistics ||
      options->ConnDirectionStatistics ||
      options->BridgeAuthoritativeDir) {
    time_t now = time(NULL);
    int print_notice = 0;

    /* If we aren't acting as a server, we can't collect stats anyway. */
    if (!server_mode(options)) {
      options->CellStatistics = 0;
      options->DirReqStatistics = 0;
      options->EntryStatistics = 0;
      options->ExitPortStatistics = 0;
    }

    if ((!old_options || !old_options->CellStatistics) &&
        options->CellStatistics) {
      rep_hist_buffer_stats_init(now);
      print_notice = 1;
    }
    if ((!old_options || !old_options->DirReqStatistics) &&
        options->DirReqStatistics) {
      if (geoip_is_loaded()) {
        geoip_dirreq_stats_init(now);
        print_notice = 1;
      } else {
        options->DirReqStatistics = 0;
        /* Don't warn Tor clients, they don't use statistics */
        if (options->ORPort)
          log_notice(LD_CONFIG, "Configured to measure directory request "
                                "statistics, but no GeoIP database found. "
                                "Please specify a GeoIP database using the "
                                "GeoIPFile option.");
      }
    }
    if ((!old_options || !old_options->EntryStatistics) &&
        options->EntryStatistics && !should_record_bridge_info(options)) {
      if (geoip_is_loaded()) {
        geoip_entry_stats_init(now);
        print_notice = 1;
      } else {
        options->EntryStatistics = 0;
        log_notice(LD_CONFIG, "Configured to measure entry node "
                              "statistics, but no GeoIP database found. "
                              "Please specify a GeoIP database using the "
                              "GeoIPFile option.");
      }
    }
    if ((!old_options || !old_options->ExitPortStatistics) &&
        options->ExitPortStatistics) {
      rep_hist_exit_stats_init(now);
      print_notice = 1;
    }
    if ((!old_options || !old_options->ConnDirectionStatistics) &&
        options->ConnDirectionStatistics) {
      rep_hist_conn_stats_init(now);
    }
    if ((!old_options || !old_options->BridgeAuthoritativeDir) &&
        options->BridgeAuthoritativeDir) {
      rep_hist_desc_stats_init(now);
      print_notice = 1;
    }
    if (print_notice)
      log_notice(LD_CONFIG, "Configured to measure statistics. Look for "
                 "the *-stats files that will first be written to the "
                 "data directory in 24 hours from now.");
  }

  if (old_options && old_options->CellStatistics &&
      !options->CellStatistics)
    rep_hist_buffer_stats_term();
  if (old_options && old_options->DirReqStatistics &&
      !options->DirReqStatistics)
    geoip_dirreq_stats_term();
  if (old_options && old_options->EntryStatistics &&
      !options->EntryStatistics)
    geoip_entry_stats_term();
  if (old_options && old_options->ExitPortStatistics &&
      !options->ExitPortStatistics)
    rep_hist_exit_stats_term();
  if (old_options && old_options->ConnDirectionStatistics &&
      !options->ConnDirectionStatistics)
    rep_hist_conn_stats_term();
  if (old_options && old_options->BridgeAuthoritativeDir &&
      !options->BridgeAuthoritativeDir)
    rep_hist_desc_stats_term();

  /* Check if we need to parse and add the EntryNodes config option. */
  if (options->EntryNodes &&
      (!old_options ||
       !routerset_equal(old_options->EntryNodes,options->EntryNodes) ||
       !routerset_equal(old_options->ExcludeNodes,options->ExcludeNodes)))
    entry_nodes_should_be_added();

  /* Since our options changed, we might need to regenerate and upload our
   * server descriptor.
   */
  if (!old_options ||
      options_transition_affects_descriptor(old_options, options))
    mark_my_descriptor_dirty("config change");

  /* We may need to reschedule some directory stuff if our status changed. */
  if (old_options) {
    if (authdir_mode_v3(options) && !authdir_mode_v3(old_options))
      dirvote_recalculate_timing(options, time(NULL));
    if (!bool_eq(directory_fetches_dir_info_early(options),
                 directory_fetches_dir_info_early(old_options)) ||
        !bool_eq(directory_fetches_dir_info_later(options),
                 directory_fetches_dir_info_later(old_options))) {
      /* Make sure update_router_have_min_dir_info gets called. */
      router_dir_info_changed();
      /* We might need to download a new consensus status later or sooner than
       * we had expected. */
      update_consensus_networkstatus_fetch_time(time(NULL));
    }
  }

  /* Load the webpage we're going to serve every time someone asks for '/' on
     our DirPort. */
  tor_free(global_dirfrontpagecontents);
  if (options->DirPortFrontPage) {
    global_dirfrontpagecontents =
      read_file_to_str(options->DirPortFrontPage, 0, NULL);
    if (!global_dirfrontpagecontents) {
      log_warn(LD_CONFIG,
               "DirPortFrontPage file '%s' not found. Continuing anyway.",
               options->DirPortFrontPage);
    }
  }

  return 0;
}

/*
 * Functions to parse config options
 */

/** If <b>option</b> is an official abbreviation for a longer option,
 * return the longer option.  Otherwise return <b>option</b>.
 * If <b>command_line</b> is set, apply all abbreviations.  Otherwise, only
 * apply abbreviations that work for the config file and the command line.
 * If <b>warn_obsolete</b> is set, warn about deprecated names. */
static const char *
expand_abbrev(const config_format_t *fmt, const char *option, int command_line,
              int warn_obsolete)
{
  int i;
  if (! fmt->abbrevs)
    return option;
  for (i=0; fmt->abbrevs[i].abbreviated; ++i) {
    /* Abbreviations are case insensitive. */
    if (!strcasecmp(option,fmt->abbrevs[i].abbreviated) &&
        (command_line || !fmt->abbrevs[i].commandline_only)) {
      if (warn_obsolete && fmt->abbrevs[i].warn) {
        log_warn(LD_CONFIG,
                 "The configuration option '%s' is deprecated; "
                 "use '%s' instead.",
                 fmt->abbrevs[i].abbreviated,
                 fmt->abbrevs[i].full);
      }
      /* Keep going through the list in case we want to rewrite it more.
       * (We could imagine recursing here, but I don't want to get the
       * user into an infinite loop if we craft our list wrong.) */
      option = fmt->abbrevs[i].full;
    }
  }
  return option;
}

/** Helper: Read a list of configuration options from the command line.
 * If successful, put them in *<b>result</b> and return 0, and return
 * -1 and leave *<b>result</b> alone. */
static int
config_get_commandlines(int argc, char **argv, config_line_t **result)
{
  config_line_t *front = NULL;
  config_line_t **new = &front;
  char *s;
  int i = 1;

  while (i < argc) {
    unsigned command = CONFIG_LINE_NORMAL;
    int want_arg = 1;

    if (!strcmp(argv[i],"-f") ||
        !strcmp(argv[i],"--defaults-torrc") ||
        !strcmp(argv[i],"--hash-password")) {
      i += 2; /* command-line option with argument. ignore them. */
      continue;
    } else if (!strcmp(argv[i],"--list-fingerprint") ||
               !strcmp(argv[i],"--verify-config") ||
               !strcmp(argv[i],"--ignore-missing-torrc") ||
               !strcmp(argv[i],"--quiet") ||
               !strcmp(argv[i],"--hush")) {
      i += 1; /* command-line option. ignore it. */
      continue;
    } else if (!strcmp(argv[i],"--nt-service") ||
               !strcmp(argv[i],"-nt-service")) {
      i += 1;
      continue;
    }

    *new = tor_malloc_zero(sizeof(config_line_t));
    s = argv[i];

    /* Each keyword may be prefixed with one or two dashes. */
    if (*s == '-')
      s++;
    if (*s == '-')
      s++;
    /* Figure out the command, if any. */
    if (*s == '+') {
      s++;
      command = CONFIG_LINE_APPEND;
    } else if (*s == '/') {
      s++;
      command = CONFIG_LINE_CLEAR;
      /* A 'clear' command has no argument. */
      want_arg = 0;
    }

    if (want_arg && i == argc-1) {
      log_warn(LD_CONFIG,"Command-line option '%s' with no value. Failing.",
               argv[i]);
      config_free_lines(front);
      return -1;
    }

    (*new)->key = tor_strdup(expand_abbrev(&options_format, s, 1, 1));
    (*new)->value = want_arg ? tor_strdup(argv[i+1]) : tor_strdup("");
    (*new)->command = command;
    (*new)->next = NULL;
    log(LOG_DEBUG, LD_CONFIG, "command line: parsed keyword '%s', value '%s'",
        (*new)->key, (*new)->value);

    new = &((*new)->next);
    i += want_arg ? 2 : 1;
  }
  *result = front;
  return 0;
}

/** Helper: allocate a new configuration option mapping 'key' to 'val',
 * append it to *<b>lst</b>. */
static void
config_line_append(config_line_t **lst,
                   const char *key,
                   const char *val)
{
  config_line_t *newline;

  newline = tor_malloc_zero(sizeof(config_line_t));
  newline->key = tor_strdup(key);
  newline->value = tor_strdup(val);
  newline->next = NULL;
  while (*lst)
    lst = &((*lst)->next);

  (*lst) = newline;
}

/** Helper: parse the config string and strdup into key/value
 * strings. Set *result to the list, or NULL if parsing the string
 * failed.  Return 0 on success, -1 on failure. Warn and ignore any
 * misformatted lines.
 *
 * If <b>extended</b> is set, then treat keys beginning with / and with + as
 * indicating "clear" and "append" respectively. */
int
config_get_lines(const char *string, config_line_t **result, int extended)
{
  config_line_t *list = NULL, **next;
  char *k, *v;

  next = &list;
  do {
    k = v = NULL;
    string = parse_config_line_from_str(string, &k, &v);
    if (!string) {
      config_free_lines(list);
      tor_free(k);
      tor_free(v);
      return -1;
    }
    if (k && v) {
      unsigned command = CONFIG_LINE_NORMAL;
      if (extended) {
        if (k[0] == '+') {
          char *k_new = tor_strdup(k+1);
          tor_free(k);
          k = k_new;
          command = CONFIG_LINE_APPEND;
        } else if (k[0] == '/') {
          char *k_new = tor_strdup(k+1);
          tor_free(k);
          k = k_new;
          tor_free(v);
          v = tor_strdup("");
          command = CONFIG_LINE_CLEAR;
        }
      }
      /* This list can get long, so we keep a pointer to the end of it
       * rather than using config_line_append over and over and getting
       * n^2 performance. */
      *next = tor_malloc_zero(sizeof(config_line_t));
      (*next)->key = k;
      (*next)->value = v;
      (*next)->next = NULL;
      (*next)->command = command;
      next = &((*next)->next);
    } else {
      tor_free(k);
      tor_free(v);
    }
  } while (*string);

  *result = list;
  return 0;
}

/**
 * Free all the configuration lines on the linked list <b>front</b>.
 */
void
config_free_lines(config_line_t *front)
{
  config_line_t *tmp;

  while (front) {
    tmp = front;
    front = tmp->next;

    tor_free(tmp->key);
    tor_free(tmp->value);
    tor_free(tmp);
  }
}

/** As config_find_option, but return a non-const pointer. */
static config_var_t *
config_find_option_mutable(config_format_t *fmt, const char *key)
{
  int i;
  size_t keylen = strlen(key);
  if (!keylen)
    return NULL; /* if they say "--" on the command line, it's not an option */
  /* First, check for an exact (case-insensitive) match */
  for (i=0; fmt->vars[i].name; ++i) {
    if (!strcasecmp(key, fmt->vars[i].name)) {
      return &fmt->vars[i];
    }
  }
  /* If none, check for an abbreviated match */
  for (i=0; fmt->vars[i].name; ++i) {
    if (!strncasecmp(key, fmt->vars[i].name, keylen)) {
      log_warn(LD_CONFIG, "The abbreviation '%s' is deprecated. "
               "Please use '%s' instead",
               key, fmt->vars[i].name);
      return &fmt->vars[i];
    }
  }
  /* Okay, unrecognized option */
  return NULL;
}

/** If <b>key</b> is a configuration option, return the corresponding const
 * config_var_t.  Otherwise, if <b>key</b> is a non-standard abbreviation,
 * warn, and return the corresponding const config_var_t.  Otherwise return
 * NULL.
 */
static const config_var_t *
config_find_option(const config_format_t *fmt, const char *key)
{
  return config_find_option_mutable((config_format_t*)fmt, key);
}

/** Return the number of option entries in <b>fmt</b>. */
static int
config_count_options(const config_format_t *fmt)
{
  int i;
  for (i=0; fmt->vars[i].name; ++i)
    ;
  return i;
}

/*
 * Functions to assign config options.
 */

/** <b>c</b>-\>key is known to be a real key. Update <b>options</b>
 * with <b>c</b>-\>value and return 0, or return -1 if bad value.
 *
 * Called from config_assign_line() and option_reset().
 */
static int
config_assign_value(const config_format_t *fmt, or_options_t *options,
                    config_line_t *c, char **msg)
{
  int i, ok;
  const config_var_t *var;
  void *lvalue;

  CHECK(fmt, options);

  var = config_find_option(fmt, c->key);
  tor_assert(var);

  lvalue = STRUCT_VAR_P(options, var->var_offset);

  switch (var->type) {

  case CONFIG_TYPE_PORT:
    if (!strcasecmp(c->value, "auto")) {
      *(int *)lvalue = CFG_AUTO_PORT;
      break;
    }
    /* fall through */
  case CONFIG_TYPE_UINT:
    i = (int)tor_parse_long(c->value, 10, 0,
                            var->type==CONFIG_TYPE_PORT ? 65535 : INT_MAX,
                            &ok, NULL);
    if (!ok) {
      tor_asprintf(msg,
          "Int keyword '%s %s' is malformed or out of bounds.",
          c->key, c->value);
      return -1;
    }
    *(int *)lvalue = i;
    break;

  case CONFIG_TYPE_INTERVAL: {
    i = config_parse_interval(c->value, &ok);
    if (!ok) {
      tor_asprintf(msg,
          "Interval '%s %s' is malformed or out of bounds.",
          c->key, c->value);
      return -1;
    }
    *(int *)lvalue = i;
    break;
  }

  case CONFIG_TYPE_MSEC_INTERVAL: {
    i = config_parse_msec_interval(c->value, &ok);
    if (!ok) {
      tor_asprintf(msg,
          "Msec interval '%s %s' is malformed or out of bounds.",
          c->key, c->value);
      return -1;
    }
    *(int *)lvalue = i;
    break;
  }

  case CONFIG_TYPE_MEMUNIT: {
    uint64_t u64 = config_parse_memunit(c->value, &ok);
    if (!ok) {
      tor_asprintf(msg,
          "Value '%s %s' is malformed or out of bounds.",
          c->key, c->value);
      return -1;
    }
    *(uint64_t *)lvalue = u64;
    break;
  }

  case CONFIG_TYPE_BOOL:
    i = (int)tor_parse_long(c->value, 10, 0, 1, &ok, NULL);
    if (!ok) {
      tor_asprintf(msg,
          "Boolean '%s %s' expects 0 or 1.",
          c->key, c->value);
      return -1;
    }
    *(int *)lvalue = i;
    break;

  case CONFIG_TYPE_AUTOBOOL:
    if (!strcmp(c->value, "auto"))
      *(int *)lvalue = -1;
    else if (!strcmp(c->value, "0"))
      *(int *)lvalue = 0;
    else if (!strcmp(c->value, "1"))
      *(int *)lvalue = 1;
    else {
      tor_asprintf(msg, "Boolean '%s %s' expects 0, 1, or 'auto'.",
                   c->key, c->value);
      return -1;
    }
    break;

  case CONFIG_TYPE_STRING:
  case CONFIG_TYPE_FILENAME:
    tor_free(*(char **)lvalue);
    *(char **)lvalue = tor_strdup(c->value);
    break;

  case CONFIG_TYPE_DOUBLE:
    *(double *)lvalue = atof(c->value);
    break;

  case CONFIG_TYPE_ISOTIME:
    if (parse_iso_time(c->value, (time_t *)lvalue)) {
      tor_asprintf(msg,
          "Invalid time '%s' for keyword '%s'", c->value, c->key);
      return -1;
    }
    break;

  case CONFIG_TYPE_ROUTERSET:
    if (*(routerset_t**)lvalue) {
      routerset_free(*(routerset_t**)lvalue);
    }
    *(routerset_t**)lvalue = routerset_new();
    if (routerset_parse(*(routerset_t**)lvalue, c->value, c->key)<0) {
      tor_asprintf(msg, "Invalid exit list '%s' for option '%s'",
                   c->value, c->key);
      return -1;
    }
    break;

  case CONFIG_TYPE_CSV:
    if (*(smartlist_t**)lvalue) {
      SMARTLIST_FOREACH(*(smartlist_t**)lvalue, char *, cp, tor_free(cp));
      smartlist_clear(*(smartlist_t**)lvalue);
    } else {
      *(smartlist_t**)lvalue = smartlist_create();
    }

    smartlist_split_string(*(smartlist_t**)lvalue, c->value, ",",
                           SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
    break;

  case CONFIG_TYPE_LINELIST:
  case CONFIG_TYPE_LINELIST_S:
    {
      config_line_t *lastval = *(config_line_t**)lvalue;
      if (lastval && lastval->fragile) {
        if (c->command != CONFIG_LINE_APPEND) {
          config_free_lines(lastval);
          *(config_line_t**)lvalue = NULL;
        } else {
          lastval->fragile = 0;
        }
      }

      config_line_append((config_line_t**)lvalue, c->key, c->value);
    }
    break;
  case CONFIG_TYPE_OBSOLETE:
    log_warn(LD_CONFIG, "Skipping obsolete configuration option '%s'", c->key);
    break;
  case CONFIG_TYPE_LINELIST_V:
    tor_asprintf(msg,
        "You may not provide a value for virtual option '%s'", c->key);
    return -1;
  default:
    tor_assert(0);
    break;
  }
  return 0;
}

/** Mark every linelist in <b>options<b> "fragile", so that fresh assignments
 * to it will replace old ones. */
static void
config_mark_lists_fragile(const config_format_t *fmt, or_options_t *options)
{
  int i;
  tor_assert(fmt);
  tor_assert(options);

  for (i = 0; fmt->vars[i].name; ++i) {
    const config_var_t *var = &fmt->vars[i];
    config_line_t *list;
    if (var->type != CONFIG_TYPE_LINELIST &&
        var->type != CONFIG_TYPE_LINELIST_V)
      continue;

    list = *(config_line_t **)STRUCT_VAR_P(options, var->var_offset);
    if (list)
      list->fragile = 1;
  }
}

/** If <b>c</b> is a syntactically valid configuration line, update
 * <b>options</b> with its value and return 0.  Otherwise return -1 for bad
 * key, -2 for bad value.
 *
 * If <b>clear_first</b> is set, clear the value first. Then if
 * <b>use_defaults</b> is set, set the value to the default.
 *
 * Called from config_assign().
 */
static int
config_assign_line(const config_format_t *fmt, or_options_t *options,
                   config_line_t *c, int use_defaults,
                   int clear_first, bitarray_t *options_seen, char **msg)
{
  const config_var_t *var;

  CHECK(fmt, options);

  var = config_find_option(fmt, c->key);
  if (!var) {
    if (fmt->extra) {
      void *lvalue = STRUCT_VAR_P(options, fmt->extra->var_offset);
      log_info(LD_CONFIG,
               "Found unrecognized option '%s'; saving it.", c->key);
      config_line_append((config_line_t**)lvalue, c->key, c->value);
      return 0;
    } else {
      tor_asprintf(msg,
                "Unknown option '%s'.  Failing.", c->key);
      return -1;
    }
  }

  /* Put keyword into canonical case. */
  if (strcmp(var->name, c->key)) {
    tor_free(c->key);
    c->key = tor_strdup(var->name);
  }

  if (!strlen(c->value)) {
    /* reset or clear it, then return */
    if (!clear_first) {
      if ((var->type == CONFIG_TYPE_LINELIST ||
           var->type == CONFIG_TYPE_LINELIST_S) &&
          c->command != CONFIG_LINE_CLEAR) {
        /* We got an empty linelist from the torrc or command line.
           As a special case, call this an error. Warn and ignore. */
        log_warn(LD_CONFIG,
                 "Linelist option '%s' has no value. Skipping.", c->key);
      } else { /* not already cleared */
        option_reset(fmt, options, var, use_defaults);
      }
    }
    return 0;
  } else if (c->command == CONFIG_LINE_CLEAR && !clear_first) {
    option_reset(fmt, options, var, use_defaults);
  }

  if (options_seen && (var->type != CONFIG_TYPE_LINELIST &&
                       var->type != CONFIG_TYPE_LINELIST_S)) {
    /* We're tracking which options we've seen, and this option is not
     * supposed to occur more than once. */
    int var_index = (int)(var - fmt->vars);
    if (bitarray_is_set(options_seen, var_index)) {
      log_warn(LD_CONFIG, "Option '%s' used more than once; all but the last "
               "value will be ignored.", var->name);
    }
    bitarray_set(options_seen, var_index);
  }

  if (config_assign_value(fmt, options, c, msg) < 0)
    return -2;
  return 0;
}

/** Restore the option named <b>key</b> in options to its default value.
 * Called from config_assign(). */
static void
config_reset_line(const config_format_t *fmt, or_options_t *options,
                  const char *key, int use_defaults)
{
  const config_var_t *var;

  CHECK(fmt, options);

  var = config_find_option(fmt, key);
  if (!var)
    return; /* give error on next pass. */

  option_reset(fmt, options, var, use_defaults);
}

/** Return true iff key is a valid configuration option. */
int
option_is_recognized(const char *key)
{
  const config_var_t *var = config_find_option(&options_format, key);
  return (var != NULL);
}

/** Return the canonical name of a configuration option, or NULL
 * if no such option exists. */
const char *
option_get_canonical_name(const char *key)
{
  const config_var_t *var = config_find_option(&options_format, key);
  return var ? var->name : NULL;
}

/** Return a canonical list of the options assigned for key.
 */
config_line_t *
option_get_assignment(const or_options_t *options, const char *key)
{
  return get_assigned_option(&options_format, options, key, 1);
}

/** Return true iff value needs to be quoted and escaped to be used in
 * a configuration file. */
static int
config_value_needs_escape(const char *value)
{
  if (*value == '\"')
    return 1;
  while (*value) {
    switch (*value)
    {
    case '\r':
    case '\n':
    case '#':
      /* Note: quotes and backspaces need special handling when we are using
       * quotes, not otherwise, so they don't trigger escaping on their
       * own. */
      return 1;
    default:
      if (!TOR_ISPRINT(*value))
        return 1;
    }
    ++value;
  }
  return 0;
}

/** Return a newly allocated deep copy of the lines in <b>inp</b>. */
static config_line_t *
config_lines_dup(const config_line_t *inp)
{
  config_line_t *result = NULL;
  config_line_t **next_out = &result;
  while (inp) {
    *next_out = tor_malloc_zero(sizeof(config_line_t));
    (*next_out)->key = tor_strdup(inp->key);
    (*next_out)->value = tor_strdup(inp->value);
    inp = inp->next;
    next_out = &((*next_out)->next);
  }
  (*next_out) = NULL;
  return result;
}

/** Return newly allocated line or lines corresponding to <b>key</b> in the
 * configuration <b>options</b>.  If <b>escape_val</b> is true and a
 * value needs to be quoted before it's put in a config file, quote and
 * escape that value. Return NULL if no such key exists. */
static config_line_t *
get_assigned_option(const config_format_t *fmt, const void *options,
                    const char *key, int escape_val)
{
  const config_var_t *var;
  const void *value;
  config_line_t *result;
  tor_assert(options && key);

  CHECK(fmt, options);

  var = config_find_option(fmt, key);
  if (!var) {
    log_warn(LD_CONFIG, "Unknown option '%s'.  Failing.", key);
    return NULL;
  }
  value = STRUCT_VAR_P(options, var->var_offset);

  result = tor_malloc_zero(sizeof(config_line_t));
  result->key = tor_strdup(var->name);
  switch (var->type)
    {
    case CONFIG_TYPE_STRING:
    case CONFIG_TYPE_FILENAME:
      if (*(char**)value) {
        result->value = tor_strdup(*(char**)value);
      } else {
        tor_free(result->key);
        tor_free(result);
        return NULL;
      }
      break;
    case CONFIG_TYPE_ISOTIME:
      if (*(time_t*)value) {
        result->value = tor_malloc(ISO_TIME_LEN+1);
        format_iso_time(result->value, *(time_t*)value);
      } else {
        tor_free(result->key);
        tor_free(result);
      }
      escape_val = 0; /* Can't need escape. */
      break;
    case CONFIG_TYPE_PORT:
      if (*(int*)value == CFG_AUTO_PORT) {
        result->value = tor_strdup("auto");
        escape_val = 0;
        break;
      }
      /* fall through */
    case CONFIG_TYPE_INTERVAL:
    case CONFIG_TYPE_MSEC_INTERVAL:
    case CONFIG_TYPE_UINT:
      /* This means every or_options_t uint or bool element
       * needs to be an int. Not, say, a uint16_t or char. */
      tor_asprintf(&result->value, "%d", *(int*)value);
      escape_val = 0; /* Can't need escape. */
      break;
    case CONFIG_TYPE_MEMUNIT:
      tor_asprintf(&result->value, U64_FORMAT,
                   U64_PRINTF_ARG(*(uint64_t*)value));
      escape_val = 0; /* Can't need escape. */
      break;
    case CONFIG_TYPE_DOUBLE:
      tor_asprintf(&result->value, "%f", *(double*)value);
      escape_val = 0; /* Can't need escape. */
      break;

    case CONFIG_TYPE_AUTOBOOL:
      if (*(int*)value == -1) {
        result->value = tor_strdup("auto");
        escape_val = 0;
        break;
      }
      /* fall through */
    case CONFIG_TYPE_BOOL:
      result->value = tor_strdup(*(int*)value ? "1" : "0");
      escape_val = 0; /* Can't need escape. */
      break;
    case CONFIG_TYPE_ROUTERSET:
      result->value = routerset_to_string(*(routerset_t**)value);
      break;
    case CONFIG_TYPE_CSV:
      if (*(smartlist_t**)value)
        result->value =
          smartlist_join_strings(*(smartlist_t**)value, ",", 0, NULL);
      else
        result->value = tor_strdup("");
      break;
    case CONFIG_TYPE_OBSOLETE:
      log_fn(LOG_PROTOCOL_WARN, LD_CONFIG,
             "You asked me for the value of an obsolete config option '%s'.",
             key);
      tor_free(result->key);
      tor_free(result);
      return NULL;
    case CONFIG_TYPE_LINELIST_S:
      log_warn(LD_CONFIG,
               "Can't return context-sensitive '%s' on its own", key);
      tor_free(result->key);
      tor_free(result);
      return NULL;
    case CONFIG_TYPE_LINELIST:
    case CONFIG_TYPE_LINELIST_V:
      tor_free(result->key);
      tor_free(result);
      result = config_lines_dup(*(const config_line_t**)value);
      break;
    default:
      tor_free(result->key);
      tor_free(result);
      log_warn(LD_BUG,"Unknown type %d for known key '%s'",
               var->type, key);
      return NULL;
    }

  if (escape_val) {
    config_line_t *line;
    for (line = result; line; line = line->next) {
      if (line->value && config_value_needs_escape(line->value)) {
        char *newval = esc_for_log(line->value);
        tor_free(line->value);
        line->value = newval;
      }
    }
  }

  return result;
}

/** Iterate through the linked list of requested options <b>list</b>.
 * For each item, convert as appropriate and assign to <b>options</b>.
 * If an item is unrecognized, set *msg and return -1 immediately,
 * else return 0 for success.
 *
 * If <b>clear_first</b>, interpret config options as replacing (not
 * extending) their previous values. If <b>clear_first</b> is set,
 * then <b>use_defaults</b> to decide if you set to defaults after
 * clearing, or make the value 0 or NULL.
 *
 * Here are the use cases:
 * 1. A non-empty AllowInvalid line in your torrc. Appends to current
 *    if linelist, replaces current if csv.
 * 2. An empty AllowInvalid line in your torrc. Should clear it.
 * 3. "RESETCONF AllowInvalid" sets it to default.
 * 4. "SETCONF AllowInvalid" makes it NULL.
 * 5. "SETCONF AllowInvalid=foo" clears it and sets it to "foo".
 *
 * Use_defaults   Clear_first
 *    0                0       "append"
 *    1                0       undefined, don't use
 *    0                1       "set to null first"
 *    1                1       "set to defaults first"
 * Return 0 on success, -1 on bad key, -2 on bad value.
 *
 * As an additional special case, if a LINELIST config option has
 * no value and clear_first is 0, then warn and ignore it.
 */

/*
There are three call cases for config_assign() currently.

Case one: Torrc entry
options_init_from_torrc() calls config_assign(0, 0)
  calls config_assign_line(0, 0).
    if value is empty, calls option_reset(0) and returns.
    calls config_assign_value(), appends.

Case two: setconf
options_trial_assign() calls config_assign(0, 1)
  calls config_reset_line(0)
    calls option_reset(0)
      calls option_clear().
  calls config_assign_line(0, 1).
    if value is empty, returns.
    calls config_assign_value(), appends.

Case three: resetconf
options_trial_assign() calls config_assign(1, 1)
  calls config_reset_line(1)
    calls option_reset(1)
      calls option_clear().
      calls config_assign_value(default)
  calls config_assign_line(1, 1).
    returns.
*/
static int
config_assign(const config_format_t *fmt, void *options, config_line_t *list,
              int use_defaults, int clear_first, char **msg)
{
  config_line_t *p;
  bitarray_t *options_seen;
  const int n_options = config_count_options(fmt);

  CHECK(fmt, options);

  /* pass 1: normalize keys */
  for (p = list; p; p = p->next) {
    const char *full = expand_abbrev(fmt, p->key, 0, 1);
    if (strcmp(full,p->key)) {
      tor_free(p->key);
      p->key = tor_strdup(full);
    }
  }

  /* pass 2: if we're reading from a resetting source, clear all
   * mentioned config options, and maybe set to their defaults. */
  if (clear_first) {
    for (p = list; p; p = p->next)
      config_reset_line(fmt, options, p->key, use_defaults);
  }

  options_seen = bitarray_init_zero(n_options);
  /* pass 3: assign. */
  while (list) {
    int r;
    if ((r=config_assign_line(fmt, options, list, use_defaults,
                              clear_first, options_seen, msg))) {
      bitarray_free(options_seen);
      return r;
    }
    list = list->next;
  }
  bitarray_free(options_seen);

  /** Now we're done assigning a group of options to the configuration.
   * Subsequent group assignments should _replace_ linelists, not extend
   * them. */
  config_mark_lists_fragile(fmt, options);

  return 0;
}

/** Try assigning <b>list</b> to the global options. You do this by duping
 * options, assigning list to the new one, then validating it. If it's
 * ok, then throw out the old one and stick with the new one. Else,
 * revert to old and return failure.  Return SETOPT_OK on success, or
 * a setopt_err_t on failure.
 *
 * If not success, point *<b>msg</b> to a newly allocated string describing
 * what went wrong.
 */
setopt_err_t
options_trial_assign(config_line_t *list, int use_defaults,
                     int clear_first, char **msg)
{
  int r;
  or_options_t *trial_options = options_dup(&options_format, get_options());

  if ((r=config_assign(&options_format, trial_options,
                       list, use_defaults, clear_first, msg)) < 0) {
    config_free(&options_format, trial_options);
    return r;
  }

  if (options_validate(get_options_mutable(), trial_options, 1, msg) < 0) {
    config_free(&options_format, trial_options);
    return SETOPT_ERR_PARSE; /*XXX make this a separate return value. */
  }

  if (options_transition_allowed(get_options(), trial_options, msg) < 0) {
    config_free(&options_format, trial_options);
    return SETOPT_ERR_TRANSITION;
  }

  if (set_options(trial_options, msg)<0) {
    config_free(&options_format, trial_options);
    return SETOPT_ERR_SETTING;
  }

  /* we liked it. put it in place. */
  return SETOPT_OK;
}

/** Reset config option <b>var</b> to 0, 0.0, NULL, or the equivalent.
 * Called from option_reset() and config_free(). */
static void
option_clear(const config_format_t *fmt, or_options_t *options,
             const config_var_t *var)
{
  void *lvalue = STRUCT_VAR_P(options, var->var_offset);
  (void)fmt; /* unused */
  switch (var->type) {
    case CONFIG_TYPE_STRING:
    case CONFIG_TYPE_FILENAME:
      tor_free(*(char**)lvalue);
      break;
    case CONFIG_TYPE_DOUBLE:
      *(double*)lvalue = 0.0;
      break;
    case CONFIG_TYPE_ISOTIME:
      *(time_t*)lvalue = 0;
      break;
    case CONFIG_TYPE_INTERVAL:
    case CONFIG_TYPE_MSEC_INTERVAL:
    case CONFIG_TYPE_UINT:
    case CONFIG_TYPE_PORT:
    case CONFIG_TYPE_BOOL:
      *(int*)lvalue = 0;
      break;
    case CONFIG_TYPE_AUTOBOOL:
      *(int*)lvalue = -1;
      break;
    case CONFIG_TYPE_MEMUNIT:
      *(uint64_t*)lvalue = 0;
      break;
    case CONFIG_TYPE_ROUTERSET:
      if (*(routerset_t**)lvalue) {
        routerset_free(*(routerset_t**)lvalue);
        *(routerset_t**)lvalue = NULL;
      }
      break;
    case CONFIG_TYPE_CSV:
      if (*(smartlist_t**)lvalue) {
        SMARTLIST_FOREACH(*(smartlist_t **)lvalue, char *, cp, tor_free(cp));
        smartlist_free(*(smartlist_t **)lvalue);
        *(smartlist_t **)lvalue = NULL;
      }
      break;
    case CONFIG_TYPE_LINELIST:
    case CONFIG_TYPE_LINELIST_S:
      config_free_lines(*(config_line_t **)lvalue);
      *(config_line_t **)lvalue = NULL;
      break;
    case CONFIG_TYPE_LINELIST_V:
      /* handled by linelist_s. */
      break;
    case CONFIG_TYPE_OBSOLETE:
      break;
  }
}

/** Clear the option indexed by <b>var</b> in <b>options</b>. Then if
 * <b>use_defaults</b>, set it to its default value.
 * Called by config_init() and option_reset_line() and option_assign_line(). */
static void
option_reset(const config_format_t *fmt, or_options_t *options,
             const config_var_t *var, int use_defaults)
{
  config_line_t *c;
  char *msg = NULL;
  CHECK(fmt, options);
  option_clear(fmt, options, var); /* clear it first */
  if (!use_defaults)
    return; /* all done */
  if (var->initvalue) {
    c = tor_malloc_zero(sizeof(config_line_t));
    c->key = tor_strdup(var->name);
    c->value = tor_strdup(var->initvalue);
    if (config_assign_value(fmt, options, c, &msg) < 0) {
      log_warn(LD_BUG, "Failed to assign default: %s", msg);
      tor_free(msg); /* if this happens it's a bug */
    }
    config_free_lines(c);
  }
}

/** Print a usage message for tor. */
static void
print_usage(void)
{
  printf(
"Copyright (c) 2001-2004, Roger Dingledine\n"
"Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson\n"
"Copyright (c) 2007-2011, The Tor Project, Inc.\n\n"
"tor -f <torrc> [args]\n"
"See man page for options, or https://www.torproject.org/ for "
"documentation.\n");
}

/** Print all non-obsolete torrc options. */
static void
list_torrc_options(void)
{
  int i;
  smartlist_t *lines = smartlist_create();
  for (i = 0; _option_vars[i].name; ++i) {
    const config_var_t *var = &_option_vars[i];
    if (var->type == CONFIG_TYPE_OBSOLETE ||
        var->type == CONFIG_TYPE_LINELIST_V)
      continue;
    printf("%s\n", var->name);
  }
  smartlist_free(lines);
}

/** Last value actually set by resolve_my_address. */
static uint32_t last_resolved_addr = 0;
/**
 * Based on <b>options-\>Address</b>, guess our public IP address and put it
 * (in host order) into *<b>addr_out</b>. If <b>hostname_out</b> is provided,
 * set *<b>hostname_out</b> to a new string holding the hostname we used to
 * get the address. Return 0 if all is well, or -1 if we can't find a suitable
 * public IP address.
 */
int
resolve_my_address(int warn_severity, const or_options_t *options,
                   uint32_t *addr_out, char **hostname_out)
{
  struct in_addr in;
  uint32_t addr; /* host order */
  char hostname[256];
  int explicit_ip=1;
  int explicit_hostname=1;
  int from_interface=0;
  char *addr_string = NULL;
  const char *address = options->Address;
  int notice_severity = warn_severity <= LOG_NOTICE ?
                          LOG_NOTICE : warn_severity;

  tor_assert(addr_out);

  if (address && *address) {
    strlcpy(hostname, address, sizeof(hostname));
  } else { /* then we need to guess our address */
    explicit_ip = 0; /* it's implicit */
    explicit_hostname = 0; /* it's implicit */

    if (gethostname(hostname, sizeof(hostname)) < 0) {
      log_fn(warn_severity, LD_NET,"Error obtaining local hostname");
      return -1;
    }
    log_debug(LD_CONFIG,"Guessed local host name as '%s'",hostname);
  }

  /* now we know hostname. resolve it and keep only the IP address */

  if (tor_inet_aton(hostname, &in) == 0) {
    /* then we have to resolve it */
    explicit_ip = 0;
    if (tor_lookup_hostname(hostname, &addr)) { /* failed to resolve */
      uint32_t interface_ip; /* host order */

      if (explicit_hostname) {
        log_fn(warn_severity, LD_CONFIG,
               "Could not resolve local Address '%s'. Failing.", hostname);
        return -1;
      }
      log_fn(notice_severity, LD_CONFIG,
             "Could not resolve guessed local hostname '%s'. "
             "Trying something else.", hostname);
      if (get_interface_address(warn_severity, &interface_ip)) {
        log_fn(warn_severity, LD_CONFIG,
               "Could not get local interface IP address. Failing.");
        return -1;
      }
      from_interface = 1;
      addr = interface_ip;
      log_fn(notice_severity, LD_CONFIG, "Learned IP address '%s' for "
             "local interface. Using that.", fmt_addr32(addr));
      strlcpy(hostname, "<guessed from interfaces>", sizeof(hostname));
    } else { /* resolved hostname into addr */
      if (!explicit_hostname &&
          is_internal_IP(addr, 0)) {
        uint32_t interface_ip;

        log_fn(notice_severity, LD_CONFIG, "Guessed local hostname '%s' "
               "resolves to a private IP address (%s). Trying something "
               "else.", hostname, fmt_addr32(addr));

        if (get_interface_address(warn_severity, &interface_ip)) {
          log_fn(warn_severity, LD_CONFIG,
                 "Could not get local interface IP address. Too bad.");
        } else if (is_internal_IP(interface_ip, 0)) {
          log_fn(notice_severity, LD_CONFIG,
                 "Interface IP address '%s' is a private address too. "
                 "Ignoring.", fmt_addr32(interface_ip));
        } else {
          from_interface = 1;
          addr = interface_ip;
          log_fn(notice_severity, LD_CONFIG,
                 "Learned IP address '%s' for local interface."
                 " Using that.", fmt_addr32(addr));
          strlcpy(hostname, "<guessed from interfaces>", sizeof(hostname));
        }
      }
    }
  } else {
    addr = ntohl(in.s_addr); /* set addr so that addr_string is not
                              * illformed */
  }

  addr_string = tor_dup_ip(addr);
  if (is_internal_IP(addr, 0)) {
    /* make sure we're ok with publishing an internal IP */
    if (!options->DirServers && !options->AlternateDirAuthority) {
      /* if they are using the default dirservers, disallow internal IPs
       * always. */
      log_fn(warn_severity, LD_CONFIG,
             "Address '%s' resolves to private IP address '%s'. "
             "Tor servers that use the default DirServers must have public "
             "IP addresses.", hostname, addr_string);
      tor_free(addr_string);
      return -1;
    }
    if (!explicit_ip) {
      /* even if they've set their own dirservers, require an explicit IP if
       * they're using an internal address. */
      log_fn(warn_severity, LD_CONFIG, "Address '%s' resolves to private "
             "IP address '%s'. Please set the Address config option to be "
             "the IP address you want to use.", hostname, addr_string);
      tor_free(addr_string);
      return -1;
    }
  }

  log_debug(LD_CONFIG, "Resolved Address to '%s'.", fmt_addr32(addr));
  *addr_out = addr;
  if (last_resolved_addr && last_resolved_addr != *addr_out) {
    /* Leave this as a notice, regardless of the requested severity,
     * at least until dynamic IP address support becomes bulletproof. */
    log_notice(LD_NET,
               "Your IP address seems to have changed to %s. Updating.",
               addr_string);
    ip_address_changed(0);
  }
  if (last_resolved_addr != *addr_out) {
    const char *method;
    const char *h = hostname;
    if (explicit_ip) {
      method = "CONFIGURED";
      h = NULL;
    } else if (explicit_hostname) {
      method = "RESOLVED";
    } else if (from_interface) {
      method = "INTERFACE";
      h = NULL;
    } else {
      method = "GETHOSTNAME";
    }
    control_event_server_status(LOG_NOTICE,
                                "EXTERNAL_ADDRESS ADDRESS=%s METHOD=%s %s%s",
                                addr_string, method, h?"HOSTNAME=":"", h);
  }
  last_resolved_addr = *addr_out;
  if (hostname_out)
    *hostname_out = tor_strdup(hostname);
  tor_free(addr_string);
  return 0;
}

/** Return true iff <b>addr</b> is judged to be on the same network as us, or
 * on a private network.
 */
int
is_local_addr(const tor_addr_t *addr)
{
  if (tor_addr_is_internal(addr, 0))
    return 1;
  /* Check whether ip is on the same /24 as we are. */
  if (get_options()->EnforceDistinctSubnets == 0)
    return 0;
  if (tor_addr_family(addr) == AF_INET) {
    /*XXXX023 IP6 what corresponds to an /24? */
    uint32_t ip = tor_addr_to_ipv4h(addr);

    /* It's possible that this next check will hit before the first time
     * resolve_my_address actually succeeds.  (For clients, it is likely that
     * resolve_my_address will never be called at all).  In those cases,
     * last_resolved_addr will be 0, and so checking to see whether ip is on
     * the same /24 as last_resolved_addr will be the same as checking whether
     * it was on net 0, which is already done by is_internal_IP.
     */
    if ((last_resolved_addr & (uint32_t)0xffffff00ul)
        == (ip & (uint32_t)0xffffff00ul))
      return 1;
  }
  return 0;
}

/** Release storage held by <b>options</b>. */
static void
config_free(const config_format_t *fmt, void *options)
{
  int i;

  if (!options)
    return;

  tor_assert(fmt);

  for (i=0; fmt->vars[i].name; ++i)
    option_clear(fmt, options, &(fmt->vars[i]));
  if (fmt->extra) {
    config_line_t **linep = STRUCT_VAR_P(options, fmt->extra->var_offset);
    config_free_lines(*linep);
    *linep = NULL;
  }
  tor_free(options);
}

/** Return true iff a and b contain identical keys and values in identical
 * order. */
static int
config_lines_eq(config_line_t *a, config_line_t *b)
{
  while (a && b) {
    if (strcasecmp(a->key, b->key) || strcmp(a->value, b->value))
      return 0;
    a = a->next;
    b = b->next;
  }
  if (a || b)
    return 0;
  return 1;
}

/** Return true iff the option <b>name</b> has the same value in <b>o1</b>
 * and <b>o2</b>.  Must not be called for LINELIST_S or OBSOLETE options.
 */
static int
option_is_same(const config_format_t *fmt,
               const or_options_t *o1, const or_options_t *o2,
               const char *name)
{
  config_line_t *c1, *c2;
  int r = 1;
  CHECK(fmt, o1);
  CHECK(fmt, o2);

  c1 = get_assigned_option(fmt, o1, name, 0);
  c2 = get_assigned_option(fmt, o2, name, 0);
  r = config_lines_eq(c1, c2);
  config_free_lines(c1);
  config_free_lines(c2);
  return r;
}

/** Copy storage held by <b>old</b> into a new or_options_t and return it. */
static or_options_t *
options_dup(const config_format_t *fmt, const or_options_t *old)
{
  or_options_t *newopts;
  int i;
  config_line_t *line;

  newopts = config_alloc(fmt);
  for (i=0; fmt->vars[i].name; ++i) {
    if (fmt->vars[i].type == CONFIG_TYPE_LINELIST_S)
      continue;
    if (fmt->vars[i].type == CONFIG_TYPE_OBSOLETE)
      continue;
    line = get_assigned_option(fmt, old, fmt->vars[i].name, 0);
    if (line) {
      char *msg = NULL;
      if (config_assign(fmt, newopts, line, 0, 0, &msg) < 0) {
        log_err(LD_BUG, "Config_get_assigned_option() generated "
                "something we couldn't config_assign(): %s", msg);
        tor_free(msg);
        tor_assert(0);
      }
    }
    config_free_lines(line);
  }
  return newopts;
}

/** Return a new empty or_options_t.  Used for testing. */
or_options_t *
options_new(void)
{
  return config_alloc(&options_format);
}

/** Set <b>options</b> to hold reasonable defaults for most options.
 * Each option defaults to zero. */
void
options_init(or_options_t *options)
{
  config_init(&options_format, options);
}

/** Set all vars in the configuration object <b>options</b> to their default
 * values. */
static void
config_init(const config_format_t *fmt, void *options)
{
  int i;
  const config_var_t *var;
  CHECK(fmt, options);

  for (i=0; fmt->vars[i].name; ++i) {
    var = &fmt->vars[i];
    if (!var->initvalue)
      continue; /* defaults to NULL or 0 */
    option_reset(fmt, options, var, 1);
  }
}

/** Allocate and return a new string holding the written-out values of the vars
 * in 'options'.  If 'minimal', do not write out any default-valued vars.
 * Else, if comment_defaults, write default values as comments.
 */
static char *
config_dump(const config_format_t *fmt, const void *default_options,
            const void *options, int minimal,
            int comment_defaults)
{
  smartlist_t *elements;
  const or_options_t *defaults = default_options;
  void *defaults_tmp = NULL;
  config_line_t *line, *assigned;
  char *result;
  int i;
  char *msg = NULL;

  if (defaults == NULL) {
    defaults = defaults_tmp = config_alloc(fmt);
    config_init(fmt, defaults_tmp);
  }

  /* XXX use a 1 here so we don't add a new log line while dumping */
  if (default_options == NULL) {
    if (fmt->validate_fn(NULL, defaults_tmp, 1, &msg) < 0) {
      log_err(LD_BUG, "Failed to validate default config.");
      tor_free(msg);
      tor_assert(0);
    }
  }

  elements = smartlist_create();
  for (i=0; fmt->vars[i].name; ++i) {
    int comment_option = 0;
    if (fmt->vars[i].type == CONFIG_TYPE_OBSOLETE ||
        fmt->vars[i].type == CONFIG_TYPE_LINELIST_S)
      continue;
    /* Don't save 'hidden' control variables. */
    if (!strcmpstart(fmt->vars[i].name, "__"))
      continue;
    if (minimal && option_is_same(fmt, options, defaults, fmt->vars[i].name))
      continue;
    else if (comment_defaults &&
             option_is_same(fmt, options, defaults, fmt->vars[i].name))
      comment_option = 1;

    line = assigned = get_assigned_option(fmt, options, fmt->vars[i].name, 1);

    for (; line; line = line->next) {
      char *tmp;
      tor_asprintf(&tmp, "%s%s %s\n",
                   comment_option ? "# " : "",
                   line->key, line->value);
      smartlist_add(elements, tmp);
    }
    config_free_lines(assigned);
  }

  if (fmt->extra) {
    line = *(config_line_t**)STRUCT_VAR_P(options, fmt->extra->var_offset);
    for (; line; line = line->next) {
      char *tmp;
      tor_asprintf(&tmp, "%s %s\n", line->key, line->value);
      smartlist_add(elements, tmp);
    }
  }

  result = smartlist_join_strings(elements, "", 0, NULL);
  SMARTLIST_FOREACH(elements, char *, cp, tor_free(cp));
  smartlist_free(elements);
  if (defaults_tmp)
    config_free(fmt, defaults_tmp);
  return result;
}

/** Return a string containing a possible configuration file that would give
 * the configuration in <b>options</b>.  If <b>minimal</b> is true, do not
 * include options that are the same as Tor's defaults.
 */
char *
options_dump(const or_options_t *options, int minimal)
{
  return config_dump(&options_format, global_default_options,
                     options, minimal, 0);
}

/** Return 0 if every element of sl is a string holding a decimal
 * representation of a port number, or if sl is NULL.
 * Otherwise set *msg and return -1. */
static int
validate_ports_csv(smartlist_t *sl, const char *name, char **msg)
{
  int i;
  tor_assert(name);

  if (!sl)
    return 0;

  SMARTLIST_FOREACH(sl, const char *, cp,
  {
    i = atoi(cp);
    if (i < 1 || i > 65535) {
      tor_asprintf(msg, "Port '%s' out of range in %s", cp, name);
      return -1;
    }
  });
  return 0;
}

/** If <b>value</b> exceeds ROUTER_MAX_DECLARED_BANDWIDTH, write
 * a complaint into *<b>msg</b> using string <b>desc</b>, and return -1.
 * Else return 0.
 */
static int
ensure_bandwidth_cap(uint64_t *value, const char *desc, char **msg)
{
  if (*value > ROUTER_MAX_DECLARED_BANDWIDTH) {
    /* This handles an understandable special case where somebody says "2gb"
     * whereas our actual maximum is 2gb-1 (INT_MAX) */
    --*value;
  }
  if (*value > ROUTER_MAX_DECLARED_BANDWIDTH) {
    tor_asprintf(msg, "%s ("U64_FORMAT") must be at most %d",
                 desc, U64_PRINTF_ARG(*value),
                 ROUTER_MAX_DECLARED_BANDWIDTH);
    return -1;
  }
  return 0;
}

/** Parse an authority type from <b>options</b>-\>PublishServerDescriptor
 * and write it to <b>options</b>-\>_PublishServerDescriptor. Treat "1"
 * as "v2,v3" unless BridgeRelay is 1, in which case treat it as "bridge".
 * Treat "0" as "".
 * Return 0 on success or -1 if not a recognized authority type (in which
 * case the value of _PublishServerDescriptor is undefined). */
static int
compute_publishserverdescriptor(or_options_t *options)
{
  smartlist_t *list = options->PublishServerDescriptor;
  dirinfo_type_t *auth = &options->_PublishServerDescriptor;
  *auth = NO_DIRINFO;
  if (!list) /* empty list, answer is none */
    return 0;
  SMARTLIST_FOREACH(list, const char *, string, {
    if (!strcasecmp(string, "v1"))
      *auth |= V1_DIRINFO;
    else if (!strcmp(string, "1"))
      if (options->BridgeRelay)
        *auth |= BRIDGE_DIRINFO;
      else
        *auth |= V2_DIRINFO | V3_DIRINFO;
    else if (!strcasecmp(string, "v2"))
      *auth |= V2_DIRINFO;
    else if (!strcasecmp(string, "v3"))
      *auth |= V3_DIRINFO;
    else if (!strcasecmp(string, "bridge"))
      *auth |= BRIDGE_DIRINFO;
    else if (!strcasecmp(string, "hidserv"))
      log_warn(LD_CONFIG,
               "PublishServerDescriptor hidserv is invalid. See "
               "PublishHidServDescriptors.");
    else if (!strcasecmp(string, "") || !strcmp(string, "0"))
      /* no authority */;
    else
      return -1;
    });
  return 0;
}

/** Lowest allowable value for RendPostPeriod; if this is too low, hidden
 * services can overload the directory system. */
#define MIN_REND_POST_PERIOD (10*60)

/** Highest allowable value for RendPostPeriod. */
#define MAX_DIR_PERIOD (MIN_ONION_KEY_LIFETIME/2)

/** Lowest allowable value for MaxCircuitDirtiness; if this is too low, Tor
 * will generate too many circuits and potentially overload the network. */
#define MIN_MAX_CIRCUIT_DIRTINESS 10

/** Lowest allowable value for CircuitStreamTimeout; if this is too low, Tor
 * will generate too many circuits and potentially overload the network. */
#define MIN_CIRCUIT_STREAM_TIMEOUT 10

/** Lowest allowable value for HeartbeatPeriod; if this is too low, we might
 * expose more information than we're comfortable with. */
#define MIN_HEARTBEAT_PERIOD (30*60)

/** Return 0 if every setting in <b>options</b> is reasonable, and a
 * permissible transition from <b>old_options</b>. Else return -1.
 * Should have no side effects, except for normalizing the contents of
 * <b>options</b>.
 *
 * On error, tor_strdup an error explanation into *<b>msg</b>.
 *
 * XXX
 * If <b>from_setconf</b>, we were called by the controller, and our
 * Log line should stay empty. If it's 0, then give us a default log
 * if there are no logs defined.
 */
static int
options_validate(or_options_t *old_options, or_options_t *options,
                 int from_setconf, char **msg)
{
  int i;
  config_line_t *cl;
  const char *uname = get_uname();
  int n_ports=0;
#define REJECT(arg) \
  STMT_BEGIN *msg = tor_strdup(arg); return -1; STMT_END
#define COMPLAIN(arg) STMT_BEGIN log(LOG_WARN, LD_CONFIG, arg); STMT_END

  tor_assert(msg);
  *msg = NULL;

  if (server_mode(options) &&
      (!strcmpstart(uname, "Windows 95") ||
       !strcmpstart(uname, "Windows 98") ||
       !strcmpstart(uname, "Windows Me"))) {
    log(LOG_WARN, LD_CONFIG, "Tor is running as a server, but you are "
        "running %s; this probably won't work. See "
        "https://wiki.torproject.org/TheOnionRouter/TorFAQ#ServerOS "
        "for details.", uname);
  }

  if (parse_ports(options, 1, msg, &n_ports) < 0)
    return -1;

  if (validate_data_directory(options)<0)
    REJECT("Invalid DataDirectory");

  if (options->Nickname == NULL) {
    if (server_mode(options)) {
        options->Nickname = tor_strdup(UNNAMED_ROUTER_NICKNAME);
    }
  } else {
    if (!is_legal_nickname(options->Nickname)) {
      tor_asprintf(msg,
          "Nickname '%s' is wrong length or contains illegal characters.",
          options->Nickname);
      return -1;
    }
  }

  if (server_mode(options) && !options->ContactInfo)
    log(LOG_NOTICE, LD_CONFIG, "Your ContactInfo config option is not set. "
        "Please consider setting it, so we can contact you if your server is "
        "misconfigured or something else goes wrong.");

  /* Special case on first boot if no Log options are given. */
  if (!options->Logs && !options->RunAsDaemon && !from_setconf) {
    if (quiet_level == 0)
        config_line_append(&options->Logs, "Log", "notice stdout");
    else if (quiet_level == 1)
        config_line_append(&options->Logs, "Log", "warn stdout");
  }

  if (options_init_logs(options, 1)<0) /* Validate the log(s) */
    REJECT("Failed to validate Log options. See logs for details.");

  if (authdir_mode(options)) {
    /* confirm that our address isn't broken, so we can complain now */
    uint32_t tmp;
    if (resolve_my_address(LOG_WARN, options, &tmp, NULL) < 0)
      REJECT("Failed to resolve/guess local address. See logs for details.");
  }

#ifndef MS_WINDOWS
  if (options->RunAsDaemon && torrc_fname && path_is_relative(torrc_fname))
    REJECT("Can't use a relative path to torrc when RunAsDaemon is set.");
#endif

  /* XXXX require that the only port not be DirPort? */
  /* XXXX require that at least one port be listened-upon. */
  if (n_ports == 0 && !options->RendConfigLines)
    log(LOG_WARN, LD_CONFIG,
        "SocksPort, TransPort, NATDPort, DNSPort, and ORPort are all "
        "undefined, and there aren't any hidden services configured.  "
        "Tor will still run, but probably won't do anything.");

#ifndef USE_TRANSPARENT
  if (options->TransPort || options->TransListenAddress)
    REJECT("TransPort and TransListenAddress are disabled in this build.");
#endif

  if (options->TokenBucketRefillInterval <= 0
      || options->TokenBucketRefillInterval > 1000) {
    REJECT("TokenBucketRefillInterval must be between 1 and 1000 inclusive.");
  }

  if (options->ExcludeExitNodes || options->ExcludeNodes) {
    options->_ExcludeExitNodesUnion = routerset_new();
    routerset_union(options->_ExcludeExitNodesUnion,options->ExcludeExitNodes);
    routerset_union(options->_ExcludeExitNodesUnion,options->ExcludeNodes);
  }

  if (options->NodeFamilies) {
    options->NodeFamilySets = smartlist_create();
    for (cl = options->NodeFamilies; cl; cl = cl->next) {
      routerset_t *rs = routerset_new();
      if (routerset_parse(rs, cl->value, cl->key) == 0) {
        smartlist_add(options->NodeFamilySets, rs);
      } else {
        routerset_free(rs);
      }
    }
  }

  if (options->ExcludeNodes && options->StrictNodes) {
    COMPLAIN("You have asked to exclude certain relays from all positions "
             "in your circuits. Expect hidden services and other Tor "
             "features to be broken in unpredictable ways.");
  }

  if (options->AuthoritativeDir) {
    if (!options->ContactInfo && !options->TestingTorNetwork)
      REJECT("Authoritative directory servers must set ContactInfo");
    if (options->V1AuthoritativeDir && !options->RecommendedVersions)
      REJECT("V1 authoritative dir servers must set RecommendedVersions.");
    if (!options->RecommendedClientVersions)
      options->RecommendedClientVersions =
        config_lines_dup(options->RecommendedVersions);
    if (!options->RecommendedServerVersions)
      options->RecommendedServerVersions =
        config_lines_dup(options->RecommendedVersions);
    if (options->VersioningAuthoritativeDir &&
        (!options->RecommendedClientVersions ||
         !options->RecommendedServerVersions))
      REJECT("Versioning authoritative dir servers must set "
             "Recommended*Versions.");
    if (options->UseEntryGuards) {
      log_info(LD_CONFIG, "Authoritative directory servers can't set "
               "UseEntryGuards. Disabling.");
      options->UseEntryGuards = 0;
    }
    if (!options->DownloadExtraInfo && authdir_mode_any_main(options)) {
      log_info(LD_CONFIG, "Authoritative directories always try to download "
               "extra-info documents. Setting DownloadExtraInfo.");
      options->DownloadExtraInfo = 1;
    }
    if (!(options->BridgeAuthoritativeDir || options->HSAuthoritativeDir ||
          options->V1AuthoritativeDir || options->V2AuthoritativeDir ||
          options->V3AuthoritativeDir))
      REJECT("AuthoritativeDir is set, but none of "
             "(Bridge/HS/V1/V2/V3)AuthoritativeDir is set.");
    /* If we have a v3bandwidthsfile and it's broken, complain on startup */
    if (options->V3BandwidthsFile && !old_options) {
      dirserv_read_measured_bandwidths(options->V3BandwidthsFile, NULL);
    }
  }

  if (options->AuthoritativeDir && !options->DirPort)
    REJECT("Running as authoritative directory, but no DirPort set.");

  if (options->AuthoritativeDir && !options->ORPort)
    REJECT("Running as authoritative directory, but no ORPort set.");

  if (options->AuthoritativeDir && options->ClientOnly)
    REJECT("Running as authoritative directory, but ClientOnly also set.");

  if (options->FetchDirInfoExtraEarly && !options->FetchDirInfoEarly)
    REJECT("FetchDirInfoExtraEarly requires that you also set "
           "FetchDirInfoEarly");

  if (options->HSAuthoritativeDir && proxy_mode(options))
    REJECT("Running as authoritative v0 HS directory, but also configured "
           "as a client.");

  if (options->ConnLimit <= 0) {
    tor_asprintf(msg,
        "ConnLimit must be greater than 0, but was set to %d",
        options->ConnLimit);
    return -1;
  }

  if (options->MaxClientCircuitsPending <= 0 ||
      options->MaxClientCircuitsPending > MAX_MAX_CLIENT_CIRCUITS_PENDING) {
    tor_asprintf(msg,
                 "MaxClientCircuitsPending must be between 1 and %d, but "
                 "was set to %d", MAX_MAX_CLIENT_CIRCUITS_PENDING,
                 options->MaxClientCircuitsPending);
    return -1;
  }

  if (validate_ports_csv(options->FirewallPorts, "FirewallPorts", msg) < 0)
    return -1;

  if (validate_ports_csv(options->LongLivedPorts, "LongLivedPorts", msg) < 0)
    return -1;

  if (validate_ports_csv(options->RejectPlaintextPorts,
                         "RejectPlaintextPorts", msg) < 0)
    return -1;

  if (validate_ports_csv(options->WarnPlaintextPorts,
                         "WarnPlaintextPorts", msg) < 0)
    return -1;

  if (options->FascistFirewall && !options->ReachableAddresses) {
    if (options->FirewallPorts && smartlist_len(options->FirewallPorts)) {
      /* We already have firewall ports set, so migrate them to
       * ReachableAddresses, which will set ReachableORAddresses and
       * ReachableDirAddresses if they aren't set explicitly. */
      smartlist_t *instead = smartlist_create();
      config_line_t *new_line = tor_malloc_zero(sizeof(config_line_t));
      new_line->key = tor_strdup("ReachableAddresses");
      /* If we're configured with the old format, we need to prepend some
       * open ports. */
      SMARTLIST_FOREACH(options->FirewallPorts, const char *, portno,
      {
        int p = atoi(portno);
        char *s;
        if (p<0) continue;
        s = tor_malloc(16);
        tor_snprintf(s, 16, "*:%d", p);
        smartlist_add(instead, s);
      });
      new_line->value = smartlist_join_strings(instead,",",0,NULL);
      /* These have been deprecated since 0.1.1.5-alpha-cvs */
      log(LOG_NOTICE, LD_CONFIG,
          "Converting FascistFirewall and FirewallPorts "
          "config options to new format: \"ReachableAddresses %s\"",
          new_line->value);
      options->ReachableAddresses = new_line;
      SMARTLIST_FOREACH(instead, char *, cp, tor_free(cp));
      smartlist_free(instead);
    } else {
      /* We do not have FirewallPorts set, so add 80 to
       * ReachableDirAddresses, and 443 to ReachableORAddresses. */
      if (!options->ReachableDirAddresses) {
        config_line_t *new_line = tor_malloc_zero(sizeof(config_line_t));
        new_line->key = tor_strdup("ReachableDirAddresses");
        new_line->value = tor_strdup("*:80");
        options->ReachableDirAddresses = new_line;
        log(LOG_NOTICE, LD_CONFIG, "Converting FascistFirewall config option "
            "to new format: \"ReachableDirAddresses *:80\"");
      }
      if (!options->ReachableORAddresses) {
        config_line_t *new_line = tor_malloc_zero(sizeof(config_line_t));
        new_line->key = tor_strdup("ReachableORAddresses");
        new_line->value = tor_strdup("*:443");
        options->ReachableORAddresses = new_line;
        log(LOG_NOTICE, LD_CONFIG, "Converting FascistFirewall config option "
            "to new format: \"ReachableORAddresses *:443\"");
      }
    }
  }

  for (i=0; i<3; i++) {
    config_line_t **linep =
      (i==0) ? &options->ReachableAddresses :
        (i==1) ? &options->ReachableORAddresses :
                 &options->ReachableDirAddresses;
    if (!*linep)
      continue;
    /* We need to end with a reject *:*, not an implicit accept *:* */
    for (;;) {
      if (!strcmp((*linep)->value, "reject *:*")) /* already there */
        break;
      linep = &((*linep)->next);
      if (!*linep) {
        *linep = tor_malloc_zero(sizeof(config_line_t));
        (*linep)->key = tor_strdup(
          (i==0) ?  "ReachableAddresses" :
            (i==1) ? "ReachableORAddresses" :
                     "ReachableDirAddresses");
        (*linep)->value = tor_strdup("reject *:*");
        break;
      }
    }
  }

  if ((options->ReachableAddresses ||
       options->ReachableORAddresses ||
       options->ReachableDirAddresses) &&
      server_mode(options))
    REJECT("Servers must be able to freely connect to the rest "
           "of the Internet, so they must not set Reachable*Addresses "
           "or FascistFirewall.");

  if (options->UseBridges &&
      server_mode(options))
    REJECT("Servers must be able to freely connect to the rest "
           "of the Internet, so they must not set UseBridges.");

  /* If both of these are set, we'll end up with funny behavior where we
   * demand enough entrynodes be up and running else we won't build
   * circuits, yet we never actually use them. */
  if (options->UseBridges && options->EntryNodes)
    REJECT("You cannot set both UseBridges and EntryNodes.");

  if (options->EntryNodes && !options->UseEntryGuards)
    log_warn(LD_CONFIG, "EntryNodes is set, but UseEntryGuards is disabled. "
             "EntryNodes will be ignored.");

  options->_AllowInvalid = 0;
  if (options->AllowInvalidNodes) {
    SMARTLIST_FOREACH(options->AllowInvalidNodes, const char *, cp, {
        if (!strcasecmp(cp, "entry"))
          options->_AllowInvalid |= ALLOW_INVALID_ENTRY;
        else if (!strcasecmp(cp, "exit"))
          options->_AllowInvalid |= ALLOW_INVALID_EXIT;
        else if (!strcasecmp(cp, "middle"))
          options->_AllowInvalid |= ALLOW_INVALID_MIDDLE;
        else if (!strcasecmp(cp, "introduction"))
          options->_AllowInvalid |= ALLOW_INVALID_INTRODUCTION;
        else if (!strcasecmp(cp, "rendezvous"))
          options->_AllowInvalid |= ALLOW_INVALID_RENDEZVOUS;
        else {
          tor_asprintf(msg,
              "Unrecognized value '%s' in AllowInvalidNodes", cp);
          return -1;
        }
      });
  }

  if (!options->SafeLogging ||
      !strcasecmp(options->SafeLogging, "0")) {
    options->_SafeLogging = SAFELOG_SCRUB_NONE;
  } else if (!strcasecmp(options->SafeLogging, "relay")) {
    options->_SafeLogging = SAFELOG_SCRUB_RELAY;
  } else if (!strcasecmp(options->SafeLogging, "1")) {
    options->_SafeLogging = SAFELOG_SCRUB_ALL;
  } else {
    tor_asprintf(msg,
                     "Unrecognized value '%s' in SafeLogging",
                     escaped(options->SafeLogging));
    return -1;
  }

  if (compute_publishserverdescriptor(options) < 0) {
    tor_asprintf(msg, "Unrecognized value in PublishServerDescriptor");
    return -1;
  }

  if ((options->BridgeRelay
        || options->_PublishServerDescriptor & BRIDGE_DIRINFO)
      && (options->_PublishServerDescriptor
          & (V1_DIRINFO|V2_DIRINFO|V3_DIRINFO))) {
    REJECT("Bridges are not supposed to publish router descriptors to the "
           "directory authorities. Please correct your "
           "PublishServerDescriptor line.");
  }

  if (options->BridgeRelay && options->DirPort) {
    log_warn(LD_CONFIG, "Can't set a DirPort on a bridge relay; disabling "
             "DirPort");
    config_free_lines(options->DirPort);
    options->DirPort = NULL;
  }

  if (options->MinUptimeHidServDirectoryV2 < 0) {
    log_warn(LD_CONFIG, "MinUptimeHidServDirectoryV2 option must be at "
                        "least 0 seconds. Changing to 0.");
    options->MinUptimeHidServDirectoryV2 = 0;
  }

  if (options->RendPostPeriod < MIN_REND_POST_PERIOD) {
    log_warn(LD_CONFIG, "RendPostPeriod option is too short; "
             "raising to %d seconds.", MIN_REND_POST_PERIOD);
    options->RendPostPeriod = MIN_REND_POST_PERIOD;
  }

  if (options->RendPostPeriod > MAX_DIR_PERIOD) {
    log_warn(LD_CONFIG, "RendPostPeriod is too large; clipping to %ds.",
             MAX_DIR_PERIOD);
    options->RendPostPeriod = MAX_DIR_PERIOD;
  }

  if (options->Tor2webMode && options->LearnCircuitBuildTimeout) {
    /* LearnCircuitBuildTimeout and Tor2webMode are incompatible in
     * two ways:
     *
     * - LearnCircuitBuildTimeout results in a low CBT, which
     *   Tor2webMode's use of one-hop rendezvous circuits lowers
     *   much further, producing *far* too many timeouts.
     *
     * - The adaptive CBT code does not update its timeout estimate
     *   using build times for single-hop circuits.
     *
     * If we fix both of these issues someday, we should test
     * Tor2webMode with LearnCircuitBuildTimeout on again. */
    log_notice(LD_CONFIG,"Tor2webMode is enabled; turning "
               "LearnCircuitBuildTimeout off.");
    options->LearnCircuitBuildTimeout = 0;
  }

  if (options->MaxCircuitDirtiness < MIN_MAX_CIRCUIT_DIRTINESS) {
    log_warn(LD_CONFIG, "MaxCircuitDirtiness option is too short; "
             "raising to %d seconds.", MIN_MAX_CIRCUIT_DIRTINESS);
    options->MaxCircuitDirtiness = MIN_MAX_CIRCUIT_DIRTINESS;
  }

  if (options->CircuitStreamTimeout &&
      options->CircuitStreamTimeout < MIN_CIRCUIT_STREAM_TIMEOUT) {
    log_warn(LD_CONFIG, "CircuitStreamTimeout option is too short; "
             "raising to %d seconds.", MIN_CIRCUIT_STREAM_TIMEOUT);
    options->CircuitStreamTimeout = MIN_CIRCUIT_STREAM_TIMEOUT;
  }

  if (options->HeartbeatPeriod &&
      options->HeartbeatPeriod < MIN_HEARTBEAT_PERIOD) {
    log_warn(LD_CONFIG, "HeartbeatPeriod option is too short; "
             "raising to %d seconds.", MIN_HEARTBEAT_PERIOD);
    options->HeartbeatPeriod = MIN_HEARTBEAT_PERIOD;
  }

  if (options->KeepalivePeriod < 1)
    REJECT("KeepalivePeriod option must be positive.");

  if (ensure_bandwidth_cap(&options->BandwidthRate,
                           "BandwidthRate", msg) < 0)
    return -1;
  if (ensure_bandwidth_cap(&options->BandwidthBurst,
                           "BandwidthBurst", msg) < 0)
    return -1;
  if (ensure_bandwidth_cap(&options->MaxAdvertisedBandwidth,
                           "MaxAdvertisedBandwidth", msg) < 0)
    return -1;
  if (ensure_bandwidth_cap(&options->RelayBandwidthRate,
                           "RelayBandwidthRate", msg) < 0)
    return -1;
  if (ensure_bandwidth_cap(&options->RelayBandwidthBurst,
                           "RelayBandwidthBurst", msg) < 0)
    return -1;
  if (ensure_bandwidth_cap(&options->PerConnBWRate,
                           "PerConnBWRate", msg) < 0)
    return -1;
  if (ensure_bandwidth_cap(&options->PerConnBWBurst,
                           "PerConnBWBurst", msg) < 0)
    return -1;
  if (ensure_bandwidth_cap(&options->AuthDirFastGuarantee,
                           "AuthDirFastGuarantee", msg) < 0)
    return -1;
  if (ensure_bandwidth_cap(&options->AuthDirGuardBWGuarantee,
                           "AuthDirGuardBWGuarantee", msg) < 0)
    return -1;

  if (options->RelayBandwidthRate && !options->RelayBandwidthBurst)
    options->RelayBandwidthBurst = options->RelayBandwidthRate;
  if (options->RelayBandwidthBurst && !options->RelayBandwidthRate)
    options->RelayBandwidthRate = options->RelayBandwidthBurst;

  if (server_mode(options)) {
    if (options->BandwidthRate < ROUTER_REQUIRED_MIN_BANDWIDTH) {
      tor_asprintf(msg,
                       "BandwidthRate is set to %d bytes/second. "
                       "For servers, it must be at least %d.",
                       (int)options->BandwidthRate,
                       ROUTER_REQUIRED_MIN_BANDWIDTH);
      return -1;
    } else if (options->MaxAdvertisedBandwidth <
               ROUTER_REQUIRED_MIN_BANDWIDTH/2) {
      tor_asprintf(msg,
                       "MaxAdvertisedBandwidth is set to %d bytes/second. "
                       "For servers, it must be at least %d.",
                       (int)options->MaxAdvertisedBandwidth,
                       ROUTER_REQUIRED_MIN_BANDWIDTH/2);
      return -1;
    }
    if (options->RelayBandwidthRate &&
      options->RelayBandwidthRate < ROUTER_REQUIRED_MIN_BANDWIDTH) {
      tor_asprintf(msg,
                       "RelayBandwidthRate is set to %d bytes/second. "
                       "For servers, it must be at least %d.",
                       (int)options->RelayBandwidthRate,
                       ROUTER_REQUIRED_MIN_BANDWIDTH);
      return -1;
    }
  }

  if (options->RelayBandwidthRate > options->RelayBandwidthBurst)
    REJECT("RelayBandwidthBurst must be at least equal "
           "to RelayBandwidthRate.");

  if (options->BandwidthRate > options->BandwidthBurst)
    REJECT("BandwidthBurst must be at least equal to BandwidthRate.");

  /* if they set relaybandwidth* really high but left bandwidth*
   * at the default, raise the defaults. */
  if (options->RelayBandwidthRate > options->BandwidthRate)
    options->BandwidthRate = options->RelayBandwidthRate;
  if (options->RelayBandwidthBurst > options->BandwidthBurst)
    options->BandwidthBurst = options->RelayBandwidthBurst;

  if (accounting_parse_options(options, 1)<0)
    REJECT("Failed to parse accounting options. See logs for details.");

  if (options->HTTPProxy) { /* parse it now */
    if (tor_addr_port_lookup(options->HTTPProxy,
                        &options->HTTPProxyAddr, &options->HTTPProxyPort) < 0)
      REJECT("HTTPProxy failed to parse or resolve. Please fix.");
    if (options->HTTPProxyPort == 0) { /* give it a default */
      options->HTTPProxyPort = 80;
    }
  }

  if (options->HTTPProxyAuthenticator) {
    if (strlen(options->HTTPProxyAuthenticator) >= 512)
      REJECT("HTTPProxyAuthenticator is too long (>= 512 chars).");
  }

  if (options->HTTPSProxy) { /* parse it now */
    if (tor_addr_port_lookup(options->HTTPSProxy,
                        &options->HTTPSProxyAddr, &options->HTTPSProxyPort) <0)
      REJECT("HTTPSProxy failed to parse or resolve. Please fix.");
    if (options->HTTPSProxyPort == 0) { /* give it a default */
      options->HTTPSProxyPort = 443;
    }
  }

  if (options->HTTPSProxyAuthenticator) {
    if (strlen(options->HTTPSProxyAuthenticator) >= 512)
      REJECT("HTTPSProxyAuthenticator is too long (>= 512 chars).");
  }

  if (options->Socks4Proxy) { /* parse it now */
    if (tor_addr_port_lookup(options->Socks4Proxy,
                        &options->Socks4ProxyAddr,
                        &options->Socks4ProxyPort) <0)
      REJECT("Socks4Proxy failed to parse or resolve. Please fix.");
    if (options->Socks4ProxyPort == 0) { /* give it a default */
      options->Socks4ProxyPort = 1080;
    }
  }

  if (options->Socks5Proxy) { /* parse it now */
    if (tor_addr_port_lookup(options->Socks5Proxy,
                            &options->Socks5ProxyAddr,
                            &options->Socks5ProxyPort) <0)
      REJECT("Socks5Proxy failed to parse or resolve. Please fix.");
    if (options->Socks5ProxyPort == 0) { /* give it a default */
      options->Socks5ProxyPort = 1080;
    }
  }

  /* Check if more than one proxy type has been enabled. */
  if (!!options->Socks4Proxy + !!options->Socks5Proxy +
      !!options->HTTPSProxy + !!options->ClientTransportPlugin > 1)
    REJECT("You have configured more than one proxy type. "
           "(Socks4Proxy|Socks5Proxy|HTTPSProxy|ClientTransportPlugin)");

  if (options->Socks5ProxyUsername) {
    size_t len;

    len = strlen(options->Socks5ProxyUsername);
    if (len < 1 || len > 255)
      REJECT("Socks5ProxyUsername must be between 1 and 255 characters.");

    if (!options->Socks5ProxyPassword)
      REJECT("Socks5ProxyPassword must be included with Socks5ProxyUsername.");

    len = strlen(options->Socks5ProxyPassword);
    if (len < 1 || len > 255)
      REJECT("Socks5ProxyPassword must be between 1 and 255 characters.");
  } else if (options->Socks5ProxyPassword)
    REJECT("Socks5ProxyPassword must be included with Socks5ProxyUsername.");

  if (options->HashedControlPassword) {
    smartlist_t *sl = decode_hashed_passwords(options->HashedControlPassword);
    if (!sl) {
      REJECT("Bad HashedControlPassword: wrong length or bad encoding");
    } else {
      SMARTLIST_FOREACH(sl, char*, cp, tor_free(cp));
      smartlist_free(sl);
    }
  }

  if (options->HashedControlSessionPassword) {
    smartlist_t *sl = decode_hashed_passwords(
                                  options->HashedControlSessionPassword);
    if (!sl) {
      REJECT("Bad HashedControlSessionPassword: wrong length or bad encoding");
    } else {
      SMARTLIST_FOREACH(sl, char*, cp, tor_free(cp));
      smartlist_free(sl);
    }
  }

  if (options->OwningControllerProcess) {
    const char *validate_pspec_msg = NULL;
    if (tor_validate_process_specifier(options->OwningControllerProcess,
                                       &validate_pspec_msg)) {
      tor_asprintf(msg, "Bad OwningControllerProcess: %s",
                   validate_pspec_msg);
      return -1;
    }
  }

  if (options->ControlPort && !options->HashedControlPassword &&
      !options->HashedControlSessionPassword &&
      !options->CookieAuthentication) {
    log_warn(LD_CONFIG, "ControlPort is open, but no authentication method "
             "has been configured.  This means that any program on your "
             "computer can reconfigure your Tor.  That's bad!  You should "
             "upgrade your Tor controller as soon as possible.");
  }

  if (options->CookieAuthFileGroupReadable && !options->CookieAuthFile) {
    log_warn(LD_CONFIG, "CookieAuthFileGroupReadable is set, but will have "
             "no effect: you must specify an explicit CookieAuthFile to "
             "have it group-readable.");
  }

  if (options->UseEntryGuards && ! options->NumEntryGuards)
    REJECT("Cannot enable UseEntryGuards with NumEntryGuards set to 0");

  if (check_nickname_list(options->MyFamily, "MyFamily", msg))
    return -1;
  for (cl = options->NodeFamilies; cl; cl = cl->next) {
    routerset_t *rs = routerset_new();
    if (routerset_parse(rs, cl->value, cl->key)) {
      routerset_free(rs);
      return -1;
    }
    routerset_free(rs);
  }

  if (validate_addr_policies(options, msg) < 0)
    return -1;

  if (validate_dir_authorities(options, old_options) < 0)
    REJECT("Directory authority line did not parse. See logs for details.");

  if (options->UseBridges && !options->Bridges)
    REJECT("If you set UseBridges, you must specify at least one bridge.");
  if (options->UseBridges && !options->TunnelDirConns)
    REJECT("If you set UseBridges, you must set TunnelDirConns.");

  for (cl = options->Bridges; cl; cl = cl->next) {
    if (parse_bridge_line(cl->value, 1)<0)
      REJECT("Bridge line did not parse. See logs for details.");
  }

  for (cl = options->ClientTransportPlugin; cl; cl = cl->next) {
    if (parse_client_transport_line(cl->value, 1)<0)
      REJECT("Transport line did not parse. See logs for details.");
  }

  for (cl = options->ServerTransportPlugin; cl; cl = cl->next) {
    if (parse_server_transport_line(cl->value, 1)<0)
      REJECT("Server transport line did not parse. See logs for details.");
  }

  if (options->ConstrainedSockets) {
    /* If the user wants to constrain socket buffer use, make sure the desired
     * limit is between MIN|MAX_TCPSOCK_BUFFER in k increments. */
    if (options->ConstrainedSockSize < MIN_CONSTRAINED_TCP_BUFFER ||
        options->ConstrainedSockSize > MAX_CONSTRAINED_TCP_BUFFER ||
        options->ConstrainedSockSize % 1024) {
      tor_asprintf(msg,
          "ConstrainedSockSize is invalid.  Must be a value between %d and %d "
          "in 1024 byte increments.",
          MIN_CONSTRAINED_TCP_BUFFER, MAX_CONSTRAINED_TCP_BUFFER);
      return -1;
    }
    if (options->DirPort) {
      /* Providing cached directory entries while system TCP buffers are scarce
       * will exacerbate the socket errors.  Suggest that this be disabled. */
      COMPLAIN("You have requested constrained socket buffers while also "
               "serving directory entries via DirPort.  It is strongly "
               "suggested that you disable serving directory requests when "
               "system TCP buffer resources are scarce.");
    }
  }

  if (options->V3AuthVoteDelay + options->V3AuthDistDelay >=
      options->V3AuthVotingInterval/2) {
    REJECT("V3AuthVoteDelay plus V3AuthDistDelay must be less than half "
           "V3AuthVotingInterval");
  }
  if (options->V3AuthVoteDelay < MIN_VOTE_SECONDS)
    REJECT("V3AuthVoteDelay is way too low.");
  if (options->V3AuthDistDelay < MIN_DIST_SECONDS)
    REJECT("V3AuthDistDelay is way too low.");

  if (options->V3AuthNIntervalsValid < 2)
    REJECT("V3AuthNIntervalsValid must be at least 2.");

  if (options->V3AuthVotingInterval < MIN_VOTE_INTERVAL) {
    REJECT("V3AuthVotingInterval is insanely low.");
  } else if (options->V3AuthVotingInterval > 24*60*60) {
    REJECT("V3AuthVotingInterval is insanely high.");
  } else if (((24*60*60) % options->V3AuthVotingInterval) != 0) {
    COMPLAIN("V3AuthVotingInterval does not divide evenly into 24 hours.");
  }

  if (rend_config_services(options, 1) < 0)
    REJECT("Failed to configure rendezvous options. See logs for details.");

  /* Parse client-side authorization for hidden services. */
  if (rend_parse_service_authorization(options, 1) < 0)
    REJECT("Failed to configure client authorization for hidden services. "
           "See logs for details.");

  if (parse_virtual_addr_network(options->VirtualAddrNetwork, 1, NULL)<0)
    return -1;

  if (options->PreferTunneledDirConns && !options->TunnelDirConns)
    REJECT("Must set TunnelDirConns if PreferTunneledDirConns is set.");

  if ((options->Socks4Proxy || options->Socks5Proxy) &&
      !options->HTTPProxy && !options->PreferTunneledDirConns)
    REJECT("When Socks4Proxy or Socks5Proxy is configured, "
           "PreferTunneledDirConns and TunnelDirConns must both be "
           "set to 1, or HTTPProxy must be configured.");

  if (options->AutomapHostsSuffixes) {
    SMARTLIST_FOREACH(options->AutomapHostsSuffixes, char *, suf,
    {
      size_t len = strlen(suf);
      if (len && suf[len-1] == '.')
        suf[len-1] = '\0';
    });
  }

  if (options->TestingTorNetwork && !options->DirServers) {
    REJECT("TestingTorNetwork may only be configured in combination with "
           "a non-default set of DirServers.");
  }

  if (options->AllowSingleHopExits && !options->DirServers) {
    COMPLAIN("You have set AllowSingleHopExits; now your relay will allow "
             "others to make one-hop exits. However, since by default most "
             "clients avoid relays that set this option, most clients will "
             "ignore you.");
  }

  /*XXXX023 checking for defaults manually like this is a bit fragile.*/

  /* Keep changes to hard-coded values synchronous to man page and default
   * values table. */
  if (options->TestingV3AuthInitialVotingInterval != 30*60 &&
      !options->TestingTorNetwork && !options->_UsingTestNetworkDefaults) {
    REJECT("TestingV3AuthInitialVotingInterval may only be changed in testing "
           "Tor networks!");
  } else if (options->TestingV3AuthInitialVotingInterval < MIN_VOTE_INTERVAL) {
    REJECT("TestingV3AuthInitialVotingInterval is insanely low.");
  } else if (((30*60) % options->TestingV3AuthInitialVotingInterval) != 0) {
    REJECT("TestingV3AuthInitialVotingInterval does not divide evenly into "
           "30 minutes.");
  }

  if (options->TestingV3AuthInitialVoteDelay != 5*60 &&
      !options->TestingTorNetwork && !options->_UsingTestNetworkDefaults) {

    REJECT("TestingV3AuthInitialVoteDelay may only be changed in testing "
           "Tor networks!");
  } else if (options->TestingV3AuthInitialVoteDelay < MIN_VOTE_SECONDS) {
    REJECT("TestingV3AuthInitialVoteDelay is way too low.");
  }

  if (options->TestingV3AuthInitialDistDelay != 5*60 &&
      !options->TestingTorNetwork && !options->_UsingTestNetworkDefaults) {
    REJECT("TestingV3AuthInitialDistDelay may only be changed in testing "
           "Tor networks!");
  } else if (options->TestingV3AuthInitialDistDelay < MIN_DIST_SECONDS) {
    REJECT("TestingV3AuthInitialDistDelay is way too low.");
  }

  if (options->TestingV3AuthInitialVoteDelay +
      options->TestingV3AuthInitialDistDelay >=
      options->TestingV3AuthInitialVotingInterval/2) {
    REJECT("TestingV3AuthInitialVoteDelay plus TestingV3AuthInitialDistDelay "
           "must be less than half TestingV3AuthInitialVotingInterval");
  }

  if (options->TestingAuthDirTimeToLearnReachability != 30*60 &&
      !options->TestingTorNetwork && !options->_UsingTestNetworkDefaults) {
    REJECT("TestingAuthDirTimeToLearnReachability may only be changed in "
           "testing Tor networks!");
  } else if (options->TestingAuthDirTimeToLearnReachability < 0) {
    REJECT("TestingAuthDirTimeToLearnReachability must be non-negative.");
  } else if (options->TestingAuthDirTimeToLearnReachability > 2*60*60) {
    COMPLAIN("TestingAuthDirTimeToLearnReachability is insanely high.");
  }

  if (options->TestingEstimatedDescriptorPropagationTime != 10*60 &&
      !options->TestingTorNetwork && !options->_UsingTestNetworkDefaults) {
    REJECT("TestingEstimatedDescriptorPropagationTime may only be changed in "
           "testing Tor networks!");
  } else if (options->TestingEstimatedDescriptorPropagationTime < 0) {
    REJECT("TestingEstimatedDescriptorPropagationTime must be non-negative.");
  } else if (options->TestingEstimatedDescriptorPropagationTime > 60*60) {
    COMPLAIN("TestingEstimatedDescriptorPropagationTime is insanely high.");
  }

  if (options->TestingTorNetwork) {
    log_warn(LD_CONFIG, "TestingTorNetwork is set. This will make your node "
                        "almost unusable in the public Tor network, and is "
                        "therefore only advised if you are building a "
                        "testing Tor network!");
  }

  if (options->AccelName && !options->HardwareAccel)
    options->HardwareAccel = 1;
  if (options->AccelDir && !options->AccelName)
    REJECT("Can't use hardware crypto accelerator dir without engine name.");

  if (options->PublishServerDescriptor)
    SMARTLIST_FOREACH(options->PublishServerDescriptor, const char *, pubdes, {
      if (!strcmp(pubdes, "1") || !strcmp(pubdes, "0"))
        if (smartlist_len(options->PublishServerDescriptor) > 1) {
          COMPLAIN("You have passed a list of multiple arguments to the "
                   "PublishServerDescriptor option that includes 0 or 1. "
                   "0 or 1 should only be used as the sole argument. "
                   "This configuration will be rejected in a future release.");
          break;
        }
    });

  if (options->BridgeRelay == 1 && ! options->ORPort)
      REJECT("BridgeRelay is 1, ORPort is not set. This is an invalid "
             "combination.");

  return 0;
#undef REJECT
#undef COMPLAIN
}

/** Helper: return true iff s1 and s2 are both NULL, or both non-NULL
 * equal strings. */
static int
opt_streq(const char *s1, const char *s2)
{
  return 0 == strcmp_opt(s1, s2);
}

/** Check if any of the previous options have changed but aren't allowed to. */
static int
options_transition_allowed(const or_options_t *old,
                           const or_options_t *new_val,
                           char **msg)
{
  if (!old)
    return 0;

  if (!opt_streq(old->PidFile, new_val->PidFile)) {
    *msg = tor_strdup("PidFile is not allowed to change.");
    return -1;
  }

  if (old->RunAsDaemon != new_val->RunAsDaemon) {
    *msg = tor_strdup("While Tor is running, changing RunAsDaemon "
                      "is not allowed.");
    return -1;
  }

  if (strcmp(old->DataDirectory,new_val->DataDirectory)!=0) {
    tor_asprintf(msg,
               "While Tor is running, changing DataDirectory "
               "(\"%s\"->\"%s\") is not allowed.",
               old->DataDirectory, new_val->DataDirectory);
    return -1;
  }

  if (!opt_streq(old->User, new_val->User)) {
    *msg = tor_strdup("While Tor is running, changing User is not allowed.");
    return -1;
  }

  if ((old->HardwareAccel != new_val->HardwareAccel)
      || !opt_streq(old->AccelName, new_val->AccelName)
      || !opt_streq(old->AccelDir, new_val->AccelDir)) {
    *msg = tor_strdup("While Tor is running, changing OpenSSL hardware "
                      "acceleration engine is not allowed.");
    return -1;
  }

  if (old->TestingTorNetwork != new_val->TestingTorNetwork) {
    *msg = tor_strdup("While Tor is running, changing TestingTorNetwork "
                      "is not allowed.");
    return -1;
  }

  if (old->DisableAllSwap != new_val->DisableAllSwap) {
    *msg = tor_strdup("While Tor is running, changing DisableAllSwap "
                      "is not allowed.");
    return -1;
  }

  if (old->TokenBucketRefillInterval != new_val->TokenBucketRefillInterval) {
    *msg = tor_strdup("While Tor is running, changing TokenBucketRefill"
                      "Interval is not allowed");
    return -1;
  }

  if (old->DisableIOCP != new_val->DisableIOCP) {
    *msg = tor_strdup("While Tor is running, changing DisableIOCP "
                      "is not allowed.");
    return -1;
  }

  return 0;
}

/** Return 1 if any change from <b>old_options</b> to <b>new_options</b>
 * will require us to rotate the CPU and DNS workers; else return 0. */
static int
options_transition_affects_workers(const or_options_t *old_options,
                                   const or_options_t *new_options)
{
  if (!opt_streq(old_options->DataDirectory, new_options->DataDirectory) ||
      old_options->NumCPUs != new_options->NumCPUs ||
      !config_lines_eq(old_options->ORPort, new_options->ORPort) ||
      old_options->ServerDNSSearchDomains !=
                                       new_options->ServerDNSSearchDomains ||
      old_options->_SafeLogging != new_options->_SafeLogging ||
      old_options->ClientOnly != new_options->ClientOnly ||
      public_server_mode(old_options) != public_server_mode(new_options) ||
      !config_lines_eq(old_options->Logs, new_options->Logs) ||
      old_options->LogMessageDomains != new_options->LogMessageDomains)
    return 1;

  /* Check whether log options match. */

  /* Nothing that changed matters. */
  return 0;
}

/** Return 1 if any change from <b>old_options</b> to <b>new_options</b>
 * will require us to generate a new descriptor; else return 0. */
static int
options_transition_affects_descriptor(const or_options_t *old_options,
                                      const or_options_t *new_options)
{
  /* XXX We can be smarter here. If your DirPort isn't being
   * published and you just turned it off, no need to republish. Etc. */
  if (!opt_streq(old_options->DataDirectory, new_options->DataDirectory) ||
      !opt_streq(old_options->Nickname,new_options->Nickname) ||
      !opt_streq(old_options->Address,new_options->Address) ||
      !config_lines_eq(old_options->ExitPolicy,new_options->ExitPolicy) ||
      old_options->ExitPolicyRejectPrivate !=
        new_options->ExitPolicyRejectPrivate ||
      !config_lines_eq(old_options->ORPort, new_options->ORPort) ||
      !config_lines_eq(old_options->DirPort, new_options->DirPort) ||
      old_options->ClientOnly != new_options->ClientOnly ||
      old_options->DisableNetwork != new_options->DisableNetwork ||
      old_options->_PublishServerDescriptor !=
        new_options->_PublishServerDescriptor ||
      get_effective_bwrate(old_options) != get_effective_bwrate(new_options) ||
      get_effective_bwburst(old_options) !=
        get_effective_bwburst(new_options) ||
      !opt_streq(old_options->ContactInfo, new_options->ContactInfo) ||
      !opt_streq(old_options->MyFamily, new_options->MyFamily) ||
      !opt_streq(old_options->AccountingStart, new_options->AccountingStart) ||
      old_options->AccountingMax != new_options->AccountingMax ||
      public_server_mode(old_options) != public_server_mode(new_options))
    return 1;

  return 0;
}

#ifdef MS_WINDOWS
/** Return the directory on windows where we expect to find our application
 * data. */
static char *
get_windows_conf_root(void)
{
  static int is_set = 0;
  static char path[MAX_PATH+1];
  TCHAR tpath[MAX_PATH] = {0};

  LPITEMIDLIST idl;
  IMalloc *m;
  HRESULT result;

  if (is_set)
    return path;

  /* Find X:\documents and settings\username\application data\ .
   * We would use SHGetSpecialFolder path, but that wasn't added until IE4.
   */
#ifdef ENABLE_LOCAL_APPDATA
#define APPDATA_PATH CSIDL_LOCAL_APPDATA
#else
#define APPDATA_PATH CSIDL_APPDATA
#endif
  if (!SUCCEEDED(SHGetSpecialFolderLocation(NULL, APPDATA_PATH, &idl))) {
    getcwd(path,MAX_PATH);
    is_set = 1;
    log_warn(LD_CONFIG,
             "I couldn't find your application data folder: are you "
             "running an ancient version of Windows 95? Defaulting to \"%s\"",
             path);
    return path;
  }
  /* Convert the path from an "ID List" (whatever that is!) to a path. */
  result = SHGetPathFromIDList(idl, tpath);
#ifdef UNICODE
  wcstombs(path,tpath,MAX_PATH);
#else
  strlcpy(path,tpath,sizeof(path));
#endif

  /* Now we need to free the memory that the path-idl was stored in.  In
   * typical Windows fashion, we can't just call 'free()' on it. */
  SHGetMalloc(&m);
  if (m) {
    m->lpVtbl->Free(m, idl);
    m->lpVtbl->Release(m);
  }
  if (!SUCCEEDED(result)) {
    return NULL;
  }
  strlcat(path,"\\tor",MAX_PATH);
  is_set = 1;
  return path;
}
#endif

/** Return the default location for our torrc file.
 * DOCDOC defaults_file */
static const char *
get_default_conf_file(int defaults_file)
{
#ifdef MS_WINDOWS
  if (defaults_file) {
    static char defaults_path[MAX_PATH+1];
    tor_snprintf(defaults_path, MAX_PATH, "%s\\torrc-defaults",
                 get_windows_conf_root());
    return defaults_path;
  } else {
    static char path[MAX_PATH+1];
    tor_snprintf(path, MAX_PATH, "%s\\torrc",
                 get_windows_conf_root());
    return path;
  }
#else
  return defaults_file ? CONFDIR "/torrc-defaults" : CONFDIR "/torrc";
#endif
}

/** Verify whether lst is a string containing valid-looking comma-separated
 * nicknames, or NULL. Return 0 on success. Warn and return -1 on failure.
 */
static int
check_nickname_list(const char *lst, const char *name, char **msg)
{
  int r = 0;
  smartlist_t *sl;

  if (!lst)
    return 0;
  sl = smartlist_create();

  smartlist_split_string(sl, lst, ",",
    SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK|SPLIT_STRIP_SPACE, 0);

  SMARTLIST_FOREACH(sl, const char *, s,
    {
      if (!is_legal_nickname_or_hexdigest(s)) {
        tor_asprintf(msg, "Invalid nickname '%s' in %s line", s, name);
        r = -1;
        break;
      }
    });
  SMARTLIST_FOREACH(sl, char *, s, tor_free(s));
  smartlist_free(sl);
  return r;
}

/** Learn config file name from command line arguments, or use the default,
 * DOCDOC defaults_file */
static char *
find_torrc_filename(int argc, char **argv,
                    int defaults_file,
                    int *using_default_torrc, int *ignore_missing_torrc)
{
  char *fname=NULL;
  int i;
  const char *fname_opt = defaults_file ? "--defaults-torrc" : "-f";
  const char *ignore_opt = defaults_file ? NULL : "--ignore-missing-torrc";

  if (defaults_file)
    *ignore_missing_torrc = 1;

  for (i = 1; i < argc; ++i) {
    if (i < argc-1 && !strcmp(argv[i],fname_opt)) {
      if (fname) {
        log(LOG_WARN, LD_CONFIG, "Duplicate %s options on command line.",
            fname_opt);
        tor_free(fname);
      }
      fname = expand_filename(argv[i+1]);

      {
        char *absfname;
        absfname = make_path_absolute(fname);
        tor_free(fname);
        fname = absfname;
      }

      *using_default_torrc = 0;
      ++i;
    } else if (ignore_opt && !strcmp(argv[i],ignore_opt)) {
      *ignore_missing_torrc = 1;
    }
  }

  if (*using_default_torrc) {
    /* didn't find one, try CONFDIR */
    const char *dflt = get_default_conf_file(defaults_file);
    if (dflt && file_status(dflt) == FN_FILE) {
      fname = tor_strdup(dflt);
    } else {
#ifndef MS_WINDOWS
      char *fn = NULL;
      if (!defaults_file)
        fn = expand_filename("~/.torrc");
      if (fn && file_status(fn) == FN_FILE) {
        fname = fn;
      } else {
        tor_free(fn);
        fname = tor_strdup(dflt);
      }
#else
      fname = tor_strdup(dflt);
#endif
    }
  }
  return fname;
}

/** Load torrc from disk, setting torrc_fname if successful.
 * DOCDOC defaults_file */
static char *
load_torrc_from_disk(int argc, char **argv, int defaults_file)
{
  char *fname=NULL;
  char *cf = NULL;
  int using_default_torrc = 1;
  int ignore_missing_torrc = 0;
  char **fname_var = defaults_file ? &torrc_defaults_fname : &torrc_fname;

  fname = find_torrc_filename(argc, argv, defaults_file,
                              &using_default_torrc, &ignore_missing_torrc);
  tor_assert(fname);
  log(LOG_DEBUG, LD_CONFIG, "Opening config file \"%s\"", fname);

  tor_free(*fname_var);
  *fname_var = fname;

  /* Open config file */
  if (file_status(fname) != FN_FILE ||
      !(cf = read_file_to_str(fname,0,NULL))) {
    if (using_default_torrc == 1 || ignore_missing_torrc) {
      if (!defaults_file)
        log(LOG_NOTICE, LD_CONFIG, "Configuration file \"%s\" not present, "
            "using reasonable defaults.", fname);
      tor_free(fname); /* sets fname to NULL */
      *fname_var = NULL;
      cf = tor_strdup("");
    } else {
      log(LOG_WARN, LD_CONFIG,
          "Unable to open configuration file \"%s\".", fname);
      goto err;
    }
  } else {
    log(LOG_NOTICE, LD_CONFIG, "Read configuration file \"%s\".", fname);
  }

  return cf;
 err:
  tor_free(fname);
  *fname_var = NULL;
  return NULL;
}

/** Read a configuration file into <b>options</b>, finding the configuration
 * file location based on the command line.  After loading the file
 * call options_init_from_string() to load the config.
 * Return 0 if success, -1 if failure. */
int
options_init_from_torrc(int argc, char **argv)
{
  char *cf=NULL, *cf_defaults=NULL;
  int i, command;
  int retval = -1;
  static char **backup_argv;
  static int backup_argc;
  char *command_arg = NULL;
  char *errmsg=NULL;

  if (argv) { /* first time we're called. save command line args */
    backup_argv = argv;
    backup_argc = argc;
  } else { /* we're reloading. need to clean up old options first. */
    argv = backup_argv;
    argc = backup_argc;
  }
  if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1],"--help"))) {
    print_usage();
    exit(0);
  }
  if (argc > 1 && !strcmp(argv[1], "--list-torrc-options")) {
    /* For documenting validating whether we've documented everything. */
    list_torrc_options();
    exit(0);
  }

  if (argc > 1 && (!strcmp(argv[1],"--version"))) {
    printf("Tor version %s.\n",get_version());
    exit(0);
  }
  if (argc > 1 && (!strcmp(argv[1],"--digests"))) {
    printf("Tor version %s.\n",get_version());
    printf("%s", libor_get_digests());
    printf("%s", tor_get_digests());
    exit(0);
  }

  /* Go through command-line variables */
  if (!global_cmdline_options) {
    /* Or we could redo the list every time we pass this place.
     * It does not really matter */
    if (config_get_commandlines(argc, argv, &global_cmdline_options) < 0) {
      goto err;
    }
  }

  command = CMD_RUN_TOR;
  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i],"--list-fingerprint")) {
      command = CMD_LIST_FINGERPRINT;
    } else if (!strcmp(argv[i],"--hash-password")) {
      command = CMD_HASH_PASSWORD;
      command_arg = tor_strdup( (i < argc-1) ? argv[i+1] : "");
      ++i;
    } else if (!strcmp(argv[i],"--verify-config")) {
      command = CMD_VERIFY_CONFIG;
    }
  }

  if (command == CMD_HASH_PASSWORD) {
    cf = tor_strdup("");
  } else {
    cf_defaults = load_torrc_from_disk(argc, argv, 1);
    cf = load_torrc_from_disk(argc, argv, 0);
    if (!cf)
      goto err;
  }

  retval = options_init_from_string(cf_defaults, cf, command, command_arg,
                                    &errmsg);

 err:

  tor_free(cf);
  tor_free(cf_defaults);
  if (errmsg) {
    log(LOG_WARN,LD_CONFIG,"%s", errmsg);
    tor_free(errmsg);
  }
  return retval < 0 ? -1 : 0;
}

/** Load the options from the configuration in <b>cf</b>, validate
 * them for consistency and take actions based on them.
 *
 * Return 0 if success, negative on error:
 *  * -1 for general errors.
 *  * -2 for failure to parse/validate,
 *  * -3 for transition not allowed
 *  * -4 for error while setting the new options
 */
setopt_err_t
options_init_from_string(const char *cf_defaults, const char *cf,
                         int command, const char *command_arg,
                         char **msg)
{
  or_options_t *oldoptions, *newoptions, *newdefaultoptions=NULL;
  config_line_t *cl;
  int retval, i;
  setopt_err_t err = SETOPT_ERR_MISC;
  tor_assert(msg);

  oldoptions = global_options; /* get_options unfortunately asserts if
                                  this is the first time we run*/

  newoptions = tor_malloc_zero(sizeof(or_options_t));
  newoptions->_magic = OR_OPTIONS_MAGIC;
  options_init(newoptions);
  newoptions->command = command;
  newoptions->command_arg = command_arg;

  for (i = 0; i < 2; ++i) {
    const char *body = i==0 ? cf_defaults : cf;
    if (!body)
      continue;
    /* get config lines, assign them */
    retval = config_get_lines(body, &cl, 1);
    if (retval < 0) {
      err = SETOPT_ERR_PARSE;
      goto err;
    }
    retval = config_assign(&options_format, newoptions, cl, 0, 0, msg);
    config_free_lines(cl);
    if (retval < 0) {
      err = SETOPT_ERR_PARSE;
      goto err;
    }
    if (i==0)
      newdefaultoptions = options_dup(&options_format, newoptions);
  }

  /* Go through command-line variables too */
  retval = config_assign(&options_format, newoptions,
                         global_cmdline_options, 0, 0, msg);
  if (retval < 0) {
    err = SETOPT_ERR_PARSE;
    goto err;
  }

  /* If this is a testing network configuration, change defaults
   * for a list of dependent config options, re-initialize newoptions
   * with the new defaults, and assign all options to it second time. */
  if (newoptions->TestingTorNetwork) {
    /* XXXX this is a bit of a kludge.  perhaps there's a better way to do
     * this?  We could, for example, make the parsing algorithm do two passes
     * over the configuration.  If it finds any "suite" options like
     * TestingTorNetwork, it could change the defaults before its second pass.
     * Not urgent so long as this seems to work, but at any sign of trouble,
     * let's clean it up.  -NM */

    /* Change defaults. */
    int i;
    for (i = 0; testing_tor_network_defaults[i].name; ++i) {
      const config_var_t *new_var = &testing_tor_network_defaults[i];
      config_var_t *old_var =
          config_find_option_mutable(&options_format, new_var->name);
      tor_assert(new_var);
      tor_assert(old_var);
      old_var->initvalue = new_var->initvalue;
    }

    /* Clear newoptions and re-initialize them with new defaults. */
    config_free(&options_format, newoptions);
    config_free(&options_format, newdefaultoptions);
    newdefaultoptions = NULL;
    newoptions = tor_malloc_zero(sizeof(or_options_t));
    newoptions->_magic = OR_OPTIONS_MAGIC;
    options_init(newoptions);
    newoptions->command = command;
    newoptions->command_arg = command_arg;

    /* Assign all options a second time. */
    for (i = 0; i < 2; ++i) {
      const char *body = i==0 ? cf_defaults : cf;
      if (!body)
        continue;
      /* get config lines, assign them */
      retval = config_get_lines(body, &cl, 1);
      if (retval < 0) {
        err = SETOPT_ERR_PARSE;
        goto err;
      }
      retval = config_assign(&options_format, newoptions, cl, 0, 0, msg);
      config_free_lines(cl);
      if (retval < 0) {
        err = SETOPT_ERR_PARSE;
        goto err;
      }
      if (i==0)
        newdefaultoptions = options_dup(&options_format, newoptions);
    }
  }

  /* Validate newoptions */
  if (options_validate(oldoptions, newoptions, 0, msg) < 0) {
    err = SETOPT_ERR_PARSE; /*XXX make this a separate return value.*/
    goto err;
  }

  if (options_transition_allowed(oldoptions, newoptions, msg) < 0) {
    err = SETOPT_ERR_TRANSITION;
    goto err;
  }

  if (set_options(newoptions, msg)) {
    err = SETOPT_ERR_SETTING;
    goto err; /* frees and replaces old options */
  }
  config_free(&options_format, global_default_options);
  global_default_options = newdefaultoptions;

  return SETOPT_OK;

 err:
  config_free(&options_format, newoptions);
  config_free(&options_format, newdefaultoptions);
  if (*msg) {
    char *old_msg = *msg;
    tor_asprintf(msg, "Failed to parse/validate config: %s", old_msg);
    tor_free(old_msg);
  }
  return err;
}

/** Return the location for our configuration file.
 */
const char *
get_torrc_fname(int defaults_fname)
{
  const char *fname = defaults_fname ? torrc_defaults_fname : torrc_fname;

  if (fname)
    return fname;
  else
    return get_default_conf_file(defaults_fname);
}

/** Adjust the address map based on the MapAddress elements in the
 * configuration <b>options</b>
 */
void
config_register_addressmaps(const or_options_t *options)
{
  smartlist_t *elts;
  config_line_t *opt;
  char *from, *to;

  addressmap_clear_configured();
  elts = smartlist_create();
  for (opt = options->AddressMap; opt; opt = opt->next) {
    int from_wildcard = 0, to_wildcard = 0;
    smartlist_split_string(elts, opt->value, NULL,
                           SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 2);
    if (smartlist_len(elts) < 2) {
      log_warn(LD_CONFIG,"MapAddress '%s' has too few arguments. Ignoring.",
               opt->value);
      goto cleanup;
    }

    from = smartlist_get(elts,0);
    to = smartlist_get(elts,1);

    if (to[0] == '.' || from[0] == '.') {
      log_warn(LD_CONFIG,"MapAddress '%s' is ambiguous - address starts with a"
              "'.'. Ignoring.",opt->value);
      goto cleanup;
    }

    if (!strcmp(to, "*") || !strcmp(from, "*")) {
      log_warn(LD_CONFIG,"MapAddress '%s' is unsupported - can't remap from "
               "or to *. Ignoring.",opt->value);
      goto cleanup;
    }
    /* Detect asterisks in expressions of type: '*.example.com' */
    if (!strncmp(from,"*.",2)) {
      from += 2;
      from_wildcard = 1;
    }
    if (!strncmp(to,"*.",2)) {
      to += 2;
      to_wildcard = 1;
    }

    if (to_wildcard && !from_wildcard) {
      log_warn(LD_CONFIG,
                "Skipping invalid argument '%s' to MapAddress: "
                "can only use wildcard (i.e. '*.') if 'from' address "
                "uses wildcard also", opt->value);
      goto cleanup;
    }

    if (address_is_invalid_destination(to, 1)) {
      log_warn(LD_CONFIG,
                "Skipping invalid argument '%s' to MapAddress", opt->value);
      goto cleanup;
    }

    addressmap_register(from, tor_strdup(to), 0, ADDRMAPSRC_TORRC,
                        from_wildcard, to_wildcard);

    if (smartlist_len(elts) > 2)
      log_warn(LD_CONFIG,"Ignoring extra arguments to MapAddress.");

  cleanup:
    SMARTLIST_FOREACH(elts, char*, cp, tor_free(cp));
    smartlist_clear(elts);
  }
  smartlist_free(elts);
}

/**
 * Initialize the logs based on the configuration file.
 */
static int
options_init_logs(or_options_t *options, int validate_only)
{
  config_line_t *opt;
  int ok;
  smartlist_t *elts;
  int daemon =
#ifdef MS_WINDOWS
               0;
#else
               options->RunAsDaemon;
#endif

  if (options->LogTimeGranularity <= 0) {
    log_warn(LD_CONFIG, "Log time granularity '%d' has to be positive.",
             options->LogTimeGranularity);
    return -1;
  } else if (1000 % options->LogTimeGranularity != 0 &&
             options->LogTimeGranularity % 1000 != 0) {
    int granularity = options->LogTimeGranularity;
    if (granularity < 40) {
      do granularity++;
      while (1000 % granularity != 0);
    } else if (granularity < 1000) {
      granularity = 1000 / granularity;
      while (1000 % granularity != 0)
        granularity--;
      granularity = 1000 / granularity;
    } else {
      granularity = 1000 * ((granularity / 1000) + 1);
    }
    log_warn(LD_CONFIG, "Log time granularity '%d' has to be either a "
                        "divisor or a multiple of 1 second. Changing to "
                        "'%d'.",
             options->LogTimeGranularity, granularity);
    if (!validate_only)
      set_log_time_granularity(granularity);
  } else {
    if (!validate_only)
      set_log_time_granularity(options->LogTimeGranularity);
  }

  ok = 1;
  elts = smartlist_create();

  for (opt = options->Logs; opt; opt = opt->next) {
    log_severity_list_t *severity;
    const char *cfg = opt->value;
    severity = tor_malloc_zero(sizeof(log_severity_list_t));
    if (parse_log_severity_config(&cfg, severity) < 0) {
      log_warn(LD_CONFIG, "Couldn't parse log levels in Log option 'Log %s'",
               opt->value);
      ok = 0; goto cleanup;
    }

    smartlist_split_string(elts, cfg, NULL,
                           SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 2);

    if (smartlist_len(elts) == 0)
      smartlist_add(elts, tor_strdup("stdout"));

    if (smartlist_len(elts) == 1 &&
        (!strcasecmp(smartlist_get(elts,0), "stdout") ||
         !strcasecmp(smartlist_get(elts,0), "stderr"))) {
      int err = smartlist_len(elts) &&
        !strcasecmp(smartlist_get(elts,0), "stderr");
      if (!validate_only) {
        if (daemon) {
          log_warn(LD_CONFIG,
                   "Can't log to %s with RunAsDaemon set; skipping stdout",
                   err?"stderr":"stdout");
        } else {
          add_stream_log(severity, err?"<stderr>":"<stdout>",
                         fileno(err?stderr:stdout));
        }
      }
      goto cleanup;
    }
    if (smartlist_len(elts) == 1 &&
        !strcasecmp(smartlist_get(elts,0), "syslog")) {
#ifdef HAVE_SYSLOG_H
      if (!validate_only) {
        add_syslog_log(severity);
      }
#else
      log_warn(LD_CONFIG, "Syslog is not supported on this system. Sorry.");
#endif
      goto cleanup;
    }

    if (smartlist_len(elts) == 2 &&
        !strcasecmp(smartlist_get(elts,0), "file")) {
      if (!validate_only) {
        char *fname = expand_filename(smartlist_get(elts, 1));
        if (add_file_log(severity, fname) < 0) {
          log_warn(LD_CONFIG, "Couldn't open file for 'Log %s': %s",
                   opt->value, strerror(errno));
          ok = 0;
        }
        tor_free(fname);
      }
      goto cleanup;
    }

    log_warn(LD_CONFIG, "Bad syntax on file Log option 'Log %s'",
             opt->value);
    ok = 0; goto cleanup;

  cleanup:
    SMARTLIST_FOREACH(elts, char*, cp, tor_free(cp));
    smartlist_clear(elts);
    tor_free(severity);
  }
  smartlist_free(elts);

  if (ok && !validate_only)
    logs_set_domain_logging(options->LogMessageDomains);

  return ok?0:-1;
}

/** Read the contents of a Bridge line from <b>line</b>. Return 0
 * if the line is well-formed, and -1 if it isn't. If
 * <b>validate_only</b> is 0, and the line is well-formed, then add
 * the bridge described in the line to our internal bridge list. */
static int
parse_bridge_line(const char *line, int validate_only)
{
  smartlist_t *items = NULL;
  int r;
  char *addrport=NULL, *fingerprint=NULL;
  char *transport_name=NULL;
  char *field1=NULL;
  tor_addr_t addr;
  uint16_t port = 0;
  char digest[DIGEST_LEN];

  items = smartlist_create();
  smartlist_split_string(items, line, NULL,
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, -1);
  if (smartlist_len(items) < 1) {
    log_warn(LD_CONFIG, "Too few arguments to Bridge line.");
    goto err;
  }

  /* field1 is either a transport name or addrport */
  field1 = smartlist_get(items, 0);
  smartlist_del_keeporder(items, 0);

  if (!(strstr(field1, ".") || strstr(field1, ":"))) {
    /* new-style bridge line */
    transport_name = field1;
    if (smartlist_len(items) < 1) {
      log_warn(LD_CONFIG, "Too few items to Bridge line.");
      goto err;
    }
    addrport = smartlist_get(items, 0);
    smartlist_del_keeporder(items, 0);
  } else {
    addrport = field1;
  }

  if (tor_addr_port_lookup(addrport, &addr, &port)<0) {
    log_warn(LD_CONFIG, "Error parsing Bridge address '%s'", addrport);
    goto err;
  }
  if (!port) {
    log_info(LD_CONFIG,
             "Bridge address '%s' has no port; using default port 443.",
             addrport);
    port = 443;
  }

  if (smartlist_len(items)) {
    fingerprint = smartlist_join_strings(items, "", 0, NULL);
    if (strlen(fingerprint) != HEX_DIGEST_LEN) {
      log_warn(LD_CONFIG, "Key digest for Bridge is wrong length.");
      goto err;
    }
    if (base16_decode(digest, DIGEST_LEN, fingerprint, HEX_DIGEST_LEN)<0) {
      log_warn(LD_CONFIG, "Unable to decode Bridge key digest.");
      goto err;
    }
  }

  if (!validate_only) {
    log_debug(LD_DIR, "Bridge at %s:%d (transport: %s) (%s)",
              fmt_addr(&addr), (int)port,
              transport_name ? transport_name : "no transport",
              fingerprint ? fingerprint : "no key listed");
    bridge_add_from_config(&addr, port,
                           fingerprint ? digest : NULL, transport_name);
  }

  r = 0;
  goto done;

 err:
  r = -1;

 done:
  SMARTLIST_FOREACH(items, char*, s, tor_free(s));
  smartlist_free(items);
  tor_free(addrport);
  tor_free(transport_name);
  tor_free(fingerprint);
  return r;
}

/** Read the contents of a ClientTransportPlugin line from
 * <b>line</b>. Return 0 if the line is well-formed, and -1 if it
 * isn't.
 *
 * If <b>validate_only</b> is 0, and the line is well-formed:
 * - If it's an external proxy line, add the transport described in the line to
 * our internal transport list.
 * - If it's a managed proxy line, launch the managed proxy. */
static int
parse_client_transport_line(const char *line, int validate_only)
{
  smartlist_t *items = NULL;
  int r;
  char *field2=NULL;

  const char *transports=NULL;
  smartlist_t *transport_list=NULL;
  char *addrport=NULL;
  tor_addr_t addr;
  uint16_t port = 0;
  int socks_ver=PROXY_NONE;

  /* managed proxy options */
  int is_managed=0;
  char **proxy_argv=NULL;
  char **tmp=NULL;
  int proxy_argc,i;

  int line_length;

  items = smartlist_create();
  smartlist_split_string(items, line, NULL,
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, -1);

  line_length =  smartlist_len(items);
  if (line_length < 3) {
    log_warn(LD_CONFIG, "Too few arguments on ClientTransportPlugin line.");
    goto err;
  }

  /* Get the first line element, split it to commas into
     transport_list (in case it's multiple transports) and validate
     the transport names. */
  transports = smartlist_get(items, 0);
  transport_list = smartlist_create();
  smartlist_split_string(transport_list, transports, ",",
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
  SMARTLIST_FOREACH_BEGIN(transport_list, const char *, transport_name) {
    if (!string_is_C_identifier(transport_name)) {
      log_warn(LD_CONFIG, "Transport name is not a C identifier (%s).",
               transport_name);
      goto err;
    }
  } SMARTLIST_FOREACH_END(transport_name);

  /* field2 is either a SOCKS version or "exec" */
  field2 = smartlist_get(items, 1);

  if (!strcmp(field2,"socks4")) {
    socks_ver = PROXY_SOCKS4;
  } else if (!strcmp(field2,"socks5")) {
    socks_ver = PROXY_SOCKS5;
  } else if (!strcmp(field2,"exec")) {
    is_managed=1;
  } else {
    log_warn(LD_CONFIG, "Strange ClientTransportPlugin field '%s'.",
             field2);
    goto err;
  }

  if (is_managed) { /* managed */
    if (!validate_only) {  /* if we are not just validating, use the
                             rest of the line as the argv of the proxy
                             to be launched */
      proxy_argc = line_length-2;
      tor_assert(proxy_argc > 0);
      proxy_argv = tor_malloc_zero(sizeof(char*)*(proxy_argc+1));
      tmp = proxy_argv;
      for (i=0;i<proxy_argc;i++) { /* store arguments */
        *tmp++ = smartlist_get(items, 2);
        smartlist_del_keeporder(items, 2);
      }
      *tmp = NULL; /*terminated with NUL pointer, just like execve() likes it*/

      /* kickstart the thing */
      pt_kickstart_client_proxy(transport_list, proxy_argv);
    }
  } else { /* external */
    if (smartlist_len(transport_list) != 1) {
      log_warn(LD_CONFIG, "You can't have an external proxy with "
               "more than one transports.");
      goto err;
    }

    addrport = smartlist_get(items, 2);

    if (tor_addr_port_lookup(addrport, &addr, &port)<0) {
      log_warn(LD_CONFIG, "Error parsing transport "
               "address '%s'", addrport);
      goto err;
    }
    if (!port) {
      log_warn(LD_CONFIG,
               "Transport address '%s' has no port.", addrport);
      goto err;
    }

    if (!validate_only) {
      transport_add_from_config(&addr, port, smartlist_get(transport_list, 0),
                                socks_ver);

      log_info(LD_DIR, "Transport '%s' found at %s:%d",
               transports, fmt_addr(&addr), (int)port);
    }
  }

  r = 0;
  goto done;

 err:
  r = -1;

 done:
  SMARTLIST_FOREACH(items, char*, s, tor_free(s));
  smartlist_free(items);
  if (transport_list) {
    SMARTLIST_FOREACH(transport_list, char*, s, tor_free(s));
    smartlist_free(transport_list);
  }

  return r;
}

/** Read the contents of a ServerTransportPlugin line from
 * <b>line</b>. Return 0 if the line is well-formed, and -1 if it
 * isn't.
 * If <b>validate_only</b> is 0, the line is well-formed, and it's a
 * managed proxy line, launch the managed proxy. */
static int
parse_server_transport_line(const char *line, int validate_only)
{
  smartlist_t *items = NULL;
  int r;
  const char *transports=NULL;
  smartlist_t *transport_list=NULL;
  char *type=NULL;
  char *addrport=NULL;
  tor_addr_t addr;
  uint16_t port = 0;

  /* managed proxy options */
  int is_managed=0;
  char **proxy_argv=NULL;
  char **tmp=NULL;
  int proxy_argc,i;

  int line_length;

  items = smartlist_create();
  smartlist_split_string(items, line, NULL,
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, -1);

  line_length =  smartlist_len(items);
  if (line_length < 3) {
    log_warn(LD_CONFIG, "Too few arguments on ServerTransportPlugin line.");
    goto err;
  }

  /* Get the first line element, split it to commas into
     transport_list (in case it's multiple transports) and validate
     the transport names. */
  transports = smartlist_get(items, 0);
  transport_list = smartlist_create();
  smartlist_split_string(transport_list, transports, ",",
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
  SMARTLIST_FOREACH_BEGIN(transport_list, const char *, transport_name) {
    if (!string_is_C_identifier(transport_name)) {
      log_warn(LD_CONFIG, "Transport name is not a C identifier (%s).",
               transport_name);
      goto err;
    }
  } SMARTLIST_FOREACH_END(transport_name);

  type = smartlist_get(items, 1);

  if (!strcmp(type, "exec")) {
    is_managed=1;
  } else if (!strcmp(type, "proxy")) {
    is_managed=0;
  } else {
    log_warn(LD_CONFIG, "Strange ServerTransportPlugin type '%s'", type);
    goto err;
  }

  if (is_managed) { /* managed */
    if (!validate_only) {
      proxy_argc = line_length-2;
      tor_assert(proxy_argc > 0);
      proxy_argv = tor_malloc_zero(sizeof(char*)*(proxy_argc+1));
      tmp = proxy_argv;

      for (i=0;i<proxy_argc;i++) { /* store arguments */
        *tmp++ = smartlist_get(items, 2);
        smartlist_del_keeporder(items, 2);
      }
      *tmp = NULL; /*terminated with NUL pointer, just like execve() likes it*/

      /* kickstart the thing */
      pt_kickstart_server_proxy(transport_list, proxy_argv);
    }
  } else { /* external */
    if (smartlist_len(transport_list) != 1) {
      log_warn(LD_CONFIG, "You can't have an external proxy with "
               "more than one transports.");
      goto err;
    }

    addrport = smartlist_get(items, 2);

    if (tor_addr_port_lookup(addrport, &addr, &port)<0) {
      log_warn(LD_CONFIG, "Error parsing transport "
               "address '%s'", addrport);
      goto err;
    }
    if (!port) {
      log_warn(LD_CONFIG,
               "Transport address '%s' has no port.", addrport);
      goto err;
    }

    if (!validate_only) {
      log_info(LD_DIR, "Server transport '%s' at %s:%d.",
               transports, fmt_addr(&addr), (int)port);
    }
  }

  r = 0;
  goto done;

 err:
  r = -1;

 done:
  SMARTLIST_FOREACH(items, char*, s, tor_free(s));
  smartlist_free(items);
  if (transport_list) {
    SMARTLIST_FOREACH(transport_list, char*, s, tor_free(s));
    smartlist_free(transport_list);
  }

  return r;
}

/** Read the contents of a DirServer line from <b>line</b>. If
 * <b>validate_only</b> is 0, and the line is well-formed, and it
 * shares any bits with <b>required_type</b> or <b>required_type</b>
 * is 0, then add the dirserver described in the line (minus whatever
 * bits it's missing) as a valid authority. Return 0 on success,
 * or -1 if the line isn't well-formed or if we can't add it. */
static int
parse_dir_server_line(const char *line, dirinfo_type_t required_type,
                      int validate_only)
{
  smartlist_t *items = NULL;
  int r;
  char *addrport=NULL, *address=NULL, *nickname=NULL, *fingerprint=NULL;
  uint16_t dir_port = 0, or_port = 0;
  char digest[DIGEST_LEN];
  char v3_digest[DIGEST_LEN];
  dirinfo_type_t type = V2_DIRINFO;
  int is_not_hidserv_authority = 0, is_not_v2_authority = 0;

  items = smartlist_create();
  smartlist_split_string(items, line, NULL,
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, -1);
  if (smartlist_len(items) < 1) {
    log_warn(LD_CONFIG, "No arguments on DirServer line.");
    goto err;
  }

  if (is_legal_nickname(smartlist_get(items, 0))) {
    nickname = smartlist_get(items, 0);
    smartlist_del_keeporder(items, 0);
  }

  while (smartlist_len(items)) {
    char *flag = smartlist_get(items, 0);
    if (TOR_ISDIGIT(flag[0]))
      break;
    if (!strcasecmp(flag, "v1")) {
      type |= (V1_DIRINFO | HIDSERV_DIRINFO);
    } else if (!strcasecmp(flag, "hs")) {
      type |= HIDSERV_DIRINFO;
    } else if (!strcasecmp(flag, "no-hs")) {
      is_not_hidserv_authority = 1;
    } else if (!strcasecmp(flag, "bridge")) {
      type |= BRIDGE_DIRINFO;
    } else if (!strcasecmp(flag, "no-v2")) {
      is_not_v2_authority = 1;
    } else if (!strcasecmpstart(flag, "orport=")) {
      int ok;
      char *portstring = flag + strlen("orport=");
      or_port = (uint16_t) tor_parse_long(portstring, 10, 1, 65535, &ok, NULL);
      if (!ok)
        log_warn(LD_CONFIG, "Invalid orport '%s' on DirServer line.",
                 portstring);
    } else if (!strcasecmpstart(flag, "v3ident=")) {
      char *idstr = flag + strlen("v3ident=");
      if (strlen(idstr) != HEX_DIGEST_LEN ||
          base16_decode(v3_digest, DIGEST_LEN, idstr, HEX_DIGEST_LEN)<0) {
        log_warn(LD_CONFIG, "Bad v3 identity digest '%s' on DirServer line",
                 flag);
      } else {
        type |= V3_DIRINFO|EXTRAINFO_DIRINFO|MICRODESC_DIRINFO;
      }
    } else {
      log_warn(LD_CONFIG, "Unrecognized flag '%s' on DirServer line",
               flag);
    }
    tor_free(flag);
    smartlist_del_keeporder(items, 0);
  }
  if (is_not_hidserv_authority)
    type &= ~HIDSERV_DIRINFO;
  if (is_not_v2_authority)
    type &= ~V2_DIRINFO;

  if (smartlist_len(items) < 2) {
    log_warn(LD_CONFIG, "Too few arguments to DirServer line.");
    goto err;
  }
  addrport = smartlist_get(items, 0);
  smartlist_del_keeporder(items, 0);
  if (addr_port_lookup(LOG_WARN, addrport, &address, NULL, &dir_port)<0) {
    log_warn(LD_CONFIG, "Error parsing DirServer address '%s'", addrport);
    goto err;
  }
  if (!dir_port) {
    log_warn(LD_CONFIG, "Missing port in DirServer address '%s'",addrport);
    goto err;
  }

  fingerprint = smartlist_join_strings(items, "", 0, NULL);
  if (strlen(fingerprint) != HEX_DIGEST_LEN) {
    log_warn(LD_CONFIG, "Key digest for DirServer is wrong length %d.",
             (int)strlen(fingerprint));
    goto err;
  }
  if (!strcmp(fingerprint, "E623F7625FBE0C87820F11EC5F6D5377ED816294")) {
    /* a known bad fingerprint. refuse to use it. We can remove this
     * clause once Tor 0.1.2.17 is obsolete. */
    log_warn(LD_CONFIG, "Dangerous dirserver line. To correct, erase your "
             "torrc file (%s), or reinstall Tor and use the default torrc.",
             get_torrc_fname(0));
    goto err;
  }
  if (base16_decode(digest, DIGEST_LEN, fingerprint, HEX_DIGEST_LEN)<0) {
    log_warn(LD_CONFIG, "Unable to decode DirServer key digest.");
    goto err;
  }

  if (!validate_only && (!required_type || required_type & type)) {
    if (required_type)
      type &= required_type; /* pare down what we think of them as an
                              * authority for. */
    log_debug(LD_DIR, "Trusted %d dirserver at %s:%d (%s)", (int)type,
              address, (int)dir_port, (char*)smartlist_get(items,0));
    if (!add_trusted_dir_server(nickname, address, dir_port, or_port,
                                digest, v3_digest, type))
      goto err;
  }

  r = 0;
  goto done;

  err:
  r = -1;

  done:
  SMARTLIST_FOREACH(items, char*, s, tor_free(s));
  smartlist_free(items);
  tor_free(addrport);
  tor_free(address);
  tor_free(nickname);
  tor_free(fingerprint);
  return r;
}

/** Free all storage held in <b>port</b> */
static void
port_cfg_free(port_cfg_t *port)
{
  tor_free(port);
}

/** Warn for every port in <b>ports</b> that is on a publicly routable
 * address. */
static void
warn_nonlocal_client_ports(const smartlist_t *ports, const char *portname)
{
  SMARTLIST_FOREACH_BEGIN(ports, const port_cfg_t *, port) {
    if (port->is_unix_addr) {
      /* Unix sockets aren't accessible over a network. */
    } else if (!tor_addr_is_internal(&port->addr, 1)) {
      log_warn(LD_CONFIG, "You specified a public address for %sPort. "
               "Other people on the Internet might find your computer and "
               "use it as an open proxy. Please don't allow this unless you "
               "have a good reason.", portname);
    } else if (!tor_addr_is_loopback(&port->addr)) {
      log_notice(LD_CONFIG, "You configured a non-loopback address for "
                 "%sPort. This allows everybody on your local network to use "
                 "your machine as a proxy. Make sure this is what you wanted.",
                 portname);
    }
  } SMARTLIST_FOREACH_END(port);
}

/** DOCDOC */
static void
warn_nonlocal_controller_ports(smartlist_t *ports, unsigned forbid)
{
  int warned = 0;
  SMARTLIST_FOREACH_BEGIN(ports, port_cfg_t *, port) {
    if (port->type != CONN_TYPE_CONTROL_LISTENER)
      continue;
    if (port->is_unix_addr)
      continue;
    if (!tor_addr_is_loopback(&port->addr)) {
      if (forbid) {
        if (!warned)
          log_warn(LD_CONFIG,
                 "You have a ControlPort set to accept "
                 "unauthenticated connections from a non-local address.  "
                 "This means that programs not running on your computer "
                 "can reconfigure your Tor, without even having to guess a "
                 "password.  That's so bad that I'm closing your ControlPort "
                 "for you.  If you need to control your Tor remotely, try "
                 "enabling authentication and using a tool like stunnel or "
                 "ssh to encrypt remote access.");
        warned = 1;
        port_cfg_free(port);
        SMARTLIST_DEL_CURRENT(ports, port);
      } else {
        log_warn(LD_CONFIG, "You have a ControlPort set to accept "
                 "connections from a non-local address.  This means that "
                 "programs not running on your computer can reconfigure your "
                 "Tor.  That's pretty bad, since the controller "
                 "protocol isn't encrypted!  Maybe you should just listen on "
                 "127.0.0.1 and use a tool like stunnel or ssh to encrypt "
                 "remote connections to your control port.");
        return; /* No point in checking the rest */
      }
    }
  } SMARTLIST_FOREACH_END(port);
}

#define CL_PORT_NO_OPTIONS (1u<<0)
#define CL_PORT_WARN_NONLOCAL (1u<<1)
#define CL_PORT_ALLOW_EXTRA_LISTENADDR (1u<<2)
#define CL_PORT_SERVER_OPTIONS (1u<<3)
#define CL_PORT_FORBID_NONLOCAL (1u<<4)

/**
 * Parse port configuration for a single port type.
 *
 * Read entries of the "FooPort" type from the list <b>ports</b>, and
 * entries of the "FooListenAddress" type from the list
 * <b>listenaddrs</b>.  Two syntaxes are supported: a legacy syntax
 * where FooPort is at most a single entry containing a port number and
 * where FooListenAddress has any number of address:port combinations;
 * and a new syntax where there are no FooListenAddress entries and
 * where FooPort can have any number of entries of the format
 * "[Address:][Port] IsolationOptions".
 *
 * In log messages, describe the port type as <b>portname</b>.
 *
 * If no address is specified, default to <b>defaultaddr</b>.  If no
 * FooPort is given, default to defaultport (if 0, there is no default).
 *
 * If CL_PORT_NO_OPTIONS is set in <b>flags</b>, do not allow stream
 * isolation options in the FooPort entries.
 *
 * If CL_PORT_WARN_NONLOCAL is set in <b>flags</b>, warn if any of the
 * ports are not on a local address.  If CL_PORT_FORBID_NONLOCAL is set,
 * this is a contrl port with no password set: don't even allow it.
 *
 * Unless CL_PORT_ALLOW_EXTRA_LISTENADDR is set in <b>flags</b>, warn
 * if FooListenAddress is set but FooPort is 0.
 *
 * If CL_PORT_SERVER_OPTIONS is set in <b>flags</b>, do not allow stream
 * isolation options in the FooPort entries; instead allow the
 * server-port option set.
 *
 * On success, if <b>out</b> is given, add a new port_cfg_t entry to
 * <b>out</b> for every port that the client should listen on.  Return 0
 * on success, -1 on failure.
 */
static int
parse_port_config(smartlist_t *out,
                         const config_line_t *ports,
                         const config_line_t *listenaddrs,
                         const char *portname,
                         int listener_type,
                         const char *defaultaddr,
                         int defaultport,
                         unsigned flags)
{
  smartlist_t *elts;
  int retval = -1;
  const unsigned is_control = (listener_type == CONN_TYPE_CONTROL_LISTENER);
  const unsigned allow_no_options = flags & CL_PORT_NO_OPTIONS;
  const unsigned use_server_options = flags & CL_PORT_SERVER_OPTIONS;
  const unsigned warn_nonlocal = flags & CL_PORT_WARN_NONLOCAL;
  const unsigned forbid_nonlocal = flags & CL_PORT_FORBID_NONLOCAL;
  const unsigned allow_spurious_listenaddr =
    flags & CL_PORT_ALLOW_EXTRA_LISTENADDR;

  /* FooListenAddress is deprecated; let's make it work like it used to work,
   * though. */
  if (listenaddrs) {
    int mainport = defaultport;

    if (ports && ports->next) {
      log_warn(LD_CONFIG, "%sListenAddress can't be used when there are "
               "multiple %sPort lines", portname, portname);
      return -1;
    } else if (ports) {
      if (!strcmp(ports->value, "auto")) {
        mainport = CFG_AUTO_PORT;
      } else {
        int ok;
        mainport = (int)tor_parse_long(ports->value, 10, 0, 65535, &ok, NULL);
        if (!ok) {
          log_warn(LD_CONFIG, "%sListenAddress can only be used with a single "
                   "%sPort with value \"auto\" or 1-65535.",
                   portname, portname);
          return -1;
        }
      }
    }

    if (mainport == 0) {
      if (allow_spurious_listenaddr)
        return 1;
      log_warn(LD_CONFIG, "%sPort must be defined if %sListenAddress is used",
               portname, portname);
      return -1;
    }

    if (use_server_options && out) {
      /* Add a no_listen port. */
      port_cfg_t *cfg = tor_malloc_zero(sizeof(port_cfg_t));
      cfg->type = listener_type;
      cfg->port = mainport;
      tor_addr_make_unspec(&cfg->addr); /* Server ports default to 0.0.0.0 */
      cfg->no_listen = 1;
      cfg->ipv4_only = 1;
      smartlist_add(out, cfg);
    }

    for (; listenaddrs; listenaddrs = listenaddrs->next) {
      tor_addr_t addr;
      uint16_t port = 0;
      if (tor_addr_port_lookup(listenaddrs->value, &addr, &port) < 0) {
        log_warn(LD_CONFIG, "Unable to parse %sListenAddress '%s'",
                 portname, listenaddrs->value);
        return -1;
      }
      if (out) {
        port_cfg_t *cfg = tor_malloc_zero(sizeof(port_cfg_t));
        cfg->type = listener_type;
        cfg->port = port ? port : mainport;
        tor_addr_copy(&cfg->addr, &addr);
        cfg->session_group = SESSION_GROUP_UNSET;
        cfg->isolation_flags = ISO_DEFAULT;
        cfg->no_advertise = 1;
        smartlist_add(out, cfg);
      }
    }

    if (warn_nonlocal && out) {
      if (is_control)
        warn_nonlocal_controller_ports(out, forbid_nonlocal);
      else
        warn_nonlocal_client_ports(out, portname);
    }
    return 0;
  } /* end if (listenaddrs) */

  /* No ListenAddress lines. If there's no FooPort, then maybe make a default
   * one. */
  if (! ports) {
    if (defaultport && out) {
       port_cfg_t *cfg = tor_malloc_zero(sizeof(port_cfg_t));
       cfg->type = listener_type;
       cfg->port = defaultport;
       tor_addr_parse(&cfg->addr, defaultaddr);
       cfg->session_group = SESSION_GROUP_UNSET;
       cfg->isolation_flags = ISO_DEFAULT;
       smartlist_add(out, cfg);
    }
    return 0;
  }

  /* At last we can actually parse the FooPort lines.  The syntax is:
   * [Addr:](Port|auto) [Options].*/
  elts = smartlist_create();

  for (; ports; ports = ports->next) {
    tor_addr_t addr;
    int port;
    int sessiongroup = SESSION_GROUP_UNSET;
    unsigned isolation = ISO_DEFAULT;

    char *addrport;
    uint16_t ptmp=0;
    int ok;
    int no_listen = 0, no_advertise = 0, all_addrs = 0,
      ipv4_only = 0, ipv6_only = 0;

    smartlist_split_string(elts, ports->value, NULL,
                           SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
    if (smartlist_len(elts) == 0) {
      log_warn(LD_CONFIG, "Invalid %sPort line with no value", portname);
      goto err;
    }

    if (allow_no_options && smartlist_len(elts) > 1) {
      log_warn(LD_CONFIG, "Too many options on %sPort line", portname);
      goto err;
    }

    /* Now parse the addr/port value */
    addrport = smartlist_get(elts, 0);
    if (!strcmp(addrport, "auto")) {
      port = CFG_AUTO_PORT;
      tor_addr_parse(&addr, defaultaddr);
    } else if (!strcasecmpend(addrport, ":auto")) {
      char *addrtmp = tor_strndup(addrport, strlen(addrport)-5);
      port = CFG_AUTO_PORT;
      if (tor_addr_port_lookup(addrtmp, &addr, &ptmp)<0 || ptmp) {
        log_warn(LD_CONFIG, "Invalid address '%s' for %sPort",
                 escaped(addrport), portname);
        tor_free(addrtmp);
        goto err;
      }
    } else {
      /* Try parsing integer port before address, because, who knows?
         "9050" might be a valid address. */
      port = (int) tor_parse_long(addrport, 10, 0, 65535, &ok, NULL);
      if (ok) {
        tor_addr_parse(&addr, defaultaddr);
      } else if (tor_addr_port_lookup(addrport, &addr, &ptmp) == 0) {
        if (ptmp == 0) {
          log_warn(LD_CONFIG, "%sPort line has address but no port", portname);
          goto err;
        }
        port = ptmp;
      } else {
        log_warn(LD_CONFIG, "Couldn't parse address '%s' for %sPort",
                 escaped(addrport), portname);
        goto err;
      }
    }

    /* Now parse the rest of the options, if any. */
    if (use_server_options) {
      /* This is a server port; parse advertising options */
      SMARTLIST_FOREACH_BEGIN(elts, char *, elt) {
        if (elt_sl_idx == 0)
          continue; /* Skip addr:port */

        if (!strcasecmp(elt, "NoAdvertise")) {
          no_advertise = 1;
        } else if (!strcasecmp(elt, "NoListen")) {
          no_listen = 1;
#if 0
        /* not implemented yet. */
        } else if (!strcasecmp(elt, "AllAddrs")) {

          all_addrs = 1;
#endif
        } else if (!strcasecmp(elt, "IPv4Only")) {
          ipv4_only = 1;
        } else if (!strcasecmp(elt, "IPv6Only")) {
          ipv6_only = 1;
        } else {
          log_warn(LD_CONFIG, "Unrecognized %sPort option '%s'",
                   portname, escaped(elt));
        }
      } SMARTLIST_FOREACH_END(elt);

      if (no_advertise && no_listen) {
        log_warn(LD_CONFIG, "Tried to set both NoListen and NoAdvertise "
                 "on %sPort line '%s'",
                 portname, escaped(ports->value));
        goto err;
      }
      if (ipv4_only && ipv6_only) {
        log_warn(LD_CONFIG, "Tried to set both IPv4Only and IPv6Only "
                 "on %sPort line '%s'",
                 portname, escaped(ports->value));
        goto err;
      }
      if (ipv4_only && tor_addr_family(&addr) == AF_INET6) {
        log_warn(LD_CONFIG, "Could not interpret %sPort address as IPv6",
                 portname);
        goto err;
      }
      if (ipv6_only && tor_addr_family(&addr) == AF_INET) {
        log_warn(LD_CONFIG, "Could not interpret %sPort address as IPv4",
                 portname);
        goto err;
      }
    } else {
      /* This is a client port; parse isolation options */
      SMARTLIST_FOREACH_BEGIN(elts, char *, elt) {
        int no = 0, isoflag = 0;
        const char *elt_orig = elt;
        if (elt_sl_idx == 0)
          continue; /* Skip addr:port */
        if (!strcasecmpstart(elt, "SessionGroup=")) {
          int group = (int)tor_parse_long(elt+strlen("SessionGroup="),
                                          10, 0, INT_MAX, &ok, NULL);
          if (!ok) {
            log_warn(LD_CONFIG, "Invalid %sPort option '%s'",
                     portname, escaped(elt));
            goto err;
          }
          if (sessiongroup >= 0) {
            log_warn(LD_CONFIG, "Multiple SessionGroup options on %sPort",
                     portname);
            goto err;
          }
          sessiongroup = group;
          continue;
        }

        if (!strcasecmpstart(elt, "No")) {
          no = 1;
          elt += 2;
        }
        if (!strcasecmpend(elt, "s"))
          elt[strlen(elt)-1] = '\0'; /* kill plurals. */

        if (!strcasecmp(elt, "IsolateDestPort")) {
          isoflag = ISO_DESTPORT;
        } else if (!strcasecmp(elt, "IsolateDestAddr")) {
          isoflag = ISO_DESTADDR;
        } else if (!strcasecmp(elt, "IsolateSOCKSAuth")) {
          isoflag = ISO_SOCKSAUTH;
        } else if (!strcasecmp(elt, "IsolateClientProtocol")) {
          isoflag = ISO_CLIENTPROTO;
        } else if (!strcasecmp(elt, "IsolateClientAddr")) {
          isoflag = ISO_CLIENTADDR;
        } else {
          log_warn(LD_CONFIG, "Unrecognized %sPort option '%s'",
                   portname, escaped(elt_orig));
        }

        if (no) {
          isolation &= ~isoflag;
        } else {
          isolation |= isoflag;
        }
      } SMARTLIST_FOREACH_END(elt);
    }

    if (out && port) {
      port_cfg_t *cfg = tor_malloc_zero(sizeof(port_cfg_t));
      cfg->type = listener_type;
      cfg->port = port;
      tor_addr_copy(&cfg->addr, &addr);
      cfg->session_group = sessiongroup;
      cfg->isolation_flags = isolation;
      cfg->no_listen = no_listen;
      cfg->no_listen = no_advertise;
      cfg->all_addrs = all_addrs;
      cfg->ipv4_only = ipv4_only;
      cfg->ipv6_only = ipv6_only;

      smartlist_add(out, cfg);
    }
    SMARTLIST_FOREACH(elts, char *, cp, tor_free(cp));
    smartlist_clear(elts);
  }

  if (warn_nonlocal && out) {
    if (is_control)
      warn_nonlocal_controller_ports(out, forbid_nonlocal);
    else
      warn_nonlocal_client_ports(out, portname);
  }

  retval = 0;
 err:
  SMARTLIST_FOREACH(elts, char *, cp, tor_free(cp));
  smartlist_free(elts);
  return retval;
}

/** DOCDOC */
static int
parse_socket_config(smartlist_t *out, const config_line_t *cfg,
                    int listener_type)
{

  if (!out)
    return 0;

  for ( ; cfg; cfg = cfg->next) {
    size_t len = strlen(cfg->value);
    port_cfg_t *port = tor_malloc_zero(sizeof(port_cfg_t) + len + 1);
    port->is_unix_addr = 1;
    memcpy(port->unix_addr, cfg->value, len+1);
    port->type = listener_type;
    smartlist_add(out, port);
  }

  return 0;
}

/** Parse all client port types (Socks, DNS, Trans, NATD) from
 * <b>options</b>. On success, set *<b>n_ports_out</b> to the number of
 * ports that are listed and return 0.  On failure, set *<b>msg</b> to a
 * description of the problem and return -1.
 *
 * If <b>validate_only</b> is false, set configured_client_ports to the
 * new list of ports parsed from <b>options</b>.
 **/
static int
parse_ports(const or_options_t *options, int validate_only,
            char **msg, int *n_ports_out)
{
  smartlist_t *ports;
  int retval = -1;

  ports = smartlist_create();

  *n_ports_out = 0;

  if (parse_port_config(ports,
             options->SocksPort, options->SocksListenAddress,
             "Socks", CONN_TYPE_AP_LISTENER,
             "127.0.0.1", 9050,
             CL_PORT_WARN_NONLOCAL|CL_PORT_ALLOW_EXTRA_LISTENADDR) < 0) {
    *msg = tor_strdup("Invalid SocksPort/SocksListenAddress configuration");
    goto err;
  }
  if (parse_port_config(ports,
                               options->DNSPort, options->DNSListenAddress,
                               "DNS", CONN_TYPE_AP_DNS_LISTENER,
                               "127.0.0.1", 0,
                               CL_PORT_WARN_NONLOCAL) < 0) {
    *msg = tor_strdup("Invalid DNSPort/DNSListenAddress configuration");
    goto err;
  }
  if (parse_port_config(ports,
                               options->TransPort, options->TransListenAddress,
                               "Trans", CONN_TYPE_AP_TRANS_LISTENER,
                               "127.0.0.1", 0,
                               CL_PORT_WARN_NONLOCAL) < 0) {
    *msg = tor_strdup("Invalid TransPort/TransListenAddress configuration");
    goto err;
  }
  if (parse_port_config(ports,
                               options->NATDPort, options->NATDListenAddress,
                               "NATD", CONN_TYPE_AP_NATD_LISTENER,
                               "127.0.0.1", 0,
                               CL_PORT_WARN_NONLOCAL) < 0) {
    *msg = tor_strdup("Invalid NatdPort/NatdListenAddress configuration");
    goto err;
  }
  {
    unsigned control_port_flags = CL_PORT_NO_OPTIONS | CL_PORT_WARN_NONLOCAL;
    const int any_passwords = (options->HashedControlPassword ||
                               options->HashedControlSessionPassword ||
                               options->CookieAuthentication);
    if (! any_passwords)
      control_port_flags |= CL_PORT_FORBID_NONLOCAL;

    if (parse_port_config(ports,
                          options->ControlPort, options->ControlListenAddress,
                          "Control", CONN_TYPE_CONTROL_LISTENER,
                          "127.0.0.1", 0,
                          control_port_flags) < 0) {
      *msg = tor_strdup("Invalid ControlPort/ControlListenAddress "
                        "configuration");
      goto err;
    }
    if (parse_socket_config(ports,
                            options->ControlSocket,
                            CONN_TYPE_CONTROL_LISTENER) < 0) {
      *msg = tor_strdup("Invalid ControlSocket configuration");
      goto err;
    }
  }
  if (! options->ClientOnly) {
    if (parse_port_config(ports,
                          options->ORPort, options->ORListenAddress,
                          "OR", CONN_TYPE_OR_LISTENER,
                          "0.0.0.0", 0,
                          CL_PORT_SERVER_OPTIONS) < 0) {
      *msg = tor_strdup("Invalid ORPort/ORListenAddress configuration");
      goto err;
    }
    if (parse_port_config(ports,
                          options->DirPort, options->DirListenAddress,
                          "Dir", CONN_TYPE_DIR_LISTENER,
                          "0.0.0.0", 0,
                          CL_PORT_SERVER_OPTIONS) < 0) {
      *msg = tor_strdup("Invalid DirPort/DirListenAddress configuration");
      goto err;
    }
  }

  if (check_server_ports(ports, options) < 0) {
    *msg = tor_strdup("Misconfigured server ports");
    goto err;
  }

  *n_ports_out = smartlist_len(ports);

  if (!validate_only) {
    if (configured_ports) {
      SMARTLIST_FOREACH(configured_ports,
                        port_cfg_t *, p, port_cfg_free(p));
      smartlist_free(configured_ports);
    }
    configured_ports = ports;
    ports = NULL; /* prevent free below. */
  }

  retval = 0;
 err:
  if (ports) {
    SMARTLIST_FOREACH(ports, port_cfg_t *, p, port_cfg_free(p));
    smartlist_free(ports);
  }
  return retval;
}

/** DOCDOC */
static int
check_server_ports(const smartlist_t *ports,
                   const or_options_t *options)
{
  int n_orport_advertised = 0;
  int n_orport_advertised_ipv4 = 0;
  int n_orport_listeners = 0;
  int n_dirport_advertised = 0;
  int n_dirport_listeners = 0;
  int n_low_port = 0;
  int r = 0;

  SMARTLIST_FOREACH_BEGIN(ports, const port_cfg_t *, port) {
    if (port->type == CONN_TYPE_DIR_LISTENER) {
      if (! port->no_advertise)
        ++n_dirport_advertised;
      if (! port->no_listen)
        ++n_dirport_listeners;
    } else if (port->type == CONN_TYPE_OR_LISTENER) {
      if (! port->no_advertise) {
        ++n_orport_advertised;
        if (tor_addr_family(&port->addr) == AF_INET ||
            (tor_addr_family(&port->addr) == AF_UNSPEC && !port->ipv6_only))
          ++n_orport_advertised_ipv4;
      }
      if (! port->no_listen)
        ++n_orport_listeners;
    } else {
      continue;
    }
#ifndef MS_WINDOWS
    if (!port->no_advertise && port->port < 1024)
      ++n_low_port;
#endif
  } SMARTLIST_FOREACH_END(port);

  if (n_orport_advertised && !n_orport_listeners) {
    log_warn(LD_CONFIG, "We are advertising an ORPort, but not actually "
             "listening on one.");
    r = -1;
  }
  if (n_dirport_advertised && !n_dirport_listeners) {
    log_warn(LD_CONFIG, "We are advertising a DirPort, but not actually "
             "listening on one.");
    r = -1;
  }
  if (n_dirport_advertised > 1) {
    log_warn(LD_CONFIG, "Can't advertise more than one DirPort.");
    r = -1;
  }
  if (n_orport_advertised && !n_orport_advertised_ipv4 &&
      !options->BridgeRelay) {
    log_warn(LD_CONFIG, "Configured non-bridge only to listen on an IPv6 "
             "address.");
    r = -1;
  }

  if (n_low_port && options->AccountingMax) {
    log(LOG_WARN, LD_CONFIG,
          "You have set AccountingMax to use hibernation. You have also "
          "chosen a low DirPort or OrPort. This combination can make Tor stop "
          "working when it tries to re-attach the port after a period of "
          "hibernation. Please choose a different port or turn off "
          "hibernation unless you know this combination will work on your "
          "platform.");
  }

  return r;
}

/** Return a list of port_cfg_t for client ports parsed from the
 * options. */
const smartlist_t *
get_configured_ports(void)
{
  if (!configured_ports)
    configured_ports = smartlist_create();
  return configured_ports;
}

/** Return the first advertised port of type <b>listener_type</b> in
    <b>address_family</b>.  */
int
get_first_advertised_port_by_type_af(int listener_type, int address_family)
{
  if (!configured_ports)
    return 0;
  SMARTLIST_FOREACH_BEGIN(configured_ports, const port_cfg_t *, cfg) {
    if (cfg->type == listener_type &&
        !cfg->no_advertise &&
        (tor_addr_family(&cfg->addr) == address_family ||
         tor_addr_family(&cfg->addr) == AF_UNSPEC)) {
      if (tor_addr_family(&cfg->addr) != AF_UNSPEC ||
          (address_family == AF_INET && !cfg->ipv6_only) ||
          (address_family == AF_INET6 && !cfg->ipv4_only)) {
        return cfg->port;
      }
    }
  } SMARTLIST_FOREACH_END(cfg);
  return 0;
}

/** Adjust the value of options->DataDirectory, or fill it in if it's
 * absent. Return 0 on success, -1 on failure. */
static int
normalize_data_directory(or_options_t *options)
{
#ifdef MS_WINDOWS
  char *p;
  if (options->DataDirectory)
    return 0; /* all set */
  p = tor_malloc(MAX_PATH);
  strlcpy(p,get_windows_conf_root(),MAX_PATH);
  options->DataDirectory = p;
  return 0;
#else
  const char *d = options->DataDirectory;
  if (!d)
    d = "~/.tor";

 if (strncmp(d,"~/",2) == 0) {
   char *fn = expand_filename(d);
   if (!fn) {
     log_warn(LD_CONFIG,"Failed to expand filename \"%s\".", d);
     return -1;
   }
   if (!options->DataDirectory && !strcmp(fn,"/.tor")) {
     /* If our homedir is /, we probably don't want to use it. */
     /* Default to LOCALSTATEDIR/tor which is probably closer to what we
      * want. */
     log_warn(LD_CONFIG,
              "Default DataDirectory is \"~/.tor\".  This expands to "
              "\"%s\", which is probably not what you want.  Using "
              "\"%s"PATH_SEPARATOR"tor\" instead", fn, LOCALSTATEDIR);
     tor_free(fn);
     fn = tor_strdup(LOCALSTATEDIR PATH_SEPARATOR "tor");
   }
   tor_free(options->DataDirectory);
   options->DataDirectory = fn;
 }
 return 0;
#endif
}

/** Check and normalize the value of options->DataDirectory; return 0 if it
 * is sane, -1 otherwise. */
static int
validate_data_directory(or_options_t *options)
{
  if (normalize_data_directory(options) < 0)
    return -1;
  tor_assert(options->DataDirectory);
  if (strlen(options->DataDirectory) > (512-128)) {
    log_warn(LD_CONFIG, "DataDirectory is too long.");
    return -1;
  }
  return 0;
}

/** This string must remain the same forevermore. It is how we
 * recognize that the torrc file doesn't need to be backed up. */
#define GENERATED_FILE_PREFIX "# This file was generated by Tor; " \
  "if you edit it, comments will not be preserved"
/** This string can change; it tries to give the reader an idea
 * that editing this file by hand is not a good plan. */
#define GENERATED_FILE_COMMENT "# The old torrc file was renamed " \
  "to torrc.orig.1 or similar, and Tor will ignore it"

/** Save a configuration file for the configuration in <b>options</b>
 * into the file <b>fname</b>.  If the file already exists, and
 * doesn't begin with GENERATED_FILE_PREFIX, rename it.  Otherwise
 * replace it.  Return 0 on success, -1 on failure. */
static int
write_configuration_file(const char *fname, const or_options_t *options)
{
  char *old_val=NULL, *new_val=NULL, *new_conf=NULL;
  int rename_old = 0, r;

  tor_assert(fname);

  switch (file_status(fname)) {
    case FN_FILE:
      old_val = read_file_to_str(fname, 0, NULL);
      if (!old_val || strcmpstart(old_val, GENERATED_FILE_PREFIX)) {
        rename_old = 1;
      }
      tor_free(old_val);
      break;
    case FN_NOENT:
      break;
    case FN_ERROR:
    case FN_DIR:
    default:
      log_warn(LD_CONFIG,
               "Config file \"%s\" is not a file? Failing.", fname);
      return -1;
  }

  if (!(new_conf = options_dump(options, 1))) {
    log_warn(LD_BUG, "Couldn't get configuration string");
    goto err;
  }

  tor_asprintf(&new_val, "%s\n%s\n\n%s",
               GENERATED_FILE_PREFIX, GENERATED_FILE_COMMENT, new_conf);

  if (rename_old) {
    int i = 1;
    size_t fn_tmp_len = strlen(fname)+32;
    char *fn_tmp;
    tor_assert(fn_tmp_len > strlen(fname)); /*check for overflow*/
    fn_tmp = tor_malloc(fn_tmp_len);
    while (1) {
      if (tor_snprintf(fn_tmp, fn_tmp_len, "%s.orig.%d", fname, i)<0) {
        log_warn(LD_BUG, "tor_snprintf failed inexplicably");
        tor_free(fn_tmp);
        goto err;
      }
      if (file_status(fn_tmp) == FN_NOENT)
        break;
      ++i;
    }
    log_notice(LD_CONFIG, "Renaming old configuration file to \"%s\"", fn_tmp);
    if (rename(fname, fn_tmp) < 0) {
      log_warn(LD_FS,
               "Couldn't rename configuration file \"%s\" to \"%s\": %s",
               fname, fn_tmp, strerror(errno));
      tor_free(fn_tmp);
      goto err;
    }
    tor_free(fn_tmp);
  }

  if (write_str_to_file(fname, new_val, 0) < 0)
    goto err;

  r = 0;
  goto done;
 err:
  r = -1;
 done:
  tor_free(new_val);
  tor_free(new_conf);
  return r;
}

/**
 * Save the current configuration file value to disk.  Return 0 on
 * success, -1 on failure.
 **/
int
options_save_current(void)
{
  /* This fails if we can't write to our configuration file.
   *
   * If we try falling back to datadirectory or something, we have a better
   * chance of saving the configuration, but a better chance of doing
   * something the user never expected. */
  return write_configuration_file(get_torrc_fname(0), get_options());
}

/** Mapping from a unit name to a multiplier for converting that unit into a
 * base unit.  Used by config_parse_unit. */
struct unit_table_t {
  const char *unit; /**< The name of the unit */
  uint64_t multiplier; /**< How many of the base unit appear in this unit */
};

/** Table to map the names of memory units to the number of bytes they
 * contain. */
static struct unit_table_t memory_units[] = {
  { "",          1 },
  { "b",         1<< 0 },
  { "byte",      1<< 0 },
  { "bytes",     1<< 0 },
  { "kb",        1<<10 },
  { "kbyte",     1<<10 },
  { "kbytes",    1<<10 },
  { "kilobyte",  1<<10 },
  { "kilobytes", 1<<10 },
  { "m",         1<<20 },
  { "mb",        1<<20 },
  { "mbyte",     1<<20 },
  { "mbytes",    1<<20 },
  { "megabyte",  1<<20 },
  { "megabytes", 1<<20 },
  { "gb",        1<<30 },
  { "gbyte",     1<<30 },
  { "gbytes",    1<<30 },
  { "gigabyte",  1<<30 },
  { "gigabytes", 1<<30 },
  { "tb",        U64_LITERAL(1)<<40 },
  { "terabyte",  U64_LITERAL(1)<<40 },
  { "terabytes", U64_LITERAL(1)<<40 },
  { NULL, 0 },
};

/** Table to map the names of time units to the number of seconds they
 * contain. */
static struct unit_table_t time_units[] = {
  { "",         1 },
  { "second",   1 },
  { "seconds",  1 },
  { "minute",   60 },
  { "minutes",  60 },
  { "hour",     60*60 },
  { "hours",    60*60 },
  { "day",      24*60*60 },
  { "days",     24*60*60 },
  { "week",     7*24*60*60 },
  { "weeks",    7*24*60*60 },
  { NULL, 0 },
};

/** Table to map the names of time units to the number of milliseconds
 * they contain. */
static struct unit_table_t time_msec_units[] = {
  { "",         1 },
  { "msec",     1 },
  { "millisecond", 1 },
  { "milliseconds", 1 },
  { "second",   1000 },
  { "seconds",  1000 },
  { "minute",   60*1000 },
  { "minutes",  60*1000 },
  { "hour",     60*60*1000 },
  { "hours",    60*60*1000 },
  { "day",      24*60*60*1000 },
  { "days",     24*60*60*1000 },
  { "week",     7*24*60*60*1000 },
  { "weeks",    7*24*60*60*1000 },
  { NULL, 0 },
};

/** Parse a string <b>val</b> containing a number, zero or more
 * spaces, and an optional unit string.  If the unit appears in the
 * table <b>u</b>, then multiply the number by the unit multiplier.
 * On success, set *<b>ok</b> to 1 and return this product.
 * Otherwise, set *<b>ok</b> to 0.
 */
static uint64_t
config_parse_units(const char *val, struct unit_table_t *u, int *ok)
{
  uint64_t v = 0;
  double d = 0;
  int use_float = 0;
  char *cp;

  tor_assert(ok);

  v = tor_parse_uint64(val, 10, 0, UINT64_MAX, ok, &cp);
  if (!*ok || (cp && *cp == '.')) {
    d = tor_parse_double(val, 0, UINT64_MAX, ok, &cp);
    if (!*ok)
      goto done;
    use_float = 1;
  }

  if (!cp) {
    *ok = 1;
    v = use_float ? DBL_TO_U64(d) :  v;
    goto done;
  }

  cp = (char*) eat_whitespace(cp);

  for ( ;u->unit;++u) {
    if (!strcasecmp(u->unit, cp)) {
      if (use_float)
        v = u->multiplier * d;
      else
        v *= u->multiplier;
      *ok = 1;
      goto done;
    }
  }
  log_warn(LD_CONFIG, "Unknown unit '%s'.", cp);
  *ok = 0;
 done:

  if (*ok)
    return v;
  else
    return 0;
}

/** Parse a string in the format "number unit", where unit is a unit of
 * information (byte, KB, M, etc).  On success, set *<b>ok</b> to true
 * and return the number of bytes specified.  Otherwise, set
 * *<b>ok</b> to false and return 0. */
static uint64_t
config_parse_memunit(const char *s, int *ok)
{
  uint64_t u = config_parse_units(s, memory_units, ok);
  return u;
}

/** Parse a string in the format "number unit", where unit is a unit of
 * time in milliseconds.  On success, set *<b>ok</b> to true and return
 * the number of milliseconds in the provided interval.  Otherwise, set
 * *<b>ok</b> to 0 and return -1. */
static int
config_parse_msec_interval(const char *s, int *ok)
{
  uint64_t r;
  r = config_parse_units(s, time_msec_units, ok);
  if (!ok)
    return -1;
  if (r > INT_MAX) {
    log_warn(LD_CONFIG, "Msec interval '%s' is too long", s);
    *ok = 0;
    return -1;
  }
  return (int)r;
}

/** Parse a string in the format "number unit", where unit is a unit of time.
 * On success, set *<b>ok</b> to true and return the number of seconds in
 * the provided interval.  Otherwise, set *<b>ok</b> to 0 and return -1.
 */
static int
config_parse_interval(const char *s, int *ok)
{
  uint64_t r;
  r = config_parse_units(s, time_units, ok);
  if (!ok)
    return -1;
  if (r > INT_MAX) {
    log_warn(LD_CONFIG, "Interval '%s' is too long", s);
    *ok = 0;
    return -1;
  }
  return (int)r;
}

/** Return the number of cpus configured in <b>options</b>.  If we are
 * told to auto-detect the number of cpus, return the auto-detected number. */
int
get_num_cpus(const or_options_t *options)
{
  if (options->NumCPUs == 0) {
    int n = compute_num_cpus();
    return (n >= 1) ? n : 1;
  } else {
    return options->NumCPUs;
  }
}

/**
 * Initialize the libevent library.
 */
static void
init_libevent(const or_options_t *options)
{
  const char *badness=NULL;
  tor_libevent_cfg cfg;

  tor_assert(options);

  configure_libevent_logging();
  /* If the kernel complains that some method (say, epoll) doesn't
   * exist, we don't care about it, since libevent will cope.
   */
  suppress_libevent_log_msg("Function not implemented");

  tor_check_libevent_header_compatibility();

  memset(&cfg, 0, sizeof(cfg));
  cfg.disable_iocp = options->DisableIOCP;
  cfg.num_cpus = get_num_cpus(options);
  cfg.msec_per_tick = options->TokenBucketRefillInterval;

  tor_libevent_initialize(&cfg);

  suppress_libevent_log_msg(NULL);

  tor_check_libevent_version(tor_libevent_get_method(),
                             get_options()->ORPort != NULL,
                             &badness);
  if (badness) {
    const char *v = tor_libevent_get_version_str();
    const char *m = tor_libevent_get_method();
    control_event_general_status(LOG_WARN,
        "BAD_LIBEVENT VERSION=%s METHOD=%s BADNESS=%s RECOVERED=NO",
                                 v, m, badness);
  }
}

/** Return the persistent state struct for this Tor. */
or_state_t *
get_or_state(void)
{
  tor_assert(global_state);
  return global_state;
}

/** Return a newly allocated string holding a filename relative to the data
 * directory.  If <b>sub1</b> is present, it is the first path component after
 * the data directory.  If <b>sub2</b> is also present, it is the second path
 * component after the data directory.  If <b>suffix</b> is present, it
 * is appended to the filename.
 *
 * Examples:
 *    get_datadir_fname2_suffix("a", NULL, NULL) -> $DATADIR/a
 *    get_datadir_fname2_suffix("a", NULL, ".tmp") -> $DATADIR/a.tmp
 *    get_datadir_fname2_suffix("a", "b", ".tmp") -> $DATADIR/a/b/.tmp
 *    get_datadir_fname2_suffix("a", "b", NULL) -> $DATADIR/a/b
 *
 * Note: Consider using the get_datadir_fname* macros in or.h.
 */
char *
options_get_datadir_fname2_suffix(const or_options_t *options,
                                  const char *sub1, const char *sub2,
                                  const char *suffix)
{
  char *fname = NULL;
  size_t len;
  tor_assert(options);
  tor_assert(options->DataDirectory);
  tor_assert(sub1 || !sub2); /* If sub2 is present, sub1 must be present. */
  len = strlen(options->DataDirectory);
  if (sub1) {
    len += strlen(sub1)+1;
    if (sub2)
      len += strlen(sub2)+1;
  }
  if (suffix)
    len += strlen(suffix);
  len++;
  fname = tor_malloc(len);
  if (sub1) {
    if (sub2) {
      tor_snprintf(fname, len, "%s"PATH_SEPARATOR"%s"PATH_SEPARATOR"%s",
                   options->DataDirectory, sub1, sub2);
    } else {
      tor_snprintf(fname, len, "%s"PATH_SEPARATOR"%s",
                   options->DataDirectory, sub1);
    }
  } else {
    strlcpy(fname, options->DataDirectory, len);
  }
  if (suffix)
    strlcat(fname, suffix, len);
  return fname;
}

/** Return true if <b>line</b> is a valid state TransportProxy line.
 *  Return false otherwise. */
static int
state_transport_line_is_valid(const char *line)
{
  smartlist_t *items = NULL;
  char *addrport=NULL;
  tor_addr_t addr;
  uint16_t port = 0;
  int r;

  items = smartlist_create();
  smartlist_split_string(items, line, NULL,
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, -1);

  if (smartlist_len(items) != 2) {
    log_warn(LD_CONFIG, "state: Not enough arguments in TransportProxy line.");
    goto err;
  }

  addrport = smartlist_get(items, 1);
  if (tor_addr_port_lookup(addrport, &addr, &port) < 0) {
    log_warn(LD_CONFIG, "state: Could not parse addrport.");
    goto err;
  }

  if (!port) {
    log_warn(LD_CONFIG, "state: Transport line did not contain port.");
    goto err;
  }

  r = 1;
  goto done;

 err:
  r = 0;

 done:
  SMARTLIST_FOREACH(items, char*, s, tor_free(s));
  smartlist_free(items);
  return r;
}

/** Return 0 if all TransportProxy lines in <b>state</b> are well
 *  formed. Otherwise, return -1. */
static int
validate_transports_in_state(or_state_t *state)
{
  int broken = 0;
  config_line_t *line;

  for (line = state->TransportProxies ; line ; line = line->next) {
    tor_assert(!strcmp(line->key, "TransportProxy"));
    if (!state_transport_line_is_valid(line->value))
      broken = 1;
  }

  if (broken)
    log_warn(LD_CONFIG, "state: State file seems to be broken.");

  return 0;
}

/** Return 0 if every setting in <b>state</b> is reasonable, and a
 * permissible transition from <b>old_state</b>.  Else warn and return -1.
 * Should have no side effects, except for normalizing the contents of
 * <b>state</b>.
 */
/* XXX from_setconf is here because of bug 238 */
static int
or_state_validate(or_state_t *old_state, or_state_t *state,
                  int from_setconf, char **msg)
{
  /* We don't use these; only options do. Still, we need to match that
   * signature. */
  (void) from_setconf;
  (void) old_state;

  if (entry_guards_parse_state(state, 0, msg)<0)
    return -1;

  if (validate_transports_in_state(state)<0)
    return -1;

  return 0;
}

/** Replace the current persistent state with <b>new_state</b> */
static int
or_state_set(or_state_t *new_state)
{
  char *err = NULL;
  int ret = 0;
  tor_assert(new_state);
  config_free(&state_format, global_state);
  global_state = new_state;
  if (entry_guards_parse_state(global_state, 1, &err)<0) {
    log_warn(LD_GENERAL,"%s",err);
    tor_free(err);
    ret = -1;
  }
  if (rep_hist_load_state(global_state, &err)<0) {
    log_warn(LD_GENERAL,"Unparseable bandwidth history state: %s",err);
    tor_free(err);
    ret = -1;
  }
  if (circuit_build_times_parse_state(&circ_times, global_state) < 0) {
    ret = -1;
  }
  return ret;
}

/**
 * Save a broken state file to a backup location.
 */
static void
or_state_save_broken(char *fname)
{
  int i;
  file_status_t status;
  size_t len = strlen(fname)+16;
  char *fname2 = tor_malloc(len);
  for (i = 0; i < 100; ++i) {
    tor_snprintf(fname2, len, "%s.%d", fname, i);
    status = file_status(fname2);
    if (status == FN_NOENT)
      break;
  }
  if (i == 100) {
    log_warn(LD_BUG, "Unable to parse state in \"%s\"; too many saved bad "
             "state files to move aside. Discarding the old state file.",
             fname);
    unlink(fname);
  } else {
    log_warn(LD_BUG, "Unable to parse state in \"%s\". Moving it aside "
             "to \"%s\".  This could be a bug in Tor; please tell "
             "the developers.", fname, fname2);
    if (rename(fname, fname2) < 0) {
      log_warn(LD_BUG, "Weirdly, I couldn't even move the state aside. The "
               "OS gave an error of %s", strerror(errno));
    }
  }
  tor_free(fname2);
}

/** Reload the persistent state from disk, generating a new state as needed.
 * Return 0 on success, less than 0 on failure.
 */
static int
or_state_load(void)
{
  or_state_t *new_state = NULL;
  char *contents = NULL, *fname;
  char *errmsg = NULL;
  int r = -1, badstate = 0;

  fname = get_datadir_fname("state");
  switch (file_status(fname)) {
    case FN_FILE:
      if (!(contents = read_file_to_str(fname, 0, NULL))) {
        log_warn(LD_FS, "Unable to read state file \"%s\"", fname);
        goto done;
      }
      break;
    case FN_NOENT:
      break;
    case FN_ERROR:
    case FN_DIR:
    default:
      log_warn(LD_GENERAL,"State file \"%s\" is not a file? Failing.", fname);
      goto done;
  }
  new_state = tor_malloc_zero(sizeof(or_state_t));
  new_state->_magic = OR_STATE_MAGIC;
  config_init(&state_format, new_state);
  if (contents) {
    config_line_t *lines=NULL;
    int assign_retval;
    if (config_get_lines(contents, &lines, 0)<0)
      goto done;
    assign_retval = config_assign(&state_format, new_state,
                                  lines, 0, 0, &errmsg);
    config_free_lines(lines);
    if (assign_retval<0)
      badstate = 1;
    if (errmsg) {
      log_warn(LD_GENERAL, "%s", errmsg);
      tor_free(errmsg);
    }
  }

  if (!badstate && or_state_validate(NULL, new_state, 1, &errmsg) < 0)
    badstate = 1;

  if (errmsg) {
    log_warn(LD_GENERAL, "%s", errmsg);
    tor_free(errmsg);
  }

  if (badstate && !contents) {
    log_warn(LD_BUG, "Uh oh.  We couldn't even validate our own default state."
             " This is a bug in Tor.");
    goto done;
  } else if (badstate && contents) {
    or_state_save_broken(fname);

    tor_free(contents);
    config_free(&state_format, new_state);

    new_state = tor_malloc_zero(sizeof(or_state_t));
    new_state->_magic = OR_STATE_MAGIC;
    config_init(&state_format, new_state);
  } else if (contents) {
    log_info(LD_GENERAL, "Loaded state from \"%s\"", fname);
  } else {
    log_info(LD_GENERAL, "Initialized state");
  }
  if (or_state_set(new_state) == -1) {
    or_state_save_broken(fname);
  }
  new_state = NULL;
  if (!contents) {
    global_state->next_write = 0;
    or_state_save(time(NULL));
  }
  r = 0;

 done:
  tor_free(fname);
  tor_free(contents);
  if (new_state)
    config_free(&state_format, new_state);

  return r;
}

/** Did the last time we tried to write the state file fail? If so, we
 * should consider disabling such features as preemptive circuit generation
 * to compute circuit-build-time. */
static int last_state_file_write_failed = 0;

/** Return whether the state file failed to write last time we tried. */
int
did_last_state_file_write_fail(void)
{
  return last_state_file_write_failed;
}

/** If writing the state to disk fails, try again after this many seconds. */
#define STATE_WRITE_RETRY_INTERVAL 3600

/** If we're a relay, how often should we checkpoint our state file even
 * if nothing else dirties it? This will checkpoint ongoing stats like
 * bandwidth used, per-country user stats, etc. */
#define STATE_RELAY_CHECKPOINT_INTERVAL (12*60*60)

/** Write the persistent state to disk. Return 0 for success, <0 on failure. */
int
or_state_save(time_t now)
{
  char *state, *contents;
  char tbuf[ISO_TIME_LEN+1];
  char *fname;

  tor_assert(global_state);

  if (global_state->next_write > now)
    return 0;

  /* Call everything else that might dirty the state even more, in order
   * to avoid redundant writes. */
  entry_guards_update_state(global_state);
  rep_hist_update_state(global_state);
  circuit_build_times_update_state(&circ_times, global_state);
  if (accounting_is_enabled(get_options()))
    accounting_run_housekeeping(now);

  global_state->LastWritten = now;

  tor_free(global_state->TorVersion);
  tor_asprintf(&global_state->TorVersion, "Tor %s", get_version());

  state = config_dump(&state_format, NULL, global_state, 1, 0);
  format_local_iso_time(tbuf, now);
  tor_asprintf(&contents,
               "# Tor state file last generated on %s local time\n"
               "# Other times below are in GMT\n"
               "# You *do not* need to edit this file.\n\n%s",
               tbuf, state);
  tor_free(state);
  fname = get_datadir_fname("state");
  if (write_str_to_file(fname, contents, 0)<0) {
    log_warn(LD_FS, "Unable to write state to file \"%s\"; "
             "will try again later", fname);
    last_state_file_write_failed = 1;
    tor_free(fname);
    tor_free(contents);
    /* Try again after STATE_WRITE_RETRY_INTERVAL (or sooner, if the state
     * changes sooner). */
    global_state->next_write = now + STATE_WRITE_RETRY_INTERVAL;
    return -1;
  }

  last_state_file_write_failed = 0;
  log_info(LD_GENERAL, "Saved state to \"%s\"", fname);
  tor_free(fname);
  tor_free(contents);

  if (server_mode(get_options()))
    global_state->next_write = now + STATE_RELAY_CHECKPOINT_INTERVAL;
  else
    global_state->next_write = TIME_MAX;

  return 0;
}

/** Return the config line for transport <b>transport</b> in the current state.
 *  Return NULL if there is no config line for <b>transport</b>. */
static config_line_t *
get_transport_in_state_by_name(const char *transport)
{
  or_state_t *or_state = get_or_state();
  config_line_t *line;
  config_line_t *ret = NULL;
  smartlist_t *items = NULL;

  for (line = or_state->TransportProxies ; line ; line = line->next) {
    tor_assert(!strcmp(line->key, "TransportProxy"));

    items = smartlist_create();
    smartlist_split_string(items, line->value, NULL,
                           SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, -1);
    if (smartlist_len(items) != 2) /* broken state */
      goto done;

    if (!strcmp(smartlist_get(items, 0), transport)) {
      ret = line;
      goto done;
    }

    SMARTLIST_FOREACH(items, char*, s, tor_free(s));
    smartlist_free(items);
    items = NULL;
  }

 done:
  if (items) {
    SMARTLIST_FOREACH(items, char*, s, tor_free(s));
    smartlist_free(items);
  }
  return ret;
}

/** Return string containing the address:port part of the
 *  TransportProxy <b>line</b> for transport <b>transport</b>.
 *  If the line is corrupted, return NULL. */
static const char *
get_transport_bindaddr(const char *line, const char *transport)
{
  char *line_tmp = NULL;

  if (strlen(line) < strlen(transport) + 2) {
    goto broken_state;
  } else {
    /* line should start with the name of the transport and a space.
       (for example, "obfs2 127.0.0.1:47245") */
    tor_asprintf(&line_tmp, "%s ", transport);
    if (strcmpstart(line, line_tmp))
      goto broken_state;

    tor_free(line_tmp);
    return (line+strlen(transport)+1);
  }

 broken_state:
  tor_free(line_tmp);
  return NULL;
}

/** Return a string containing the address:port that a proxy transport
 *  should bind on. The string is stored on the heap and must be freed
 *  by the caller of this function. */
char *
get_bindaddr_for_transport(const char *transport)
{
  char *default_addrport = NULL;
  const char *stored_bindaddr = NULL;

  config_line_t *line = get_transport_in_state_by_name(transport);
  if (!line) /* Found no references in state for this transport. */
    goto no_bindaddr_found;

  stored_bindaddr = get_transport_bindaddr(line->value, transport);
  if (stored_bindaddr) /* found stored bindaddr in state file. */
    return tor_strdup(stored_bindaddr);

 no_bindaddr_found:
  /** If we didn't find references for this pluggable transport in the
      state file, we should instruct the pluggable transport proxy to
      listen on INADDR_ANY on a random ephemeral port. */
  tor_asprintf(&default_addrport, "%s:%s", fmt_addr32(INADDR_ANY), "0");
  return default_addrport;
}

/** Save <b>transport</b> listening on <b>addr</b>:<b>port</b> to
    state */
void
save_transport_to_state(const char *transport,
                        const tor_addr_t *addr, uint16_t port)
{
  or_state_t *state = get_or_state();

  char *transport_addrport=NULL;

  /** find where to write on the state */
  config_line_t **next, *line;

  /* see if this transport is already stored in state */
  config_line_t *transport_line =
    get_transport_in_state_by_name(transport);

  if (transport_line) { /* if transport already exists in state... */
    const char *prev_bindaddr = /* get its addrport... */
      get_transport_bindaddr(transport_line->value, transport);
    tor_asprintf(&transport_addrport, "%s:%d", fmt_addr(addr), (int)port);

    /* if transport in state has the same address as this one, life is good */
    if (!strcmp(prev_bindaddr, transport_addrport)) {
      log_info(LD_CONFIG, "Transport seems to have spawned on its usual "
               "address:port.");
      goto done;
    } else { /* if addrport in state is different than the one we got */
      log_info(LD_CONFIG, "Transport seems to have spawned on different "
               "address:port. Let's update the state file with the new "
               "address:port");
      tor_free(transport_line->value); /* free the old line */
      tor_asprintf(&transport_line->value, "%s %s:%d", transport,
                   fmt_addr(addr),
                   (int) port); /* replace old addrport line with new line */
    }
  } else { /* never seen this one before; save it in state for next time */
    log_info(LD_CONFIG, "It's the first time we see this transport. "
             "Let's save its address:port");
    next = &state->TransportProxies;
    /* find the last TransportProxy line in the state and point 'next'
       right after it  */
    line = state->TransportProxies;
    while (line) {
      next = &(line->next);
      line = line->next;
    }

    /* allocate space for the new line and fill it in */
    *next = line = tor_malloc_zero(sizeof(config_line_t));
    line->key = tor_strdup("TransportProxy");
    tor_asprintf(&line->value, "%s %s:%d", transport,
                 fmt_addr(addr), (int) port);

    next = &(line->next);
  }

  if (!get_options()->AvoidDiskWrites)
    or_state_mark_dirty(state, 0);

 done:
  tor_free(transport_addrport);
}

/** Given a file name check to see whether the file exists but has not been
 * modified for a very long time.  If so, remove it. */
void
remove_file_if_very_old(const char *fname, time_t now)
{
#define VERY_OLD_FILE_AGE (28*24*60*60)
  struct stat st;

  if (stat(fname, &st)==0 && st.st_mtime < now-VERY_OLD_FILE_AGE) {
    char buf[ISO_TIME_LEN+1];
    format_local_iso_time(buf, st.st_mtime);
    log_notice(LD_GENERAL, "Obsolete file %s hasn't been modified since %s. "
               "Removing it.", fname, buf);
    unlink(fname);
  }
}

/** Helper to implement GETINFO functions about configuration variables (not
 * their values).  Given a "config/names" question, set *<b>answer</b> to a
 * new string describing the supported configuration variables and their
 * types. */
int
getinfo_helper_config(control_connection_t *conn,
                      const char *question, char **answer,
                      const char **errmsg)
{
  (void) conn;
  (void) errmsg;
  if (!strcmp(question, "config/names")) {
    smartlist_t *sl = smartlist_create();
    int i;
    for (i = 0; _option_vars[i].name; ++i) {
      const config_var_t *var = &_option_vars[i];
      const char *type;
      char *line;
      switch (var->type) {
        case CONFIG_TYPE_STRING: type = "String"; break;
        case CONFIG_TYPE_FILENAME: type = "Filename"; break;
        case CONFIG_TYPE_UINT: type = "Integer"; break;
        case CONFIG_TYPE_PORT: type = "Port"; break;
        case CONFIG_TYPE_INTERVAL: type = "TimeInterval"; break;
        case CONFIG_TYPE_MSEC_INTERVAL: type = "TimeMsecInterval"; break;
        case CONFIG_TYPE_MEMUNIT: type = "DataSize"; break;
        case CONFIG_TYPE_DOUBLE: type = "Float"; break;
        case CONFIG_TYPE_BOOL: type = "Boolean"; break;
        case CONFIG_TYPE_AUTOBOOL: type = "Boolean+Auto"; break;
        case CONFIG_TYPE_ISOTIME: type = "Time"; break;
        case CONFIG_TYPE_ROUTERSET: type = "RouterList"; break;
        case CONFIG_TYPE_CSV: type = "CommaList"; break;
        case CONFIG_TYPE_LINELIST: type = "LineList"; break;
        case CONFIG_TYPE_LINELIST_S: type = "Dependant"; break;
        case CONFIG_TYPE_LINELIST_V: type = "Virtual"; break;
        default:
        case CONFIG_TYPE_OBSOLETE:
          type = NULL; break;
      }
      if (!type)
        continue;
      tor_asprintf(&line, "%s %s\n",var->name,type);
      smartlist_add(sl, line);
    }
    *answer = smartlist_join_strings(sl, "", 0, NULL);
    SMARTLIST_FOREACH(sl, char *, c, tor_free(c));
    smartlist_free(sl);
  }
  return 0;
}

