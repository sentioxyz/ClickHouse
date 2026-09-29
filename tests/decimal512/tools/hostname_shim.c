/* hostname_shim.c - LD_PRELOAD library for isolated, non-production ClickHouse test runs (isolated_ch.sh).
 *
 * gethostname() and uname().nodename return $CH_ISO_HOSTNAME (default "localhost") in the isolated server,
 * clickhouse-test and every client or shell they start.
 *
 * Why: clickhouse-test replaces the runner's host name in each test's stdout with "localhost" as a plain substring
 * (replace_in_file(stdout, socket.gethostname(), "localhost")). On a host named "build" that also rewrites ordinary
 * words ("a build that ..." -> "a localhost that ...") and a correct test fails against an unchanged reference
 * (04881_low_cardinality_default_value_needle). Upstream CI runs in containers whose random host names do not occur
 * in outputs. Unprivileged user/UTS namespaces are not available on this host (AppArmor), so the name is set per
 * process instead. With "localhost" the substitution is a no-op, and host names printed by the server (hostName(),
 * FQDN()) already read "localhost", which is what the references expect.
 *
 * Test-only: loaded solely through LD_PRELOAD, which isolated_ch.sh sets for the processes it starts.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

static const char * shim_name(void)
{
    const char * name = getenv("CH_ISO_HOSTNAME");
    return (name && *name) ? name : "localhost";
}

int gethostname(char * buf, size_t len)
{
    const char * name = shim_name();
    size_t size = strlen(name) + 1;
    if (size > len)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, name, size);
    return 0;
}

int uname(struct utsname * buf)
{
    static int (*real_uname)(struct utsname *) = NULL;
    if (!real_uname)
        real_uname = (int (*)(struct utsname *))dlsym(RTLD_NEXT, "uname");
    if (!real_uname)
    {
        errno = ENOSYS;
        return -1;
    }
    int res = real_uname(buf);
    if (res == 0)
    {
        strncpy(buf->nodename, shim_name(), sizeof(buf->nodename) - 1);
        buf->nodename[sizeof(buf->nodename) - 1] = '\0';
    }
    return res;
}
