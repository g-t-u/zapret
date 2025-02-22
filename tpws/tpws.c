#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <netinet/tcp.h>
#include <getopt.h>
#include <pwd.h>
#include <sys/resource.h>
#include <time.h>

#include "tpws.h"

#ifdef BSD
 #include <sys/sysctl.h>
#endif

#include "tpws_conn.h"
#include "hostlist.h"
#include "params.h"
#include "sec.h"
#include "redirect.h"
#include "helpers.h"

struct params_s params;

bool bHup = false;
static void onhup(int sig)
{
	printf("HUP received !\n");
	if (params.hostlist || params.hostlist_exclude)
		printf("Will reload hostlists on next request\n");
	bHup = true;
}
// should be called in normal execution
void dohup()
{
	if (bHup)
	{
		if (!LoadHostLists(&params.hostlist, &params.hostlist_files) ||
			!LoadHostLists(&params.hostlist_exclude, &params.hostlist_exclude_files))
		{
			// what will we do without hostlist ?? sure, gonna die
			exit(1);
		}
		bHup = false;
	}
}



static int8_t block_sigpipe()
{
	sigset_t sigset;
	memset(&sigset, 0, sizeof(sigset));

	//Get the old sigset, add SIGPIPE and update sigset
	if (sigprocmask(SIG_BLOCK, NULL, &sigset) == -1) {
		perror("sigprocmask (get)");
		return -1;
	}

	if (sigaddset(&sigset, SIGPIPE) == -1) {
		perror("sigaddset");
		return -1;
	}

	if (sigprocmask(SIG_BLOCK, &sigset, NULL) == -1) {
		perror("sigprocmask (set)");
		return -1;
	}

	return 0;
}


static bool is_interface_online(const char *ifname)
{
	struct ifreq ifr;
	int sock;
	
	if ((sock=socket(PF_INET, SOCK_DGRAM, IPPROTO_IP))==-1)
		return false;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ-1] = 0;
	ioctl(sock, SIOCGIFFLAGS, &ifr);
	close(sock);
	return !!(ifr.ifr_flags & IFF_UP);
}
static int get_default_ttl()
{
	int sock,ttl=0;
	socklen_t optlen=sizeof(ttl);
	
	if ((sock=socket(PF_INET, SOCK_DGRAM, IPPROTO_IP))!=-1)
	{
	    getsockopt(sock, IPPROTO_IP, IP_TTL, &ttl, &optlen);
	    close(sock);
	}
	return ttl;
}


