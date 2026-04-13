/*
 * frank-netcard: ESP-01 AT Modem Firmware
 *
 * Turns an ESP-01 (ESP8266) module into a UART-controlled network modem.
 * A host MCU (e.g. RP2350) sends AT commands over UART to manage WiFi
 * connections and TCP/TLS/UDP sockets.
 *
 * See README.md for the full command reference.
 */

#include <ESP8266WiFi.h>
#include <ESP8266Ping.h>
#include <WiFiClientSecureBearSSL.h>
#include <WiFiUdp.h>

// ── Configuration ────────────────────────────────────────────────

#define FW_NAME          "frank-netcard"
#define FW_VERSION       "1.01"

#define MAX_SOCKETS      4
#define CMD_BUF_SIZE     512
#define DATA_BUF_SIZE    1024
#define DEFAULT_BAUD     115200

#define WIFI_TIMEOUT_MS  15000
#define CONN_TIMEOUT_MS  20000
#define DNS_TIMEOUT_MS   5000
#define SEND_TIMEOUT_MS  10000

// ── Types ────────────────────────────────────────────────────────

enum SockType { SOCK_NONE = 0, SOCK_TCP, SOCK_TLS, SOCK_UDP };

struct Socket {
    SockType  type        = SOCK_NONE;
    WiFiClient              *tcp = nullptr;
    BearSSL::WiFiClientSecure *tls = nullptr;
    WiFiUDP                 *udp = nullptr;
    String    remoteHost;
    uint16_t  remotePort  = 0;
    bool      prevAlive   = false;   // tracks connection for close detection

    void close() {
        if (tcp) { tcp->stop(); delete tcp; tcp = nullptr; }
        if (tls) { tls->stop(); delete tls; tls = nullptr; }
        if (udp) { udp->stop(); delete udp; udp = nullptr; }
        type       = SOCK_NONE;
        remoteHost = "";
        remotePort = 0;
        prevAlive  = false;
    }

    bool connected() const {
        switch (type) {
            case SOCK_TCP: return tcp && tcp->connected();
            case SOCK_TLS: return tls && tls->connected();
            case SOCK_UDP: return udp != nullptr;
            default:       return false;
        }
    }

    int avail() const {
        switch (type) {
            case SOCK_TCP: return tcp ? tcp->available() : 0;
            case SOCK_TLS: return tls ? tls->available() : 0;
            default:       return 0;
        }
    }

    int readBuf(uint8_t *buf, size_t len) {
        switch (type) {
            case SOCK_TCP: return tcp ? tcp->read(buf, len) : -1;
            case SOCK_TLS: return tls ? tls->read(buf, len) : -1;
            case SOCK_UDP: return udp ? udp->read(buf, len) : -1;
            default:       return -1;
        }
    }

    size_t writeBuf(const uint8_t *buf, size_t len) {
        size_t w;
        switch (type) {
            case SOCK_TCP:
                if (!tcp) return 0;
                w = tcp->write(buf, len);
                tcp->flush();
                return w;
            case SOCK_TLS:
                if (!tls) return 0;
                w = tls->write(buf, len);
                tls->flush();
                return w;
            case SOCK_UDP:
                if (!udp) return 0;
                udp->beginPacket(remoteHost.c_str(), remotePort);
                w = udp->write(buf, len);
                udp->endPacket();
                return w;
            default: return 0;
        }
    }
};

// ── Globals ──────────────────────────────────────────────────────

static Socket   sockets[MAX_SOCKETS];
static char     cmdBuf[CMD_BUF_SIZE];
static uint16_t cmdLen = 0;
static uint8_t  dataBuf[DATA_BUF_SIZE];

// Parser state machine (command mode vs binary-send mode)
enum PState { PS_CMD, PS_DATA };
static PState       pState       = PS_CMD;
static int          sendId       = -1;
static int          sendTotal    = 0;
static int          sendGot      = 0;
static unsigned long sendStart   = 0;

// WiFi state tracking
static bool wifiPrev = false;

// ── Output Helpers ───────────────────────────────────────────────

static void respond(const char *s)        { Serial.print(s); Serial.print("\r\n"); }
static void respond(const String &s)      { Serial.print(s); Serial.print("\r\n"); }
static void respondOK()                   { respond("OK"); }
static void respondErr(const char *msg)   { Serial.print("ERROR:"); respond(msg); }
static void respondErr(const String &msg) { Serial.print("ERROR:"); respond(msg); }

