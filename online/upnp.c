// Asks the router to forward the UDP port to this PC (UPnP Internet Gateway Device: find it
// with SSDP, read its description, then AddPortMapping), and logs the router's internet
// address, the one a guest elsewhere connects to. Routers with UPnP switched off ignore
// this; the port then has to be forwarded by hand.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"

static int g_port;

// Parses http://host[:port]/path.
static int parse_url(const char *url, char *host, int hostlen, int *port, char *path, int pathlen)
{
    if (_strnicmp(url, "http://", 7)) return 0;
    url += 7;
    const char *end = url + strcspn(url, ":/");
    if (end - url >= hostlen) return 0;
    memcpy(host, url, end - url);
    host[end - url] = 0;
    *port = 80;
    if (*end == ':') *port = atoi(end + 1), end += strcspn(end, "/");
    snprintf(path, pathlen, "%s", *end ? end : "/");
    return 1;
}

// One HTTP request; the whole response (headers and body) into resp. Returns its length.
static int http(const char *host, int port, const char *request, int reqlen, char *resp, int cap)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD timeout = 4000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);
    int len = 0;
    if (!connect(s, (struct sockaddr *)&a, sizeof a) && send(s, request, reqlen, 0) == reqlen) {
        int n;
        while (len < cap - 1 && (n = recv(s, resp + len, cap - 1 - len, 0)) > 0) len += n;
    }
    closesocket(s);
    resp[len] = 0;
    return len;
}

// Copies the text between <tag> and </tag> after `from`; returns where it ended, or NULL.
static const char *xml_value(const char *from, const char *tag, char *out, int outlen)
{
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *a = strstr(from, open);
    if (!a) return NULL;
    a += strlen(open);
    const char *b = strstr(a, close);
    if (!b || b - a >= outlen) return NULL;
    memcpy(out, a, b - a);
    out[b - a] = 0;
    return b;
}

static int soap(const char *host, int port, const char *path, const char *service, const char *action,
                const char *args, char *resp, int cap)
{
    char body[1536], req[2560];
    int blen = snprintf(body, sizeof body,
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s>"
        "</s:Body></s:Envelope>\r\n", action, service, args, action);
    int rlen = snprintf(req, sizeof req,
        "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
        "SOAPAction: \"%s#%s\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
        path, host, port, service, action, blen, body);
    if (!http(host, port, req, rlen, resp, cap)) return 0;
    return atoi(resp + strcspn(resp, " "));   // the HTTP status
}

// SSDP: the first gateway that answers gives the URL of its description. The search goes
// out of the adapter the internet is reached through, not a virtual one (VMs, VPNs).
static int find_gateway(char *location, int len)
{
    struct in_addr me;
    int have_me = host_main_address(&me);
    static const char *targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
    };
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    DWORD timeout = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
    if (have_me) {
        struct sockaddr_in local;
        memset(&local, 0, sizeof local);
        local.sin_family = AF_INET;
        local.sin_addr = me;
        bind(s, (struct sockaddr *)&local, sizeof local);
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&me, sizeof me);
    }
    int found = 0;
    for (int round = 0; round < 3 && !found; round++) {
        for (int i = 0; i < 2; i++) {
            char msg[256];
            int n = snprintf(msg, sizeof msg, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                             "MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n", targets[i]);
            sendto(s, msg, n, 0, (struct sockaddr *)&to, sizeof to);
        }
        char resp[2048];
        int n;
        while (!found && (n = recv(s, resp, sizeof resp - 1, 0)) > 0) {
            resp[n] = 0;
            for (char *line = resp; line && *line; line = strstr(line, "\r\n") ? strstr(line, "\r\n") + 2 : NULL)
                if (!_strnicmp(line, "LOCATION:", 9)) {
                    line += 9;
                    while (*line == ' ') line++;
                    int l = (int)strcspn(line, "\r\n");
                    if (l < len) {
                        memcpy(location, line, l);
                        location[l] = 0;
                        found = 1;
                    }
                    break;
                }
        }
    }
    closesocket(s);
    return found;
}

// Our address on the network the gateway is on.
static int local_address_toward(const char *host, char *ip, int len)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a;
    int alen = sizeof a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(1900);
    inet_pton(AF_INET, host, &a.sin_addr);
    int ok = !connect(s, (struct sockaddr *)&a, sizeof a) && !getsockname(s, (struct sockaddr *)&a, &alen)
             && inet_ntop(AF_INET, &a.sin_addr, ip, len);
    closesocket(s);
    return ok;
}

static int is_private(const char *ip)
{
    int a, b;
    if (sscanf(ip, "%d.%d", &a, &b) != 2) return 1;
    return a == 10 || a == 127 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168)
        || (a == 100 && b >= 64 && b < 128) || a == 0;
}