static void exithelp()
{
	printf(
		" --bind-addr=<v4_addr>|<v6_addr>; for v6 link locals append %%interface_name\n"
		" --bind-iface4=<interface_name>\t; bind to the first ipv4 addr of interface\n"
		" --bind-iface6=<interface_name>\t; bind to the first ipv6 addr of interface\n"
		" --bind-linklocal=no|unwanted|prefer|force\n"
		"\t\t\t\t; prohibit, accept, prefer or force ipv6 link local bind\n"
		" --bind-wait-ifup=<sec>\t\t; wait for interface to appear and up\n"
		" --bind-wait-ip=<sec>\t\t; after ifup wait for ip address to appear up to N seconds\n"
		" --bind-wait-ip-linklocal=<sec>\t; (prefer) accept only LL first N seconds then any  (unwanted) accept only globals first N seconds then LL\n"
		" --bind-wait-only\t\t; wait for bind conditions satisfaction then exit. return code 0 if success.\n"
		" * multiple binds are supported. each bind-addr, bind-iface* start new bind\n"
		" --port=<port>\t\t\t; only one port number for all binds is supported\n"
		" --socks\t\t\t; implement socks4/5 proxy instead of transparent proxy\n"
		" --no-resolve\t\t\t; disable socks5 remote dns ability (resolves are not async, they block all activity)\n"
		" --local-rcvbuf=<bytes>\n"
		" --local-sndbuf=<bytes>\n"
		" --remote-rcvbuf=<bytes>\n"
		" --remote-sndbuf=<bytes>\n"
		" --skip-nodelay\t\t\t; do not set TCP_NODELAY option for outgoing connections (incompatible with split options)\n"
		" --maxconn=<max_connections>\n"
#ifdef SPLICE_PRESENT
		" --maxfiles=<max_open_files>\t; should be at least (X*connections+16), where X=6 in tcp proxy mode, X=4 in tampering mode\n"
#else
		" --maxfiles=<max_open_files>\t; should be at least (connections*2+16)\n"
#endif
		" --max-orphan-time=<sec>\t; if local leg sends something and closes and remote leg is still connecting then cancel connection attempt after N seconds\n"
		" --daemon\t\t\t; daemonize\n"
		" --pidfile=<filename>\t\t; write pid to file\n"
		" --user=<username>\t\t; drop root privs\n"
		" --uid=uid[:gid]\t\t; drop root privs\n"
#if defined(BSD) && !defined(__OpenBSD__) && !defined(__APPLE__)
		" --enable-pf\t\t\t; enable PF redirector support. required in FreeBSD when used with PF firewall.\n"
#endif
		" --debug=0|1|2\t\t\t; 0(default)=silent 1=verbose 2=debug\n"
		"\nTAMPERING:\n"
		" --hostlist=<filename>\t\t; only act on hosts in the list (one host per line, subdomains auto apply, gzip supported, multiple hostlists allowed)\n"
		" --hostlist-exclude=<filename>\t; do not act on hosts in the list (one host per line, subdomains auto apply, gzip supported, multiple hostlists allowed)\n"
		" --split-http-req=method|host\t; split at specified logical part of plain http request\n"
		" --split-pos=<numeric_offset>\t; split at specified pos. split-http-req takes precedence for http.\n"
		" --split-any-protocol\t\t; split not only http and https\n"
#if defined(BSD) && !defined(__APPLE__)
		" --disorder\t\t\t; when splitting simulate sending second fragment first (BSD sends entire message instead of first fragment, this is not good)\n"
#else
		" --disorder\t\t\t; when splitting simulate sending second fragment first\n"
#endif
		" --hostcase\t\t\t; change Host: => host:\n"
		" --hostspell\t\t\t; exact spelling of \"Host\" header. must be 4 chars. default is \"host\"\n"
		" --hostdot\t\t\t; add \".\" after Host: name\n"
		" --hosttab\t\t\t; add tab after Host: name\n"
		" --hostnospace\t\t\t; remove space after Host:\n"
		" --hostpad=<bytes>\t\t; add dummy padding headers before Host:\n"
		" --domcase\t\t\t; mix domain case : Host: TeSt.cOm\n"
		" --methodspace\t\t\t; add extra space after method\n"
		" --methodeol\t\t\t; add end-of-line before method\n"
		" --unixeol\t\t\t; replace 0D0A to 0A\n"
	);
	exit(1);
}
static void cleanup_params()
{
	strlist_destroy(&params.hostlist_files);
	strlist_destroy(&params.hostlist_exclude_files);
	if (params.hostlist_exclude)
	{
		StrPoolDestroy(&params.hostlist_exclude);
		params.hostlist_exclude = NULL;
	}
	if (params.hostlist)
	{
		StrPoolDestroy(&params.hostlist);
		params.hostlist = NULL;
	}
}
static void exithelp_clean()
{
	cleanup_params();
	exithelp();
}
static void exit_clean(int code)
{
	cleanup_params();
	exit(code);
}
static void nextbind_clean()
{
	params.binds_last++;
	if (params.binds_last>=MAX_BINDS)
	{
		fprintf(stderr,"maximum of %d binds are supported\n",MAX_BINDS);
		exit_clean(1);
	}
}
static void checkbind_clean()
{
	if (params.binds_last<0)
	{
		fprintf(stderr,"start new bind with --bind-addr,--bind-iface*\n");
		exit_clean(1);
	}
}


void save_default_ttl()
{
	if (!params.ttl_default)
	{
    	    params.ttl_default = get_default_ttl();
	    if (!params.ttl_default)
	    {
		    fprintf(stderr, "could not get default ttl\n");
		    exit_clean(1);
	    }
	}
}