// ── WiFi Command Handlers ────────────────────────────────────────

static void cmdWSCAN() {
    int n = WiFi.scanNetworks();
    if (n < 0) { respondErr("SCAN_FAILED"); return; }
    for (int i = 0; i < n; i++) {
        Serial.print("+WSCAN:");
        Serial.print(WiFi.SSID(i));
        Serial.print(",");
        Serial.print(WiFi.RSSI(i));
        Serial.print(",");
        Serial.print(WiFi.encryptionType(i));
        Serial.print(",");
        Serial.print(WiFi.channel(i));
        Serial.print("\r\n");
    }
    WiFi.scanDelete();
    respondOK();
}

static void cmdWJOIN(const String &args) {
    int sep = args.indexOf(',');
    if (sep < 0) { respondErr("USAGE:ssid,password"); return; }

    String ssid = args.substring(0, sep);
    String pass = args.substring(sep + 1);

    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
        delay(100);
        yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("+WJOIN:");
        Serial.print(WiFi.localIP().toString());
        Serial.print(",");
        Serial.print(WiFi.subnetMask().toString());
        Serial.print(",");
        Serial.print(WiFi.gatewayIP().toString());
        Serial.print("\r\n");
        wifiPrev = true;
        respondOK();
    } else {
        WiFi.disconnect();
        respondErr("CONNECT_FAILED");
    }
}

static void cmdWQUIT() {
    for (int i = 0; i < MAX_SOCKETS; i++) sockets[i].close();
    WiFi.disconnect();
    wifiPrev = false;
    respondOK();
}

static void cmdWSTAT() {
    Serial.print("+WSTAT:");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("CONNECTED,");
        Serial.print(WiFi.SSID());
        Serial.print(",");
        Serial.print(WiFi.localIP().toString());
        Serial.print(",");
        Serial.print(WiFi.subnetMask().toString());
        Serial.print(",");
        Serial.print(WiFi.gatewayIP().toString());
        Serial.print(",");
        Serial.print(WiFi.dnsIP().toString());
        Serial.print(",");
        Serial.print(WiFi.RSSI());
    } else {
        Serial.print("DISCONNECTED");
    }
    Serial.print("\r\n");
    respondOK();
}

// ── Network Command Handlers ─────────────────────────────────────

// AT+PING=host[,count]
// Sends ICMP pings and returns per-ping results + summary.
// Response: +PING:seq,time_ms  (per ping)
//           +PINGSTAT:sent,received,avg_ms  (summary)
//           OK / ERROR:reason
static void cmdPING(const String &args) {
    if (args.length() == 0) { respondErr("USAGE:host[,count]"); return; }
    if (WiFi.status() != WL_CONNECTED) { respondErr("NO_WIFI"); return; }

    String host;
    int count = 4;

    int comma = args.indexOf(',');
    if (comma >= 0) {
        host = args.substring(0, comma);
        count = args.substring(comma + 1).toInt();
        if (count < 1) count = 1;
        if (count > 10) count = 10;
    } else {
        host = args;
    }

    IPAddress ip;
    if (!WiFi.hostByName(host.c_str(), ip, DNS_TIMEOUT_MS)) {
        respondErr("DNS_FAILED");
        return;
    }

    int sent = 0, received = 0;
    long total_ms = 0;

    for (int i = 0; i < count; i++) {
        sent++;
        if (Ping.ping(ip, 1)) {
            int t = (int)Ping.averageTime();
            received++;
            total_ms += t;
            Serial.print("+PING:");
            Serial.print(i + 1);
            Serial.print(",");
            Serial.print(t);
            Serial.print("\r\n");
        } else {
            Serial.print("+PING:");
            Serial.print(i + 1);
            Serial.print(",-1\r\n");
        }
    }

    int avg = received > 0 ? (int)(total_ms / received) : 0;
    Serial.print("+PINGSTAT:");
    Serial.print(sent);
    Serial.print(",");
    Serial.print(received);
    Serial.print(",");
    Serial.print(avg);
    Serial.print("\r\n");
    respondOK();
}

