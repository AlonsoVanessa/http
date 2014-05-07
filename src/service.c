/*
    service.c -- Http service. Includes timer for expired requests.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Locals ************************************/
/**
    Standard HTTP error code table
 */
typedef struct HttpStatusCode {
    int     code;                           /**< Http error code */
    char    *codeString;                    /**< Code as a string (for hashing) */
    char    *msg;                           /**< Error message */
} HttpStatusCode;


PUBLIC HttpStatusCode HttpStatusCodes[] = {
    { 100, "100", "Continue" },
    { 101, "101", "Switching Protocols" },
    { 200, "200", "OK" },
    { 201, "201", "Created" },
    { 202, "202", "Accepted" },
    { 204, "204", "No Content" },
    { 205, "205", "Reset Content" },
    { 206, "206", "Partial Content" },
    { 301, "301", "Moved Permanently" },
    { 302, "302", "Moved Temporarily" },
    { 304, "304", "Not Modified" },
    { 305, "305", "Use Proxy" },
    { 307, "307", "Temporary Redirect" },
    { 400, "400", "Bad Request" },
    { 401, "401", "Unauthorized" },
    { 402, "402", "Payment Required" },
    { 403, "403", "Forbidden" },
    { 404, "404", "Not Found" },
    { 405, "405", "Method Not Allowed" },
    { 406, "406", "Not Acceptable" },
    { 408, "408", "Request Timeout" },
    { 409, "409", "Conflict" },
    { 410, "410", "Gone" },
    { 411, "411", "Length Required" },
    { 412, "412", "Precondition Failed" },
    { 413, "413", "Request Entity Too Large" },
    { 414, "414", "Request-URI Too Large" },
    { 415, "415", "Unsupported Media Type" },
    { 416, "416", "Requested Range Not Satisfiable" },
    { 417, "417", "Expectation Failed" },
    { 500, "500", "Internal Server Error" },
    { 501, "501", "Not Implemented" },
    { 502, "502", "Bad Gateway" },
    { 503, "503", "Service Unavailable" },
    { 504, "504", "Gateway Timeout" },
    { 505, "505", "Http Version Not Supported" },
    { 507, "507", "Insufficient Storage" },

    /*
        Proprietary codes (used internally) when connection to client is severed
     */
    { 550, "550", "Comms Error" },
    { 551, "551", "General Client Error" },
    { 0,   0 }
};

/****************************** Forward Declarations **************************/

static void httpTimer(Http *http, MprEvent *event);
static bool isIdle(bool traceRequests);
static void manageHttp(Http *http, int flags);
static void terminateHttp(int state, int how, int status);
static void updateCurrentDate();

/*********************************** Code *************************************/