void parse_params(int argc, char *argv[])
{
	int option_index = 0;
	int v, i;

	memset(&params, 0, sizeof(params));
	memcpy(params.hostspell, "host", 4); // default hostspell
	params.maxconn = DEFAULT_MAX_CONN;
	params.max_orphan_time = DEFAULT_MAX_ORPHAN_TIME;
	params.binds_last = -1;
	LIST_INIT(&params.hostlist_files);
	LIST_INIT(&params.hostlist_exclude_files);

#if defined(__OpenBSD__) || defined(__APPLE__)
	params.pf_enable = true; // OpenBSD and MacOS have no other choice
#endif
	if (can_drop_root())
	{
	    params.uid = params.gid = 0x7FFFFFFF; // default uid:gid
	    params.droproot = true;
	}

	const struct option long_options[] = {
		{ "help",no_argument,0,0 },// optidx=0
		{ "h",no_argument,0,0 },// optidx=1
		{ "bind-addr",required_argument,0,0 },// optidx=2
		{ "bind-iface4",required_argument,0,0 },// optidx=3
		{ "bind-iface6",required_argument,0,0 },// optidx=4
		{ "bind-linklocal",required_argument,0,0 },// optidx=5
		{ "bind-wait-ifup",required_argument,0,0 },// optidx=6
		{ "bind-wait-ip",required_argument,0,0 },// optidx=7
		{ "bind-wait-ip-linklocal",required_argument,0,0 },// optidx=8
		{ "bind-wait-only",no_argument,0,0 },// optidx=9
		{ "port",required_argument,0,0 },// optidx=10
		{ "daemon",no_argument,0,0 },// optidx=11
		{ "user",required_argument,0,0 },// optidx=12
		{ "uid",required_argument,0,0 },// optidx=13
		{ "maxconn",required_argument,0,0 },// optidx=14
		{ "maxfiles",required_argument,0,0 },// optidx=15
		{ "max-orphan-time",required_argument,0,0 },// optidx=16
		{ "hostcase",no_argument,0,0 },// optidx=17
		{ "hostspell",required_argument,0,0 },// optidx=18
		{ "hostdot",no_argument,0,0 },// optidx=19
		{ "hostnospace",no_argument,0,0 },// optidx=20
		{ "hostpad",required_argument,0,0 },// optidx=21
		{ "domcase",no_argument,0,0 },// optidx=22
		{ "split-http-req",required_argument,0,0 },// optidx=23
		{ "split-pos",required_argument,0,0 },// optidx=24
		{ "split-any-protocol",optional_argument,0,0},// optidx=25
		{ "disorder",no_argument,0,0 },// optidx=26
		{ "methodspace",no_argument,0,0 },// optidx=27
		{ "methodeol",no_argument,0,0 },// optidx=28
		{ "hosttab",no_argument,0,0 },// optidx=29
		{ "unixeol",no_argument,0,0 },// optidx=30
		{ "hostlist",required_argument,0,0 },// optidx=31
		{ "hostlist-exclude",required_argument,0,0 },// optidx=32
		{ "pidfile",required_argument,0,0 },// optidx=33
		{ "debug",optional_argument,0,0 },// optidx=34
		{ "local-rcvbuf",required_argument,0,0 },// optidx=35
		{ "local-sndbuf",required_argument,0,0 },// optidx=36
		{ "remote-rcvbuf",required_argument,0,0 },// optidx=37
		{ "remote-sndbuf",required_argument,0,0 },// optidx=38
		{ "socks",no_argument,0,0 },// optidx=39
		{ "no-resolve",no_argument,0,0 },// optidx=40
		{ "skip-nodelay",no_argument,0,0 },// optidx=41
#if defined(BSD) && !defined(__OpenBSD__) && !defined(__APPLE__)
		{ "enable-pf",no_argument,0,0 },// optidx=42
#endif
		{ NULL,0,NULL,0 }
	};
	while ((v = getopt_long_only(argc, argv, "", long_options, &option_index)) != -1)
	{
		if (v) exithelp_clean();
		switch (option_index)
		{
		case 0:
		case 1:
			exithelp_clean();
			break;
		case 2: /* bind-addr */
			nextbind_clean();
			{
				char *p = strchr(optarg,'%');
				if (p)
				{
					*p=0;
					strncpy(params.binds[params.binds_last].bindiface, p+1, sizeof(params.binds[params.binds_last].bindiface));
				}
				strncpy(params.binds[params.binds_last].bindaddr, optarg, sizeof(params.binds[params.binds_last].bindaddr));
			}
			params.binds[params.binds_last].bindaddr[sizeof(params.binds[params.binds_last].bindaddr) - 1] = 0;
			break;
		case 3: /* bind-iface4 */
			nextbind_clean();
			params.binds[params.binds_last].bind_if6=false;
			strncpy(params.binds[params.binds_last].bindiface, optarg, sizeof(params.binds[params.binds_last].bindiface));
			params.binds[params.binds_last].bindiface[sizeof(params.binds[params.binds_last].bindiface) - 1] = 0;
			break;
		case 4: /* bind-iface6 */
			nextbind_clean();
			params.binds[params.binds_last].bind_if6=true;
			strncpy(params.binds[params.binds_last].bindiface, optarg, sizeof(params.binds[params.binds_last].bindiface));
			params.binds[params.binds_last].bindiface[sizeof(params.binds[params.binds_last].bindiface) - 1] = 0;
			break;
		case 5: /* bind-linklocal */
			checkbind_clean();
			params.binds[params.binds_last].bindll = true;
			if (!strcmp(optarg, "no"))
				params.binds[params.binds_last].bindll=no;
			else if (!strcmp(optarg, "prefer"))
				params.binds[params.binds_last].bindll=prefer;
			else if (!strcmp(optarg, "force"))
				params.binds[params.binds_last].bindll=force;
			else if (!strcmp(optarg, "unwanted"))
				params.binds[params.binds_last].bindll=unwanted;
			else
			{
				fprintf(stderr, "invalid parameter in bind-linklocal : %s\n",optarg);
				exit_clean(1);
			}
			break;
		case 6: /* bind-wait-ifup */
			checkbind_clean();
			params.binds[params.binds_last].bind_wait_ifup = atoi(optarg);
			break;
		case 7: /* bind-wait-ip */
			checkbind_clean();
			params.binds[params.binds_last].bind_wait_ip = atoi(optarg);
			break;
		case 8: /* bind-wait-ip-linklocal */
			checkbind_clean();
			params.binds[params.binds_last].bind_wait_ip_ll = atoi(optarg);
			break;
		case 9: /* bind-wait-only */
			params.bind_wait_only = true;
			break;
		case 10: /* port */
			i = atoi(optarg);
			if (i <= 0 || i > 65535)
			{
				fprintf(stderr, "bad port number\n");
				exit_clean(1);
			}
			params.port = (uint16_t)i;
			break;
		case 11: /* daemon */
			params.daemon = true;
			break;
		case 12: /* user */
		{
			struct passwd *pwd = getpwnam(optarg);
			if (!pwd)
			{
				fprintf(stderr, "non-existent username supplied\n");
				exit_clean(1);
			}
			params.uid = pwd->pw_uid;
			params.gid = pwd->pw_gid;
			params.droproot = true;
			break;
		}
		case 13: /* uid */
			params.gid=0x7FFFFFFF; // default git. drop gid=0
			params.droproot = true;
			if (sscanf(optarg,"%u:%u",&params.uid,&params.gid)<1)
			{
				fprintf(stderr, "--uid should be : uid[:gid]\n");
				exit_clean(1);
			}
			break;
		case 14: /* maxconn */
			params.maxconn = atoi(optarg);
			if (params.maxconn <= 0 || params.maxconn > 10000)
			{
				fprintf(stderr, "bad maxconn\n");
				exit_clean(1);
			}
			break;
		case 15: /* maxfiles */
			params.maxfiles = atoi(optarg);
			if (params.maxfiles < 0)
			{
				fprintf(stderr, "bad maxfiles\n");
				exit_clean(1);
			}
			break;
		case 16: /* max-orphan-time */
			params.max_orphan_time = atoi(optarg);
			if (params.max_orphan_time < 0)
			{
				fprintf(stderr, "bad max_orphan_time\n");
				exit_clean(1);
			}
			break;
		case 17: /* hostcase */
			params.hostcase = true;
			params.tamper = true;
			break;
		case 18: /* hostspell */
			if (strlen(optarg) != 4)
			{
				fprintf(stderr, "hostspell must be exactly 4 chars long\n");
				exit_clean(1);
			}
			params.hostcase = true;
			memcpy(params.hostspell, optarg, 4);
			params.tamper = true;
			break;
		case 19: /* hostdot */
			params.hostdot = true;
			params.tamper = true;
			break;
		case 20: /* hostnospace */
			params.hostnospace = true;
			params.tamper = true;
			break;
		case 21: /* hostpad */
			params.hostpad = atoi(optarg);
			params.tamper = true;
			break;
		case 22: /* domcase */
			params.domcase = true;
			params.tamper = true;
			break;
		case 23: /* split-http-req */
			if (!strcmp(optarg, "method"))
				params.split_http_req = split_method;
			else if (!strcmp(optarg, "host"))
				params.split_http_req = split_host;
			else
			{
				fprintf(stderr, "Invalid argument for split-http-req\n");
				exit_clean(1);
			}
			params.tamper = true;
			break;
		case 24: /* split-pos */
			i = atoi(optarg);
			if (i)
				params.split_pos = i;
			else
			{
				fprintf(stderr, "Invalid argument for split-pos\n");
				exit_clean(1);
			}
			params.tamper = true;
			break;
		case 25: /* split-any-protocol */
			params.split_any_protocol = true;
			break;
		case 26: /* disorder */
			params.disorder = true;
			save_default_ttl();
			break;
		case 27: /* methodspace */
			params.methodspace = true;
			params.tamper = true;
			break;
		case 28: /* methodeol */
			params.methodeol = true;
			params.tamper = true;
			break;
		case 29: /* hosttab */
			params.hosttab = true;
			params.tamper = true;
			break;
		case 30: /* unixeol */
			params.unixeol = true;
			params.tamper = true;
			break;
		case 31: /* hostlist */
			if (!strlist_add(&params.hostlist_files, optarg))
			{
				fprintf(stderr, "strlist_add failed\n");
				exit_clean(1);
			}
			params.tamper = true;
			break;
		case 32: /* hostlist-exclude */
			if (!strlist_add(&params.hostlist_exclude_files, optarg))
			{
				fprintf(stderr, "strlist_add failed\n");
				exit_clean(1);
			}
			params.tamper = true;
			break;
		case 33: /* pidfile */
			strncpy(params.pidfile,optarg,sizeof(params.pidfile));
			params.pidfile[sizeof(params.pidfile)-1]='\0';
			break;
		case 34:
			params.debug = optarg ? atoi(optarg) : 1;
			break;
		case 35: /* local-rcvbuf */
			params.local_rcvbuf = atoi(optarg)/2;
			break;
		case 36: /* local-sndbuf */
			params.local_sndbuf = atoi(optarg)/2;
			break;
		case 37: /* remote-rcvbuf */
			params.remote_rcvbuf = atoi(optarg)/2;
			break;
		case 38: /* remote-sndbuf */
			params.remote_sndbuf = atoi(optarg)/2;
			break;
		case 39: /* socks */
			params.proxy_type = CONN_TYPE_SOCKS;
			break;
		case 40: /* no-resolve */
			params.no_resolve = true;
			break;
		case 41: /* skip-nodelay */
			params.skip_nodelay = true;
			break;
#if defined(BSD) && !defined(__OpenBSD__) && !defined(__APPLE__)
		case 42: /* enable-pf */
			params.pf_enable = true;
			break;
#endif
		}
	}
	if (!params.bind_wait_only && !params.port)
	{
		fprintf(stderr, "Need port number\n");
		exit_clean(1);
	}
	if (params.binds_last<=0)
	{
		params.binds_last=0; // default bind to all
	}
	if (params.skip_nodelay && (params.split_http_req || params.split_pos))
	{
		fprintf(stderr, "Cannot split with --skip-nodelay\n");
		exit_clean(1);
	}

	if (!LoadHostLists(&params.hostlist, &params.hostlist_files))
	{
		fprintf(stderr, "Include hostlist load failed\n");
		exit_clean(1);
	}
	if (!LoadHostLists(&params.hostlist_exclude, &params.hostlist_exclude_files))
	{
		fprintf(stderr, "Exclude hostlist load failed\n");
		exit_clean(1);
	}
}