static void cmdRESOLVE(const String &args) {
    if (args.length() == 0) { respondErr("USAGE:hostname"); return; }
    if (WiFi.status() != WL_CONNECTED) { respondErr("NO_WIFI"); return; }

    IPAddress ip;
    if (WiFi.hostByName(args.c_str(), ip, DNS_TIMEOUT_MS)) {
        Serial.print("+RESOLVE:");
        Serial.print(ip.toString());
        Serial.print("\r\n");
        respondOK();
    } else {
        respondErr("DNS_FAILED");
    }
}

// ── Socket Command Handlers ──────────────────────────────────────

static void cmdSOPEN(const String &args) {
    // Format: id,TYPE,host,port
    int c1 = args.indexOf(',');
    int c2 = (c1 >= 0) ? args.indexOf(',', c1 + 1) : -1;
    int c3 = (c2 >= 0) ? args.indexOf(',', c2 + 1) : -1;
    if (c1 < 0 || c2 < 0 || c3 < 0) {
        respondErr("USAGE:id,TCP|TLS|UDP,host,port");
        return;
    }

    int    id   = args.substring(0, c1).toInt();
    String type = args.substring(c1 + 1, c2);
    String host = args.substring(c2 + 1, c3);
    int    port = args.substring(c3 + 1).toInt();

    if (id < 0 || id >= MAX_SOCKETS)   { respondErr("INVALID_ID"); return; }
    if (sockets[id].type != SOCK_NONE) { respondErr("ALREADY_OPEN"); return; }
    if (WiFi.status() != WL_CONNECTED) { respondErr("NO_WIFI"); return; }
    if (port < 1 || port > 65535)      { respondErr("INVALID_PORT"); return; }

    type.toUpperCase();

    if (type == "TCP") {
        // Pre-resolve DNS so we can report DNS vs TCP failures separately
        IPAddress ip;
        if (!WiFi.hostByName(host.c_str(), ip, DNS_TIMEOUT_MS)) {
            respondErr("DNS_FAILED");
            return;
        }
        WiFiClient *c = new WiFiClient();
        c->setTimeout(CONN_TIMEOUT_MS);
        if (c->connect(ip, port)) {
            sockets[id].tcp        = c;
            sockets[id].type       = SOCK_TCP;
            sockets[id].remoteHost = host;
            sockets[id].remotePort = port;
            sockets[id].prevAlive  = true;
            respondOK();
        } else {
            delete c;
            respondErr("CONNECT_FAILED");
        }
    }
    else if (type == "TLS" || type == "SSL") {
        int freeHeap = ESP.getFreeHeap();
        if (freeHeap < 25000) {
            respondErr("LOW_MEMORY");
            return;
        }
        // Pre-resolve DNS so we can distinguish DNS vs TLS failures
        IPAddress ip;
        if (!WiFi.hostByName(host.c_str(), ip, DNS_TIMEOUT_MS)) {
            respondErr("DNS_FAILED");
            return;
        }
        BearSSL::WiFiClientSecure *c = new BearSSL::WiFiClientSecure();
        c->setInsecure();  // Skip cert verification (ESP-01 RAM constraint)
        // Size RX buffer to available heap; 16384 handles all servers but
        // needs ~40KB total.  Fall back to 4096 on tighter heaps.
        int rxBuf = (freeHeap > 48000) ? 16384 : 4096;
        c->setBufferSizes(rxBuf, 512);
        c->setTimeout(CONN_TIMEOUT_MS);
        // Use hostname (not IP) so BearSSL sends the SNI extension
        if (c->connect(host.c_str(), port)) {
            sockets[id].tls        = c;
            sockets[id].type       = SOCK_TLS;
            sockets[id].remoteHost = host;
            sockets[id].remotePort = port;
            sockets[id].prevAlive  = true;
            respondOK();
        } else {
            int sslErr = c->getLastSSLError();
            delete c;
            String msg = "CONNECT_FAILED";
            if (sslErr != 0) {
                msg += ":ssl=";
                msg += String(sslErr);
            }
            respondErr(msg);
        }
    }
    else if (type == "UDP") {
        WiFiUDP *u = new WiFiUDP();
        if (u->begin(0)) {  // bind to any available local port
            sockets[id].udp        = u;
            sockets[id].type       = SOCK_UDP;
            sockets[id].remoteHost = host;
            sockets[id].remotePort = port;
            sockets[id].prevAlive  = true;
            respondOK();
        } else {
            delete u;
            respondErr("BIND_FAILED");
        }
    }
    else {
        respondErr("INVALID_TYPE:use TCP, TLS, or UDP");
    }
}