static int add_mapping(const char *host, int port, const char *path, const char *service, const char *me,
                       int mapped, int lease, char *resp, int cap)
{
    char args[768];
    snprintf(args, sizeof args,
        "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>"
        "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
        "<NewPortMappingDescription>FF2 Remaster online co-op</NewPortMappingDescription>"
        "<NewLeaseDuration>%d</NewLeaseDuration>", mapped, mapped, me, lease);
#ifdef UPNP_DRY_RUN // tests: find the router and ask its address, change nothing
    (void)host, (void)port, (void)path, (void)service, (void)resp, (void)cap;
    host_log("UPnP dry run: would forward %d to %s, lease %d", mapped, me, lease);
    return 200;
#endif
    return soap(host, port, path, service, "AddPortMapping", args, resp, cap);
}

static DWORD WINAPI upnp_thread(void *arg)
{
    (void)arg;
    char location[512], host[64], path[384], ctl_host[64], ctl_path[600], service[128], url[512], me[64];
    int port, ctl_port;
    static char resp[65536];
    if (!find_gateway(location, sizeof location) || !parse_url(location, host, sizeof host, &port, path, sizeof path)) {
        host_log("UPnP: no router answered; forward UDP port %d to this PC by hand to play over the internet", g_port);
        return 0;
    }
    char req[1024];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", path, host, port);
    if (!http(host, port, req, n, resp, sizeof resp)) {
        host_log("UPnP: the router's description at %s didn't load", location);
        return 0;
    }
    // The WAN connection service: IP (cable / fiber) or PPP (DSL), any version.
    const char *at = NULL;
    static const char *kinds[] = { "urn:schemas-upnp-org:service:WANIPConnection:", "urn:schemas-upnp-org:service:WANPPPConnection:" };
    for (int i = 0; i < 2 && !at; i++) at = strstr(resp, kinds[i]);
    int svc_len = at ? (int)strcspn(at, "<") : 0;
    if (!at || svc_len >= (int)sizeof service) {
        host_log("UPnP: the router has no port forwarding service");
        return 0;
    }
    memcpy(service, at, svc_len);
    service[svc_len] = 0;
    if (!xml_value(at, "controlURL", url, sizeof url)) {
        host_log("UPnP: the router's description has no control address");
        return 0;
    }
    if (!_strnicmp(url, "http://", 7)) {
        if (!parse_url(url, ctl_host, sizeof ctl_host, &ctl_port, ctl_path, sizeof ctl_path)) return 0;
    } else {
        snprintf(ctl_host, sizeof ctl_host, "%s", host);
        ctl_port = port;
        snprintf(ctl_path, sizeof ctl_path, "%s%s", url[0] == '/' ? "" : "/", url);
    }
    if (!local_address_toward(host, me, sizeof me)) return 0;

    // A lease of 2 hours, renewed every hour; routers that only keep permanent ones get 0.
    for (int round = 0, permanent = 0;; round++) {
        int status = add_mapping(ctl_host, ctl_port, ctl_path, service, me, g_port, permanent ? 0 : 7200, resp, sizeof resp);
        if (status != 200 && !permanent
            && (status = add_mapping(ctl_host, ctl_port, ctl_path, service, me, g_port, 0, resp, sizeof resp)) == 200)
            permanent = 1;
        // the controls port (proto.h); the stream works without it
        if (status == 200
            && add_mapping(ctl_host, ctl_port, ctl_path, service, me, g_port + 1, permanent ? 0 : 7200, resp, sizeof resp) != 200
            && round == 0)
            host_log("UPnP: the router didn't forward UDP port %d (the guest's controls); forward it by hand if they lag",
                     g_port + 1);
        if (status != 200) {
            char code[16] = "";
            xml_value(resp, "errorCode", code, sizeof code);
            host_log("UPnP: the router refused to forward UDP port %d (HTTP %d, error %s); forward it by hand",
                     g_port, status, *code ? code : "?");
            return 0;
        }
        if (round == 0) {
            char ip[64] = "";
            if (soap(ctl_host, ctl_port, ctl_path, service, "GetExternalIPAddress", "", resp, sizeof resp) == 200)
                xml_value(resp, "NewExternalIPAddress", ip, sizeof ip);
            if (!*ip) host_log("UPnP: router forwards UDP ports %d-%d to %s", g_port, g_port + 1, me);
            else if (is_private(ip))
                host_log("UPnP: router forwards UDP port %d, but its internet address %s is a shared one (CGNAT or a second "
                         "router): guests outside may not get through; a VPN like Tailscale or ZeroTier works", g_port, ip);
            else host_log("UPnP: router forwards UDP ports %d-%d; guests over the internet connect to %s", g_port, g_port + 1, ip);
            if (permanent) host_log("UPnP: the router keeps the port open until it's removed in the router's settings");
        }
        if (permanent) return 0;
#ifdef UPNP_DRY_RUN
        return 0;
#endif
        Sleep(3600 * 1000);
    }
}

void upnp_start(int port)
{
    g_port = port;
    CloseHandle(CreateThread(NULL, 0, upnp_thread, NULL, 0, NULL));
}