static bool find_listen_addr(struct sockaddr_storage *salisten, const char *bindiface, bool bind_if6, enum bindll bindll, int *if_index)
{
	struct ifaddrs *addrs,*a;
	bool found=false;
	bool bindll_want = bindll==prefer || bindll==force;
    
	if (getifaddrs(&addrs)<0)
		return false;

	// for ipv6 preference order
	// bind-linklocal-1 : link-local,any
	// bind-linklocal=0 : private,global,link-local
	for(int pass=0;pass<3;pass++)
	{
		a  = addrs;
		while (a)
		{
			if (a->ifa_addr)
			{
				if (a->ifa_addr->sa_family==AF_INET &&
				    *bindiface && !bind_if6 && !strcmp(a->ifa_name, bindiface))
				{
					salisten->ss_family = AF_INET;
					memcpy(&((struct sockaddr_in*)salisten)->sin_addr, &((struct sockaddr_in*)a->ifa_addr)->sin_addr, sizeof(struct in_addr));
					found=true;
					goto ex;
				}
				// ipv6 links locals are fe80::/10
				else if (a->ifa_addr->sa_family==AF_INET6
				          &&
				         (!*bindiface && (bindll==prefer || bindll==force) ||
				          *bindiface && bind_if6 && !strcmp(a->ifa_name, bindiface))
				          &&
					 (bindll==force && is_linklocal((struct sockaddr_in6*)a->ifa_addr) ||
					  bindll==prefer && (pass==0 && is_linklocal((struct sockaddr_in6*)a->ifa_addr) || pass==1 && is_private6((struct sockaddr_in6*)a->ifa_addr) || pass==2) ||
					  bindll==no &&	(pass==0 && is_private6((struct sockaddr_in6*)a->ifa_addr) || pass==1 && !is_linklocal((struct sockaddr_in6*)a->ifa_addr)) ||
					  bindll==unwanted && (pass==0 && is_private6((struct sockaddr_in6*)a->ifa_addr) || pass==1 && !is_linklocal((struct sockaddr_in6*)a->ifa_addr) || pass==2))
					)
				{
					salisten->ss_family = AF_INET6;
					memcpy(&((struct sockaddr_in6*)salisten)->sin6_addr, &((struct sockaddr_in6*)a->ifa_addr)->sin6_addr, sizeof(struct in6_addr));
					if (if_index) *if_index = if_nametoindex(a->ifa_name);
					found=true;
					goto ex;
				}
			}
			a = a->ifa_next;
		}
	}
ex:
	freeifaddrs(addrs);
	return found;
}