static void cmdSSEND(const String &args) {
    int sep = args.indexOf(',');
    if (sep < 0) { respondErr("USAGE:id,length"); return; }

    int id  = args.substring(0, sep).toInt();
    int len = args.substring(sep + 1).toInt();

    if (id < 0 || id >= MAX_SOCKETS)           { respondErr("INVALID_ID"); return; }
    if (sockets[id].type == SOCK_NONE)          { respondErr("NOT_OPEN"); return; }
    if (!sockets[id].connected())               { respondErr("NOT_CONNECTED"); return; }
    if (len <= 0 || len > (int)DATA_BUF_SIZE)   { respondErr("INVALID_LEN"); return; }

    // Switch to binary receive mode
    respond(">");
    pState    = PS_DATA;
    sendId    = id;
    sendTotal = len;
    sendGot   = 0;
    sendStart = millis();

    // Drain any trailing \r or \n left from the command line terminator.
    // Without this, the \n of the \r\n pair gets mixed into the payload.
    while (Serial.available()) {
        char c = Serial.peek();
        if (c == '\r' || c == '\n') {
            Serial.read();
        } else {
            break;
        }
    }
}

static void cmdSCLOSE(const String &args) {
    int id = args.toInt();
    if (id < 0 || id >= MAX_SOCKETS)   { respondErr("INVALID_ID"); return; }
    if (sockets[id].type == SOCK_NONE) { respondErr("NOT_OPEN"); return; }
    sockets[id].close();
    respondOK();
}

static void cmdSSTAT() {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].type == SOCK_NONE) continue;
        Serial.print("+SSTAT:");
        Serial.print(i);
        Serial.print(",");
        switch (sockets[i].type) {
            case SOCK_TCP: Serial.print("TCP"); break;
            case SOCK_TLS: Serial.print("TLS"); break;
            case SOCK_UDP: Serial.print("UDP"); break;
            default: break;
        }
        Serial.print(",");
        Serial.print(sockets[i].connected() ? "CONNECTED" : "CLOSED");
        Serial.print(",");
        Serial.print(sockets[i].remoteHost);
        Serial.print(",");
        Serial.print(sockets[i].remotePort);
        Serial.print("\r\n");
    }
    respondOK();
}

// ── System Command Handlers ──────────────────────────────────────

static void cmdRST() {
    respondOK();
    Serial.flush();
    delay(100);
    ESP.restart();
}

static void cmdVER() {
    Serial.print("+VER:");
    Serial.print(FW_NAME);
    Serial.print(",");
    Serial.print(FW_VERSION);
    Serial.print("\r\n");
    respondOK();
}

static void cmdHEAP() {
    Serial.print("+HEAP:");
    Serial.print(ESP.getFreeHeap());
    Serial.print("\r\n");
    respondOK();
}

static void cmdBAUD(const String &args) {
    long baud = args.toInt();
    if (baud < 9600 || baud > 3000000) { respondErr("INVALID_BAUD"); return; }
    respondOK();
    Serial.flush();
    delay(50);
    Serial.begin(baud);
}

// ── Command Dispatcher ───────────────────────────────────────────

static void dispatch(const char *raw) {
    String s(raw);
    s.trim();
    if (s.length() == 0) return;

    if (!s.startsWith("AT")) { respondErr("UNKNOWN"); return; }
    if (s == "AT")           { respondOK(); return; }
    if (!s.startsWith("AT+")){ respondErr("UNKNOWN"); return; }

    String rest = s.substring(3);
    int eq = rest.indexOf('=');
    String cmd, args;
    if (eq >= 0) {
        cmd  = rest.substring(0, eq);
        args = rest.substring(eq + 1);
    } else {
        cmd = rest;
    }
    cmd.toUpperCase();

    // System
    if      (cmd == "RST")     cmdRST();
    else if (cmd == "VER")     cmdVER();
    else if (cmd == "HEAP")    cmdHEAP();
    else if (cmd == "BAUD")    cmdBAUD(args);
    // WiFi
    else if (cmd == "WSCAN")   cmdWSCAN();
    else if (cmd == "WJOIN")   cmdWJOIN(args);
    else if (cmd == "WQUIT")   cmdWQUIT();
    else if (cmd == "WSTAT")   cmdWSTAT();
    // Network
    else if (cmd == "RESOLVE") cmdRESOLVE(args);
    else if (cmd == "PING")    cmdPING(args);
    // Sockets
    else if (cmd == "SOPEN")   cmdSOPEN(args);
    else if (cmd == "SSEND")   cmdSSEND(args);
    else if (cmd == "SCLOSE")  cmdSCLOSE(args);
    else if (cmd == "SSTAT")   cmdSSTAT();
    else respondErr("UNKNOWN_CMD");
}