PUBLIC Http *httpCreate(int flags)
{
    Http            *http;
    HttpStatusCode  *code;

    mprGlobalLock();
    if (MPR->httpService) {
        mprGlobalUnlock();
        return MPR->httpService;
    }
    if ((http = mprAllocObj(Http, manageHttp)) == 0) {
        mprGlobalUnlock();
        return 0;
    }
    MPR->httpService = http;
    http->software = sclone(ME_HTTP_SOFTWARE);
    http->protocol = sclone("HTTP/1.1");
    http->mutex = mprCreateLock();
    http->stages = mprCreateHash(-1, MPR_HASH_STABLE);
    http->hosts = mprCreateList(-1, MPR_LIST_STABLE);
    http->connections = mprCreateList(-1, MPR_LIST_STATIC_VALUES);
    http->authTypes = mprCreateHash(-1, MPR_HASH_CASELESS | MPR_HASH_UNIQUE | MPR_HASH_STABLE);
    http->authStores = mprCreateHash(-1, MPR_HASH_CASELESS | MPR_HASH_UNIQUE | MPR_HASH_STABLE);
    http->routeSets = mprCreateHash(-1, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
    http->booted = mprGetTime();
    http->flags = flags;
    http->monitorMaxPeriod = 0;
    http->monitorMinPeriod = MAXINT;
    http->secret = mprGetRandomString(HTTP_MAX_SECRET);
    http->localPlatform = slower(sfmt("%s-%s-%s", ME_OS, ME_CPU, ME_PROFILE));

    updateCurrentDate();
    http->statusCodes = mprCreateHash(41, MPR_HASH_STATIC_VALUES | MPR_HASH_STATIC_KEYS | MPR_HASH_STABLE);
    for (code = HttpStatusCodes; code->code; code++) {
        mprAddKey(http->statusCodes, code->codeString, code);
    }
    httpGetUserGroup();
    httpInitParser();
    httpInitAuth();
    httpOpenNetConnector(http);
    httpOpenSendConnector(http);
    httpOpenRangeFilter(http);
    httpOpenChunkFilter(http);
#if ME_HTTP_WEB_SOCKETS
    httpOpenWebSockFilter(http);
#endif
    mprSetIdleCallback(isIdle);
    mprAddTerminator(terminateHttp);

    if (flags & HTTP_SERVER_SIDE) {
        http->endpoints = mprCreateList(-1, MPR_LIST_STABLE);
        http->counters = mprCreateList(-1, MPR_LIST_STABLE);
        http->monitors = mprCreateList(-1, MPR_LIST_STABLE);
        http->routeTargets = mprCreateHash(-1, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
        http->routeConditions = mprCreateHash(-1, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
        http->routeUpdates = mprCreateHash(-1, MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
        http->sessionCache = mprCreateCache(MPR_CACHE_SHARED | MPR_HASH_STABLE);
        http->addresses = mprCreateHash(-1, MPR_HASH_STABLE);
        http->defenses = mprCreateHash(-1, MPR_HASH_STABLE);
        http->remedies = mprCreateHash(-1, MPR_HASH_CASELESS | MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
        httpOpenUploadFilter(http);
        httpOpenCacheHandler(http);
        httpOpenPassHandler(http);
        httpOpenActionHandler(http);
        http->serverLimits = httpCreateLimits(1);
        httpDefineRouteBuiltins();
        httpAddCounters();
        httpAddRemedies();
    }
    if (flags & HTTP_CLIENT_SIDE) {
        http->defaultClientHost = sclone("127.0.0.1");
        http->defaultClientPort = 80;
        http->clientLimits = httpCreateLimits(0);
        http->clientRoute = httpCreateConfiguredRoute(0, 0);
        http->clientHandler = httpCreateHandler(http, "client", 0);
    }
    mprGlobalUnlock();
    return http;
}


static void manageHttp(Http *http, int flags)
{
    HttpConn    *conn;
    int         next;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(http->addresses);
        mprMark(http->authStores);
        mprMark(http->authTypes);
        mprMark(http->clientHandler);
        mprMark(http->clientLimits);
        mprMark(http->clientRoute);
        mprMark(http->connections);
        mprMark(http->context);
        mprMark(http->counters);
        mprMark(http->currentDate);
        mprMark(http->dateCache);
        mprMark(http->defaultClientHost);
        mprMark(http->defenses);
        mprMark(http->endpoints);
        mprMark(http->forkData);
        mprMark(http->group);
        mprMark(http->hosts);
        mprMark(http->localPlatform);
        mprMark(http->monitors);
        mprMark(http->mutex);
        mprMark(http->parsers);
        mprMark(http->platform);
        mprMark(http->platformDir);
        mprMark(http->protocol);
        mprMark(http->proxyHost);
        mprMark(http->remedies);
        mprMark(http->routeConditions);
        mprMark(http->routeSets);
        mprMark(http->routeTargets);
        mprMark(http->routeUpdates);
        mprMark(http->secret);
        mprMark(http->serverLimits);
        mprMark(http->sessionCache);
        mprMark(http->software);
        mprMark(http->stages);
        mprMark(http->statusCodes);
        mprMark(http->timer);
        mprMark(http->timestamp);
        mprMark(http->user);

        /*
            Endpoints keep connections alive until a timeout. Keep marking even if no other references.
         */
        lock(http->connections);
        for (next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; ) {
            if (httpServerConn(conn)) {
                mprMark(conn);
            }
        }
        unlock(http->connections);
    }
}


PUBLIC int httpStartEndpoints()
{
    HttpEndpoint    *endpoint;
    Http            *http;
    int             next;

    if ((http = MPR->httpService) == 0) {
        return MPR_ERR_BAD_STATE;
    }
    for (ITERATE_ITEMS(http->endpoints, endpoint, next)) {
        if (httpStartEndpoint(endpoint) < 0) {
            return MPR_ERR_CANT_OPEN;
        }
    }
    return 0;
}


PUBLIC void httpStopEndpoints()
{
    HttpEndpoint    *endpoint;
    Http            *http;
    int             next;

    if ((http = MPR->httpService) == 0) {
        return;
    }
    lock(http->connections);
    for (next = 0; (endpoint = mprGetNextItem(http->endpoints, &next)) != 0; ) {
        httpStopEndpoint(endpoint);
    }
    unlock(http->connections);
}


/*
    Called to close all connections owned by a service (e.g. ejs)
 */
PUBLIC void httpStopConnections(void *data)
{
    Http        *http;
    HttpConn    *conn;
    int         next;

    if ((http = MPR->httpService) == 0) {
        return;
    }
    lock(http->connections);
    for (next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; ) {
        if (data == 0 || conn->data == data) {
            httpDestroyConn(conn);
        }
    }
    unlock(http->connections);
}


/*
    Destroy the http service. This should be called only after ensuring all running requests have completed.
    Normally invoked by the http terminator from mprDestroy
 */
PUBLIC void httpDestroy() 
{
    Http            *http;

    if ((http = MPR->httpService) == 0) {
        return;
    }
    httpStopConnections(0);
    httpStopEndpoints();

    if (http->timer) {
        mprRemoveEvent(http->timer);
        http->timer = 0;
    }
    if (http->timestamp) {
        mprRemoveEvent(http->timestamp);
        http->timestamp = 0;
    }
    MPR->httpService = NULL;
}


/*
    Http terminator called from mprDestroy
 */
static void terminateHttp(int state, int how, int status)
{
    if (state >= MPR_STOPPED) {
        httpDestroy();
    }
}


/*
    Test if the http service (including MPR) is idle with no running requests
 */
static bool isIdle(bool traceRequests)
{
    HttpConn        *conn;
    Http            *http;
    MprTicks        now;
    int             next;
    static MprTicks lastTrace = 0;

    if ((http = MPR->httpService) != 0) {
        now = http->now;
        lock(http->connections);
        for (next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; ) {
            if (conn->state != HTTP_STATE_BEGIN && conn->state != HTTP_STATE_COMPLETE) {
                if (traceRequests && lastTrace < now) {
                    if (conn->rx) {
                        mprLog(2, "http: Request for \"%s\" is still active", conn->rx->uri ? conn->rx->uri : conn->rx->pathInfo);
                    }
                    lastTrace = now;
                }
                unlock(http->connections);
                return 0;
            }
        }
        unlock(http->connections);
    } else {
        now = mprGetTicks();
    }
    return mprServicesAreIdle(traceRequests);
}


PUBLIC void httpAddEndpoint(Http *http, HttpEndpoint *endpoint)
{
    mprAddItem(http->endpoints, endpoint);
}


PUBLIC void httpRemoveEndpoint(Http *http, HttpEndpoint *endpoint)
{
    if (http) {
        mprRemoveItem(http->endpoints, endpoint);
    }
}


/*
    Lookup a host address. If ipAddr is null or port is -1, then those elements are wild.
 */
PUBLIC HttpEndpoint *httpLookupEndpoint(Http *http, cchar *ip, int port)
{
    HttpEndpoint    *endpoint;
    int             next;

    if (ip == 0) {
        ip = "";
    }
    for (next = 0; (endpoint = mprGetNextItem(http->endpoints, &next)) != 0; ) {
        if (endpoint->port <= 0 || port <= 0 || endpoint->port == port) {
            assert(endpoint->ip);
            if (*endpoint->ip == '\0' || *ip == '\0' || scmp(endpoint->ip, ip) == 0) {
                return endpoint;
            }
        }
    }
    return 0;
}


PUBLIC HttpEndpoint *httpGetFirstEndpoint(Http *http)
{
    return mprGetFirstItem(http->endpoints);
}


/*
    WARNING: this should not be called by users as httpCreateHost will automatically call this.
 */
PUBLIC void httpAddHost(Http *http, HttpHost *host)
{
    mprAddItem(http->hosts, host);
}


PUBLIC void httpRemoveHost(Http *http, HttpHost *host)
{
    if (http) {
        mprRemoveItem(http->hosts, host);
    }
}


PUBLIC HttpHost *httpLookupHost(Http *http, cchar *name)
{
    HttpHost    *host;
    int         next;

    for (next = 0; (host = mprGetNextItem(http->hosts, &next)) != 0; ) {
        if (smatch(name, host->name)) {
            return host;
        }
    }
    return 0;
}


PUBLIC void httpInitLimits(HttpLimits *limits, bool serverSide)
{
    memset(limits, 0, sizeof(HttpLimits));
    limits->bufferSize = ME_MAX_QBUFFER;
    limits->cacheItemSize = ME_MAX_CACHE_ITEM;
    limits->chunkSize = ME_MAX_CHUNK;
    limits->clientMax = ME_MAX_CLIENTS;
    limits->connectionsMax = ME_MAX_CONNECTIONS;
    limits->headerMax = ME_MAX_NUM_HEADERS;
    limits->headerSize = ME_MAX_HEADERS;
    limits->keepAliveMax = ME_MAX_KEEP_ALIVE;
    limits->processMax = ME_MAX_PROCESSES;
    limits->requestsPerClientMax = ME_MAX_REQUESTS_PER_CLIENT;
    limits->sessionMax = ME_MAX_SESSIONS;
    limits->uriSize = ME_MAX_URI;

    limits->inactivityTimeout = ME_MAX_INACTIVITY_DURATION;
    limits->requestTimeout = ME_MAX_REQUEST_DURATION;
    limits->requestParseTimeout = ME_MAX_PARSE_DURATION;
    limits->sessionTimeout = ME_MAX_SESSION_DURATION;

    limits->webSocketsMax = ME_MAX_WSS_SOCKETS;
    limits->webSocketsMessageSize = ME_MAX_WSS_MESSAGE;
    limits->webSocketsFrameSize = ME_MAX_WSS_FRAME;
    limits->webSocketsPacketSize = ME_MAX_WSS_PACKET;
    limits->webSocketsPing = ME_MAX_PING_DURATION;

    if (serverSide) {
        limits->receiveFormSize = ME_MAX_RECEIVE_FORM;
        limits->receiveBodySize = ME_MAX_RECEIVE_BODY;
        limits->transmissionBodySize = ME_MAX_TX_BODY;
        limits->uploadSize = ME_MAX_UPLOAD;
    } else {
        limits->receiveFormSize = MAXOFF;
        limits->receiveBodySize = MAXOFF;
        limits->transmissionBodySize = MAXOFF;
        limits->uploadSize = MAXOFF;
    }

#if KEEP
    mprSetMaxSocketClients(endpoint, atoi(value));

    if (scaselesscmp(key, "LimitClients") == 0) {
        mprSetMaxSocketClients(endpoint, atoi(value));
        return 1;
    }
    if (scaselesscmp(key, "LimitMemoryMax") == 0) {
        mprSetAllocLimits(endpoint, -1, atoi(value));
        return 1;
    }
    if (scaselesscmp(key, "LimitMemoryRedline") == 0) {
        mprSetAllocLimits(endpoint, atoi(value), -1);
        return 1;
    }
#endif
}


PUBLIC HttpLimits *httpCreateLimits(int serverSide)
{
    HttpLimits  *limits;

    if ((limits = mprAllocStruct(HttpLimits)) != 0) {
        httpInitLimits(limits, serverSide);
    }
    return limits;
}


PUBLIC void httpEaseLimits(HttpLimits *limits)
{
    limits->receiveFormSize = MAXOFF;
    limits->receiveBodySize = MAXOFF;
    limits->transmissionBodySize = MAXOFF;
    limits->uploadSize = MAXOFF;
}


PUBLIC void httpAddStage(Http *http, HttpStage *stage)
{
    mprAddKey(http->stages, stage->name, stage);
}


PUBLIC HttpStage *httpLookupStage(Http *http, cchar *name)
{
    if (!http) {
        return 0;
    }
    return mprLookupKey(http->stages, name);
}


PUBLIC void *httpLookupStageData(Http *http, cchar *name)
{
    HttpStage   *stage;

    if (!http) {
        return 0;
    }
    if ((stage = mprLookupKey(http->stages, name)) != 0) {
        return stage->stageData;
    }
    return 0;
}


PUBLIC cchar *httpLookupStatus(Http *http, int status)
{
    HttpStatusCode  *ep;
    char            *key;

    key = itos(status);
    ep = (HttpStatusCode*) mprLookupKey(http->statusCodes, key);
    if (ep == 0) {
        return "Custom error";
    }
    return ep->msg;
}


PUBLIC void httpSetForkCallback(Http *http, MprForkCallback callback, void *data)
{
    http->forkCallback = callback;
    http->forkData = data;
}


PUBLIC void httpSetListenCallback(Http *http, HttpListenCallback fn)
{
    http->listenCallback = fn;
}


/*
    The http timer does maintenance activities and will fire per second while there are active requests.
    This routine will also be called by httpTerminate with event == 0 to signify a shutdown.
    NOTE: Because we lock the http here, connections cannot be deleted while we are modifying the list.
 */
static void httpTimer(Http *http, MprEvent *event)
{
    HttpConn    *conn;
    HttpStage   *stage;
    HttpLimits  *limits;
    MprModule   *module;
    int         next, active, abort;

    updateCurrentDate();

    /* 
       Check for any inactive connections or expired requests (inactivityTimeout and requestTimeout)
       OPT - could check for expired connections every 10 seconds.
     */
    lock(http->connections);
    mprTrace(7, "httpTimer: %d active connections", mprGetListLength(http->connections));
    for (active = 0, next = 0; (conn = mprGetNextItem(http->connections, &next)) != 0; active++) {
        limits = conn->limits;
        if (!conn->timeoutEvent) {
            abort = mprIsStopping();
            if (httpServerConn(conn) && (HTTP_STATE_CONNECTED < conn->state && conn->state < HTTP_STATE_PARSED) && 
                    (http->now - conn->started) > limits->requestParseTimeout) {
                conn->timeout = HTTP_PARSE_TIMEOUT;
                abort = 1;
            } else if ((http->now - conn->lastActivity) > limits->inactivityTimeout) {
                conn->timeout = HTTP_INACTIVITY_TIMEOUT;
                abort = 1;
            } else if ((http->now - conn->started) > limits->requestTimeout) {
                conn->timeout = HTTP_REQUEST_TIMEOUT;
                abort = 1;
            } else if (!event) {
                /* Called directly from httpStop to stop connections */
                if (MPR->exitTimeout > 0) {
                    if (conn->state == HTTP_STATE_COMPLETE || 
                        (HTTP_STATE_CONNECTED < conn->state && conn->state < HTTP_STATE_PARSED)) {
                        abort = 1;
                    }
                } else {
                    abort = 1;
                }
            }
            if (abort && !mprGetDebugMode()) {
                httpScheduleConnTimeout(conn);
            }
        }
    }

    /*
        Check for unloadable modules
        OPT - could check for modules every minute
     */
    if (mprGetListLength(http->connections) == 0) {
        for (next = 0; (module = mprGetNextItem(MPR->moduleService->modules, &next)) != 0; ) {
            if (module->timeout) {
                if (module->lastActivity + module->timeout < http->now) {
                    mprLog(2, "Unloading inactive module %s", module->name);
                    if ((stage = httpLookupStage(http, module->name)) != 0) {
                        if (mprUnloadModule(module) < 0)  {
                            active++;
                        } else {
                            stage->flags |= HTTP_STAGE_UNLOADED;
                        }
                    } else {
                        mprUnloadModule(module);
                    }
                } else {
                    active++;
                }
            }
        }
    }
    httpPruneMonitors();

    if (active == 0 || mprIsStopping()) {
        if (event) {
            mprRemoveEvent(event);
        }
        http->timer = 0;
        /*
            Going to sleep now, so schedule a GC to free as much as possible.
         */
        mprGC(MPR_GC_FORCE | MPR_GC_NO_BLOCK);
    } else {
        mprGC(MPR_GC_NO_BLOCK);
    }
    unlock(http->connections);
}


static void timestamp()
{
    mprLog(0, "Time: %s", mprGetDate(NULL));
}


PUBLIC void httpSetTimestamp(MprTicks period)
{
    Http    *http;

    http = MPR->httpService;
    if (period < (10 * MPR_TICKS_PER_SEC)) {
        period = (10 * MPR_TICKS_PER_SEC);
    }
    if (http->timestamp) {
        mprRemoveEvent(http->timestamp);
    }
    if (period > 0) {
        http->timestamp = mprCreateTimerEvent(NULL, "httpTimestamp", period, timestamp, NULL, 
            MPR_EVENT_CONTINUOUS | MPR_EVENT_QUICK);
    }
}


PUBLIC void httpAddConn(Http *http, HttpConn *conn)
{
    http->now = mprGetTicks();
    assert(http->now >= 0);
    conn->started = http->now;
    mprAddItem(http->connections, conn);
    updateCurrentDate();

    lock(http);
    conn->seqno = (int) http->totalConnections++;
    if (!http->timer) {
#if ME_DEBUG
        if (!mprGetDebugMode())
#endif
        {
            http->timer = mprCreateTimerEvent(NULL, "httpTimer", HTTP_TIMER_PERIOD, httpTimer, http, 
                MPR_EVENT_CONTINUOUS | MPR_EVENT_QUICK);
        }
    }
    unlock(http);
}


PUBLIC void httpRemoveConn(Http *http, HttpConn *conn)
{
    if (http) {
        mprRemoveItem(http->connections, conn);
    }
}


PUBLIC char *httpGetDateString(MprPath *sbuf)
{
    MprTicks    when;

    if (sbuf == 0) {
        when = mprGetTime();
    } else {
        when = (MprTicks) sbuf->mtime * MPR_TICKS_PER_SEC;
    }
    return mprFormatUniversalTime(HTTP_DATE_FORMAT, when);
}


PUBLIC void *httpGetContext(Http *http)
{
    return http->context;
}


PUBLIC void httpSetContext(Http *http, void *context)
{
    http->context = context;
}


PUBLIC int httpGetDefaultClientPort(Http *http)
{
    return http->defaultClientPort;
}


PUBLIC cchar *httpGetDefaultClientHost(Http *http)
{
    return http->defaultClientHost;
}


PUBLIC void httpSetDefaultClientPort(Http *http, int port)
{
    http->defaultClientPort = port;
}


PUBLIC void httpSetDefaultClientHost(Http *http, cchar *host)
{
    http->defaultClientHost = sclone(host);
}


PUBLIC void httpSetSoftware(Http *http, cchar *software)
{
    http->software = sclone(software);
}


PUBLIC void httpSetProxy(Http *http, cchar *host, int port)
{
    http->proxyHost = sclone(host);
    http->proxyPort = port;
}


static void updateCurrentDate()
{
    Http        *http;
    MprTicks    diff;

    http = MPR->httpService;
    http->now = mprGetTicks();
    diff = http->now - http->currentTime;
    if (diff <= MPR_TICKS_PER_SEC || diff >= MPR_TICKS_PER_SEC) {
        /*
            Optimize and only update the string date representation once per second
         */
        http->currentTime = http->now;
        http->currentDate = httpGetDateString(NULL);
    }
}


PUBLIC void httpGetStats(HttpStats *sp)
{
    Http                *http;
    HttpAddress         *address;
    MprKey              *kp;
    MprMemStats         *ap;
    MprWorkerStats      wstats;
    ssize               memSessions;

    memset(sp, 0, sizeof(*sp));
    http = MPR->httpService;
    ap = mprGetMemStats();

    sp->cpuUsage = ap->cpuUsage;
    sp->cpuCores = ap->cpuCores;
    sp->ram = ap->ram;
    sp->mem = ap->rss;
    sp->memRedline = ap->warnHeap;
    sp->memMax = ap->maxHeap;

    sp->heap = ap->bytesAllocated;
    sp->heapUsed = ap->bytesAllocated - ap->bytesFree;
    sp->heapPeak = ap->bytesAllocatedPeak;
    sp->heapFree = ap->bytesFree;
    sp->heapRegions = ap->heapRegions;

    mprGetWorkerStats(&wstats);
    sp->workersBusy = wstats.busy;
    sp->workersIdle = wstats.idle;
    sp->workersYielded = wstats.yielded;
    sp->workersMax = wstats.max;

    sp->activeConnections = mprGetListLength(http->connections);
    sp->activeProcesses = http->activeProcesses;

    mprGetCacheStats(http->sessionCache, &sp->activeSessions, &memSessions);
    sp->memSessions = memSessions;

    lock(http->addresses);
    for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
        sp->activeRequests += (int) address->counters[HTTP_COUNTER_ACTIVE_REQUESTS].value;
        sp->activeClients++;
    }
    unlock(http->addresses);

    sp->totalRequests = http->totalRequests;
    sp->totalConnections = http->totalConnections;
    sp->totalSweeps = MPR->heap->stats.sweeps;
}


PUBLIC char *httpStatsReport(int flags)
{
    MprTime             now;
    MprBuf              *buf;
    HttpStats           s;
    double              elapsed;
    static MprTime      lastTime;
    static HttpStats    last;
    double              mb;

    mb = 1024.0 * 1024;
    now = mprGetTime();
    elapsed = (now - lastTime) / 1000.0;
    httpGetStats(&s);
    buf = mprCreateBuf(0, 0);

    mprPutToBuf(buf, "\nHttp Report: at %s\n\n", mprGetDate("%D %T"));
    if (flags & HTTP_STATS_MEMORY) {
        mprPutToBuf(buf, "Memory       %8.1f MB, %5.1f%% max\n", s.mem / mb, s.mem / (double) s.memMax * 100.0);
        mprPutToBuf(buf, "Heap         %8.1f MB, %5.1f%% mem\n", s.heap / mb, s.heap / (double) s.mem * 100.0);
        mprPutToBuf(buf, "Heap-peak    %8.1f MB\n", s.heapPeak / mb);
        mprPutToBuf(buf, "Heap-used    %8.1f MB, %5.1f%% used\n", s.heapUsed / mb, s.heapUsed / (double) s.heap * 100.0);
        mprPutToBuf(buf, "Heap-free    %8.1f MB, %5.1f%% free\n", s.heapFree / mb, s.heapFree / (double) s.heap * 100.0);

        if (s.memMax == (size_t) -1) {
            mprPutToBuf(buf, "Heap limit          -\n");
            mprPutToBuf(buf, "Heap readline       -\n");
        } else {
            mprPutToBuf(buf, "Heap limit   %8.1f MB\n", s.memMax / mb);
            mprPutToBuf(buf, "Heap redline %8.1f MB\n", s.memRedline / mb);
        }
    }

    mprPutToBuf(buf, "Connections  %8.1f per/sec\n", (s.totalConnections - last.totalConnections) / elapsed);
    mprPutToBuf(buf, "Requests     %8.1f per/sec\n", (s.totalRequests - last.totalRequests) / elapsed);
    mprPutToBuf(buf, "Sweeps       %8.1f per/sec\n", (s.totalSweeps - last.totalSweeps) / elapsed);
    mprPutCharToBuf(buf, '\n');

    mprPutToBuf(buf, "Clients      %8d active\n", s.activeClients);
    mprPutToBuf(buf, "Connections  %8d active\n", s.activeConnections);
    mprPutToBuf(buf, "Processes    %8d active\n", s.activeProcesses);
    mprPutToBuf(buf, "Requests     %8d active\n", s.activeRequests);
    mprPutToBuf(buf, "Sessions     %8d active\n", s.activeSessions);
    mprPutToBuf(buf, "Workers      %8d busy - %d yielded, %d idle, %d max\n", 
        s.workersBusy, s.workersYielded, s.workersIdle, s.workersMax);
    mprPutToBuf(buf, "Sessions     %8.1f MB\n", s.memSessions / mb);
    mprPutCharToBuf(buf, '\n');

    last = s;
    lastTime = now;
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}


PUBLIC bool httpConfigure(HttpConfigureProc proc, void *data, MprTicks timeout)
{
    Http        *http;
    MprTicks    mark;

    http = MPR->httpService;
    mark = mprGetTicks();
    if (timeout < 0) {
        timeout = http->serverLimits->requestTimeout;
    } else if (timeout == 0) {
        timeout = MAXINT;
    }
    do {
        lock(http->connections);
        /* Own request will count as 1 */
        if (mprGetListLength(http->connections) == 0) {
            (proc)(data);
            unlock(http->connections);
            return 1;
        }
        unlock(http->connections);
        mprSleep(10);
        /* Defaults to 10 secs */
    } while (mprGetRemainingTicks(mark, timeout) > 0);
    return 0;
}


PUBLIC void httpSetRequestLogCallback(HttpRequestCallback callback)
{
    Http    *http;

    if ((http = MPR->httpService) != 0) {
        http->logCallback = callback;
    }
}


PUBLIC int httpApplyUserGroup() 
{
#if ME_UNIX_LIKE
    Http    *http;

    http = MPR->httpService;
    if (http->userChanged || http->groupChanged) {
        if (!smatch(MPR->logPath, "stdout") && !smatch(MPR->logPath, "stderr")) {
            if (chown(MPR->logPath, http->uid, http->gid) < 0) {
                mprError("Cannot change ownership on %s", MPR->logPath);
            }
        }
    }
    if (httpApplyChangedGroup() < 0 || httpApplyChangedUser() < 0) {
        return MPR_ERR_CANT_COMPLETE;
    }
    if (http->userChanged || http->groupChanged) {
        struct group    *gp;
        gid_t           glist[64], gid;
        MprBuf          *gbuf = mprCreateBuf(0, 0);
        cchar           *groups;
        int             i, ngroup;

        gid = getgid();
        ngroup = getgroups(sizeof(glist) / sizeof(gid_t), glist);
        if (ngroup > 1) {
            mprPutStringToBuf(gbuf, ", groups: ");
            for (i = 0; i < ngroup; i++) {
                if (glist[i] == gid) continue;
                if ((gp = getgrgid(glist[i])) != 0) {
                    mprPutToBuf(gbuf, "%s (%d) ", gp->gr_name, glist[i]);
                } else {
                    mprPutToBuf(gbuf, "(%d) ", glist[i]);
                }
            }
        }
        groups = mprGetBufStart(gbuf);
        mprLog(MPR_INFO, "Running as user \"%s\" (%d), group \"%s\" (%d)%s", http->user, http->uid, 
            http->group, http->gid, groups);
    }
#endif
    return 0;
}


PUBLIC void httpGetUserGroup()
{
#if ME_UNIX_LIKE
    Http            *http;
    struct passwd   *pp;
    struct group    *gp;

    http = MPR->httpService;
    http->uid = getuid();
    if ((pp = getpwuid(http->uid)) == 0) {
        mprError("Cannot read user credentials: %d. Check your /etc/passwd file.", http->uid);
    } else {
        http->user = sclone(pp->pw_name);
    }
    http->gid = getgid();
    if ((gp = getgrgid(http->gid)) == 0) {
        mprError("Cannot read group credentials: %d. Check your /etc/group file", http->gid);
    } else {
        http->group = sclone(gp->gr_name);
    }
#else
    http->uid = http->gid = -1;
#endif
}


PUBLIC int httpSetUserAccount(cchar *newUser)
{
    Http        *http;

    http = MPR->httpService;
    if (smatch(newUser, "HTTP") || smatch(newUser, "APPWEB")) {
#if ME_UNIX_LIKE
        /* Only change user if root */
        if (getuid() != 0) {
            mprLog(2, "Running as user account \"%s\"", http->user);
            return 0;
        }
#endif
#if MACOSX || FREEBSD
        newUser = "_www";
#elif LINUX || ME_UNIX_LIKE
        newUser = "nobody";
#elif WINDOWS
        newUser = "Administrator";
#endif
    }
#if ME_UNIX_LIKE
{
    struct passwd   *pp;
    if (snumber(newUser)) {
        http->uid = atoi(newUser);
        if ((pp = getpwuid(http->uid)) == 0) {
            mprError("Bad user id: %d", http->uid);
            return MPR_ERR_CANT_ACCESS;
        }
        newUser = pp->pw_name;

    } else {
        if ((pp = getpwnam(newUser)) == 0) {
            mprError("Bad user name: %s", newUser);
            return MPR_ERR_CANT_ACCESS;
        }
        http->uid = pp->pw_uid;
    }
    http->userChanged = 1;
}
#endif
    http->user = sclone(newUser);
    return 0;
}


//  TODO - this should be pushed down into http
PUBLIC int httpSetGroupAccount(cchar *newGroup)
{
    Http    *http;

    http = MPR->httpService;
    if (smatch(newGroup, "HTTP") || smatch(newGroup, "APPWEB")) {
#if ME_UNIX_LIKE
        /* Only change group if root */
        if (getuid() != 0) {
            return 0;
        }
#endif
#if MACOSX || FREEBSD
        newGroup = "_www";
#elif LINUX || ME_UNIX_LIKE
{
        char    *buf;
        newGroup = "nobody";
        /*
            Debian has nogroup, Fedora has nobody. Ugh!
         */
        if ((buf = mprReadPathContents("/etc/group", NULL)) != 0) {
            if (scontains(buf, "nogroup:")) {
                newGroup = "nogroup";
            }
        }
}
#elif WINDOWS
        newGroup = "Administrator";
#endif
    }
#if ME_UNIX_LIKE
    struct group    *gp;

    if (snumber(newGroup)) {
        http->gid = atoi(newGroup);
        if ((gp = getgrgid(http->gid)) == 0) {
            mprError("Bad group id: %d", http->gid);
            return MPR_ERR_CANT_ACCESS;
        }
        newGroup = gp->gr_name;

    } else {
        if ((gp = getgrnam(newGroup)) == 0) {
            mprError("Bad group name: %s", newGroup);
            return MPR_ERR_CANT_ACCESS;
        }
        http->gid = gp->gr_gid;
    }
    http->groupChanged = 1;
#endif
    http->group = sclone(newGroup);
    return 0;
}


PUBLIC int httpApplyChangedUser()
{
#if ME_UNIX_LIKE
    Http    *http;

    http = MPR->httpService;
    if (http->userChanged && http->uid >= 0) {
        if (http->gid >= 0 && http->groupChanged) {
            if (setgroups(0, NULL) == -1) {
                mprError("Cannot clear supplemental groups");
            }
            if (setgid(http->gid) == -1) {
                mprError("Cannot change group to %s: %d\n"
                    "WARNING: This is a major security exposure", http->group, http->gid);
            }
        } else {
            struct passwd   *pp;
            if ((pp = getpwuid(http->uid)) == 0) {
                mprError("Cannot get user entry for id: %d", http->uid);
                return MPR_ERR_CANT_ACCESS;
            }
            mprLog(4, "Initgroups for %s GID %d", http->user, pp->pw_gid);
            if (initgroups(http->user, pp->pw_gid) == -1) {
                mprError("Cannot initgroups for %s, errno: %d", http->user, errno);
            }
        }
        if ((setuid(http->uid)) != 0) {
            mprError("Cannot change user to: %s: %d\n"
                "WARNING: This is a major security exposure", http->user, http->uid);
            if (getuid() != 0) {
                mprError("Log in as administrator/root and retry");
            }
            return MPR_ERR_BAD_STATE;
#if LINUX && PR_SET_DUMPABLE
        } else {
            prctl(PR_SET_DUMPABLE, 1);
#endif
        }
    }
#endif
    return 0;
}


PUBLIC int httpApplyChangedGroup()
{
#if ME_UNIX_LIKE
    Http    *http;

    http = MPR->httpService;
    if (http->groupChanged && http->gid >= 0) {
        if (setgid(http->gid) != 0) {
            mprError("Cannot change group to %s: %d\n"
                "WARNING: This is a major security exposure", http->group, http->gid);
            if (getuid() != 0) {
                mprError("Log in as administrator/root and retry");
            }
            return MPR_ERR_BAD_STATE;
#if LINUX && PR_SET_DUMPABLE
        } else {
            prctl(PR_SET_DUMPABLE, 1);
#endif
        }
    }
#endif
    return 0;
}


PUBLIC int httpParsePlatform(cchar *platform, cchar **os, cchar **arch, cchar **profile)
{
    char   *rest;

    if (platform == 0 || *platform == '\0') {
        return MPR_ERR_BAD_ARGS;
    }
    *os = stok(sclone(platform), "-", &rest);
    *arch = sclone(stok(NULL, "-", &rest));
    *profile = sclone(rest);
    if (*os == 0 || *arch == 0 || *profile == 0 || **os == '\0' || **arch == '\0' || **profile == '\0') {
        return MPR_ERR_BAD_ARGS;
    }
    return 0;
}


/*
    Set the platform and platform objects location
    PlatformPath may be a platform spec that must be located, or it may be a complete path to the platform output directory.
    If platformPath is null, the local platform definition is used.
    Probe is the name of the primary executable program in the platform bin directory.
 */
PUBLIC int httpSetPlatform(cchar *platformPath, cchar *probe)
{
    Http            *http;
    MprDirEntry     *dp;
    cchar           *platform, *dir, *junk, *path;
    int             next, i;

    http = MPR->httpService;
    http->platform = http->platformDir = 0;

    if (!platformPath) {
        platformPath = http->localPlatform;
    }
    platform = mprGetPathBase(platformPath);

    if (mprPathExists(mprJoinPath(platformPath, probe), R_OK)) {
        http->platform = platform;
        http->platformDir = sclone(platformPath);

    } else if (smatch(platform, http->localPlatform)) {
        /*
            Check probe with current executable
         */
        path = mprJoinPath(mprGetPathDir(mprGetAppDir()), probe);
        if (mprPathExists(path, R_OK)) {
            http->platform = http->localPlatform;
            http->platformDir = mprGetPathParent(mprGetAppDir());

        } else {
            /*
                Check probe with installed product
             */
            if (mprPathExists(mprJoinPath(ME_VAPP_PREFIX, probe), R_OK)) {
                http->platform = http->localPlatform;
                http->platformDir = sclone(ME_VAPP_PREFIX);
            }
        }
    }
    
    /*
        Last chance. Search up the tree for a similar platform directory.
        This permits specifying a partial platform like "vxworks" without architecture and profile.
     */
    if (!http->platformDir) {
        dir = mprGetCurrentPath();
        for (i = 0; !mprSamePath(dir, "/") && i < 64; i++) {
            for (ITERATE_ITEMS(mprGetPathFiles(dir, 0), dp, next)) {
                if (dp->isDir && sstarts(mprGetPathBase(dp->name), platform)) {
                    path = mprJoinPath(dir, dp->name);
                    if (mprPathExists(mprJoinPath(path, probe), R_OK)) {
                        http->platform = mprGetPathBase(dp->name);
                        http->platformDir = mprJoinPath(dir, dp->name);
                        break;
                    }
                }
            }
            dir = mprGetPathParent(dir);
        }
    }
    if (!http->platform) {
        return MPR_ERR_CANT_FIND;
    }
    if (httpParsePlatform(http->platform, &junk, &junk, &junk) < 0) {
        return MPR_ERR_BAD_ARGS;
    }
    http->platformDir = mprGetAbsPath(http->platformDir);
    mprLog(1, "Using platform %s at \"%s\"", http->platform, http->platformDir);
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