static bool read_system_maxfiles(rlim_t *maxfile)
{
#ifdef __linux__
	FILE *F;
	int n;
	uintmax_t um;
	if (!(F=fopen("/proc/sys/fs/file-max","r")))
		return false;
	n=fscanf(F,"%ju",&um);
	fclose(F);
	if (!n)	return false;
	*maxfile = (rlim_t)um;
	return true;
#elif defined(BSD)
	int maxfiles,mib[2]={CTL_KERN, KERN_MAXFILES};
	size_t len = sizeof(maxfiles);
	if (sysctl(mib,2,&maxfiles,&len,NULL,0)==-1)
		return false;
	*maxfile = (rlim_t)maxfiles;
	return true;
#else
	return false;
#endif
}
static bool write_system_maxfiles(rlim_t maxfile)
{
#ifdef __linux__
	FILE *F;
	int n;
	if (!(F=fopen("/proc/sys/fs/file-max","w")))
		return false;
	n=fprintf(F,"%ju",(uintmax_t)maxfile);
	fclose(F);
	return !!n;
#elif defined(BSD)
	int maxfiles=(int)maxfile,mib[2]={CTL_KERN, KERN_MAXFILES};
	if (sysctl(mib,2,NULL,0,&maxfiles,sizeof(maxfiles))==-1)
		return false;
	return true;
#else
	return false;
#endif
}