// ── Async Polling ────────────────────────────────────────────────

static void pollSockets() {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].type == SOCK_NONE) continue;

        // ── UDP ──
        if (sockets[i].type == SOCK_UDP && sockets[i].udp) {
            int pktSize = sockets[i].udp->parsePacket();
            if (pktSize > 0) {
                int toRead = (pktSize > (int)sizeof(dataBuf))
                           ? (int)sizeof(dataBuf) : pktSize;
                int got = sockets[i].udp->read(dataBuf, toRead);
                if (got > 0) {
                    Serial.print("+SRECV:");
                    Serial.print(i);
                    Serial.print(",");
                    Serial.print(got);
                    Serial.print("\r\n");
                    Serial.write(dataBuf, got);
                }
            }
            continue;
        }

        // ── TCP / TLS ──
        int avail = sockets[i].avail();
        if (avail > 0) {
            int toRead = (avail > (int)sizeof(dataBuf))
                       ? (int)sizeof(dataBuf) : avail;
            int got = sockets[i].readBuf(dataBuf, toRead);
            if (got > 0) {
                Serial.print("+SRECV:");
                Serial.print(i);
                Serial.print(",");
                Serial.print(got);
                Serial.print("\r\n");
                Serial.write(dataBuf, got);
            }
        }

        // Detect remote close (fires only after buffered data is drained)
        if (sockets[i].prevAlive && !sockets[i].connected()
            && sockets[i].avail() == 0) {
            Serial.print("+SCLOSED:");
            Serial.print(i);
            Serial.print("\r\n");
            sockets[i].close();
        }
    }
}

static void pollWiFi() {
    bool now = (WiFi.status() == WL_CONNECTED);
    if (wifiPrev && !now) {
        respond("+WDISCONN");
        wifiPrev = false;
        // Close all sockets — they can't survive a WiFi drop
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (sockets[i].type != SOCK_NONE) {
                Serial.print("+SCLOSED:");
                Serial.print(i);
                Serial.print("\r\n");
                sockets[i].close();
            }
        }
    } else if (!wifiPrev && now) {
        Serial.print("+WCONN:");
        Serial.print(WiFi.localIP().toString());
        Serial.print("\r\n");
        wifiPrev = true;
    }
}

// ── Arduino Entry Points ─────────────────────────────────────────

void setup() {
    Serial.begin(DEFAULT_BAUD);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.setAutoReconnect(true);

    delay(100);
    respond("+READY");
}

void loop() {
    // ── Read from UART ──
    while (Serial.available()) {
        if (pState == PS_CMD) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                if (cmdLen > 0) {
                    cmdBuf[cmdLen] = '\0';
                    dispatch(cmdBuf);
                    cmdLen = 0;
                }
            } else if (cmdLen < CMD_BUF_SIZE - 1) {
                cmdBuf[cmdLen++] = c;
            }
        }
        else {  // PS_DATA — binary receive for AT+SSEND
            int avail = Serial.available();
            int need  = sendTotal - sendGot;
            int chunk = (avail < need) ? avail : need;
            for (int i = 0; i < chunk; i++) {
                dataBuf[sendGot++] = Serial.read();
            }
            if (sendGot >= sendTotal) {
                size_t written = sockets[sendId].writeBuf(dataBuf, sendTotal);
                respond(written == (size_t)sendTotal ? "SEND OK" : "SEND FAIL");
                pState = PS_CMD;
            }
        }
    }

    // ── Send-data timeout guard ──
    if (pState == PS_DATA && millis() - sendStart > SEND_TIMEOUT_MS) {
        respondErr("SEND_TIMEOUT");
        pState = PS_CMD;
    }

    // ── Async events (only in command mode to avoid interleaving
    //    with binary data the host is currently transmitting) ──
    if (pState == PS_CMD) {
        pollSockets();
        pollWiFi();
    }

    yield();
}