static bool set_ulimit()
{
	rlim_t fdmax,fdmin_system,cur_lim=0;
	int n;

	if (!params.maxfiles)
	{
		// 4 fds per tamper connection (2 pipe + 2 socket), 6 fds for tcp proxy connection (4 pipe + 2 socket)
		// additional 1/2 for unpaired remote legs sending buffers
		// 16 for listen_fd, epoll, hostlist, ...
#ifdef SPLICE_PRESENT
		fdmax = (params.tamper ? 4 : 6) * params.maxconn;
#else
		fdmax = 2 * params.maxconn;
#endif
		fdmax += fdmax/2 + 16;
	}
	else
		fdmax = params.maxfiles;
	fdmin_system = fdmax + 4096;
	DBGPRINT("set_ulimit : fdmax=%ju fdmin_system=%ju",(uintmax_t)fdmax,(uintmax_t)fdmin_system)

	if (!read_system_maxfiles(&cur_lim))
		return false;
	DBGPRINT("set_ulimit : current system file-max=%ju",(uintmax_t)cur_lim)
	if (cur_lim<fdmin_system)
	{
		DBGPRINT("set_ulimit : system fd limit is too low. trying to increase to %ju",(uintmax_t)fdmin_system)
		if (!write_system_maxfiles(fdmin_system))
		{
			fprintf(stderr,"could not set system-wide max file descriptors\n");
			return false;
		}
	}

	struct rlimit rlim = {fdmax,fdmax};
	n=setrlimit(RLIMIT_NOFILE, &rlim);
	if (n==-1) perror("setrlimit");
	return n!=-1;
}

struct salisten_s
{
	struct sockaddr_storage salisten;
	socklen_t salisten_len;
	int ipv6_only;
	int bind_wait_ip_left; // how much seconds left from bind_wait_ip
};
static const char *bindll_s[] = { "unwanted","no","prefer","force" };
int main(int argc, char *argv[])
{
	int i, listen_fd[MAX_BINDS], yes = 1, retval = 0, if_index, exit_v=EXIT_FAILURE;
	struct salisten_s list[MAX_BINDS];
	char ip_port[48];

	srand(time(NULL));
	parse_params(argc, argv);

	if (params.daemon) daemonize();

	if (*params.pidfile && !writepid(params.pidfile))
	{
		fprintf(stderr,"could not write pidfile\n");
		goto exiterr;
	}

	memset(&list, 0, sizeof(list));
	for(i=0;i<=params.binds_last;i++) listen_fd[i]=-1;

	for(i=0;i<=params.binds_last;i++)
	{
		VPRINT("Prepare bind %d : addr=%s iface=%s v6=%u link_local=%s wait_ifup=%d wait_ip=%d wait_ip_ll=%d",i,
			params.binds[i].bindaddr,params.binds[i].bindiface,params.binds[i].bind_if6,bindll_s[params.binds[i].bindll],
			params.binds[i].bind_wait_ifup,params.binds[i].bind_wait_ip,params.binds[i].bind_wait_ip_ll);
		if_index=0;
		if (*params.binds[i].bindiface)
		{
			if (params.binds[i].bind_wait_ifup > 0)
			{
				int sec=0;
				if (!is_interface_online(params.binds[i].bindiface))
				{
					printf("waiting for ifup of %s for up to %d second(s)...\n",params.binds[i].bindiface,params.binds[i].bind_wait_ifup);
					do
					{
						sleep(1);
						sec++;
					}
					while (!is_interface_online(params.binds[i].bindiface) && sec<params.binds[i].bind_wait_ifup);
					if (sec>=params.binds[i].bind_wait_ifup)
					{
						printf("wait timed out\n");
						goto exiterr;
					}
				}
			}
			if (!(if_index = if_nametoindex(params.binds[i].bindiface)) && params.binds[i].bind_wait_ip<=0)
			{
				printf("bad iface %s\n",params.binds[i].bindiface);
				goto exiterr;
			}
		}
		list[i].bind_wait_ip_left = params.binds[i].bind_wait_ip;
		if (*params.binds[i].bindaddr)
		{
			if (inet_pton(AF_INET, params.binds[i].bindaddr, &((struct sockaddr_in*)(&list[i].salisten))->sin_addr))
			{
				list[i].salisten.ss_family = AF_INET;
			}
			else if (inet_pton(AF_INET6, params.binds[i].bindaddr, &((struct sockaddr_in6*)(&list[i].salisten))->sin6_addr))
			{
				list[i].salisten.ss_family = AF_INET6;
				list[i].ipv6_only = 1;
			}
			else
			{
				printf("bad bind addr : %s\n", params.binds[i].bindaddr);
				goto exiterr;
			}
		}
		else
		{
			if (*params.binds[i].bindiface || params.binds[i].bindll)
			{
				bool found;
				enum bindll bindll_1;
				int sec=0;

				if (params.binds[i].bind_wait_ip > 0)
				{
					printf("waiting for ip on %s for up to %d second(s)...\n", *params.binds[i].bindiface ? params.binds[i].bindiface : "<any>", params.binds[i].bind_wait_ip);
					if (params.binds[i].bind_wait_ip_ll>0)
					{
						if (params.binds[i].bindll==prefer)
							printf("during the first %d second(s) accepting only link locals...\n", params.binds[i].bind_wait_ip_ll);
						else if (params.binds[i].bindll==unwanted)
							printf("during the first %d second(s) accepting only ipv6 globals...\n", params.binds[i].bind_wait_ip_ll);
					}
				}

				for(;;)
				{
					// allow, no, prefer, force
					bindll_1 =	(params.binds[i].bindll==prefer && sec<params.binds[i].bind_wait_ip_ll) ? force : 
							(params.binds[i].bindll==unwanted && sec<params.binds[i].bind_wait_ip_ll) ? no : 
							params.binds[i].bindll;
					if (sec && sec==params.binds[i].bind_wait_ip_ll)
					{
						if (params.binds[i].bindll==prefer)
							printf("link local address wait timeout. now accepting globals\n");
						else if (params.binds[i].bindll==unwanted)
							printf("global ipv6 address wait timeout. now accepting link locals\n");
					}
					found = find_listen_addr(&list[i].salisten,params.binds[i].bindiface,params.binds[i].bind_if6,bindll_1,&if_index);
					if (found) break;

					if (sec>=params.binds[i].bind_wait_ip)
						break;

					sleep(1);
					sec++;
				} 

				if (!found)
				{
					printf("suitable ip address not found\n");
					goto exiterr;
				}
				list[i].bind_wait_ip_left = params.binds[i].bind_wait_ip - sec;
				list[i].ipv6_only=1;
			}
			else
			{
				list[i].salisten.ss_family = AF_INET6;
				// leave sin6_addr zero
			}
		}
		if (list[i].salisten.ss_family == AF_INET6)
		{
			list[i].salisten_len = sizeof(struct sockaddr_in6);
			((struct sockaddr_in6*)(&list[i].salisten))->sin6_port = htons(params.port);
			if (is_linklocal((struct sockaddr_in6*)(&list[i].salisten)))
				((struct sockaddr_in6*)(&list[i].salisten))->sin6_scope_id = if_index;
		}
		else
		{
			list[i].salisten_len = sizeof(struct sockaddr_in);
			((struct sockaddr_in*)(&list[i].salisten))->sin_port = htons(params.port);
		}
	}

	if (params.bind_wait_only)
	{
		printf("bind wait condition satisfied\n");
		exit_v = 0;
		goto exiterr;
	}

	if (params.proxy_type==CONN_TYPE_TRANSPARENT && !redir_init())
	{
		fprintf(stderr,"could not initialize redirector !!!\n");
		goto exiterr;
	}

	for(i=0;i<=params.binds_last;i++)
	{
		if (params.debug)
		{
			ntop46_port((struct sockaddr *)&list[i].salisten, ip_port, sizeof(ip_port));
			VPRINT("Binding %d to %s",i,ip_port);
		}

		if ((listen_fd[i] = socket(list[i].salisten.ss_family, SOCK_STREAM, 0)) == -1) {
			perror("socket");
			goto exiterr;
		}

#ifndef __OpenBSD__
// in OpenBSD always IPV6_ONLY for wildcard sockets
		if ((list[i].salisten.ss_family == AF_INET6) && setsockopt(listen_fd[i], IPPROTO_IPV6, IPV6_V6ONLY, &list[i].ipv6_only, sizeof(int)) == -1)
		{
			perror("setsockopt (IPV6_ONLY)");
			goto exiterr;
		}
#endif

		if (setsockopt(listen_fd[i], SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1)
		{
			perror("setsockopt (SO_REUSEADDR)");
			goto exiterr;
		}
	
		//Mark that this socket can be used for transparent proxying
		//This allows the socket to accept connections for non-local IPs
		if (params.proxy_type==CONN_TYPE_TRANSPARENT)
		{
		#ifdef __linux__
			if (setsockopt(listen_fd[i], SOL_IP, IP_TRANSPARENT, &yes, sizeof(yes)) == -1)
			{
				perror("setsockopt (IP_TRANSPARENT)");
				goto exiterr;
			}
		#elif defined(BSD) && defined(SO_BINDANY)
			if (setsockopt(listen_fd[i], SOL_SOCKET, SO_BINDANY, &yes, sizeof(yes)) == -1)
			{
				perror("setsockopt (SO_BINDANY)");
				goto exiterr;
			}
		#endif
		}

		if (!set_socket_buffers(listen_fd[i], params.local_rcvbuf, params.local_sndbuf))
			goto exiterr;
		if (!params.local_rcvbuf)
		{
			// HACK : dont know why but if dont set RCVBUF explicitly RCVBUF of accept()-ed socket can be very large. may be linux bug ?
			int v;
			socklen_t sz=sizeof(int);
			if (!getsockopt(listen_fd[i],SOL_SOCKET,SO_RCVBUF,&v,&sz))
			{
				v/=2;
				setsockopt(listen_fd[i],SOL_SOCKET,SO_RCVBUF,&v,sizeof(int));
			}
		}
		bool bBindBug=false;
		for(;;)
		{
			if (bind(listen_fd[i], (struct sockaddr *)&list[i].salisten, list[i].salisten_len) == -1)
			{
				// in linux strange behaviour was observed
				// just after ifup and address assignment there's short window when bind() can't bind to addresses got from getifaddrs()
				// it does not happen to transparent sockets because they can bind to any non-existend ip
				// also only ipv6 seem to be buggy this way
				if (errno==EADDRNOTAVAIL && params.proxy_type!=CONN_TYPE_TRANSPARENT && list[i].bind_wait_ip_left)
				{
					if (!bBindBug)
					{
						ntop46_port((struct sockaddr *)&list[i].salisten, ip_port, sizeof(ip_port));
						printf("address %s is not available. will retry for %d sec\n",ip_port,list[i].bind_wait_ip_left);
						bBindBug=true;
					}
					sleep(1);
					list[i].bind_wait_ip_left--;
					continue;
				}
				perror("bind");
				goto exiterr;
			}
			break;
		}
		if (listen(listen_fd[i], BACKLOG) == -1)
		{
			perror("listen");
			goto exiterr;
		}
	}

	set_ulimit();
	sec_harden();

	if (params.droproot && !droproot(params.uid,params.gid))
		goto exiterr;
	print_id();
	//splice() causes the process to receive the SIGPIPE-signal if one part (for
	//example a socket) is closed during splice(). I would rather have splice()
	//fail and return -1, so blocking SIGPIPE.
	if (block_sigpipe() == -1) {
		fprintf(stderr, "Could not block SIGPIPE signal\n");
		goto exiterr;
	}

	printf(params.proxy_type==CONN_TYPE_SOCKS ? "socks mode\n" : "transparent proxy mode\n");
	if (!params.tamper) printf("TCP proxy mode (no tampering)\n");

	signal(SIGHUP, onhup); 

	retval = event_loop(listen_fd,params.binds_last+1);
	exit_v = retval < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
	printf("Exiting\n");
	
exiterr:
	redir_close();
	for(i=0;i<=params.binds_last;i++) if (listen_fd[i]!=-1) close(listen_fd[i]);
	cleanup_params();
	return exit_v;
}
