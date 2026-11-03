#!/usr/bin/env python3
"""A stand-in box for working on the web page without the hardware.

Serves components/webui/www/index.html at / and answers the page's JSON-RPC calls (POST /rpc frames and
GET /rpc/<Method>) with sample data. There is no WebSocket, so the page falls back to plain HTTP polling.
Settings calls answer {"ok": true} and change nothing; Deck.ShowMaintenance opens and closes the box menu
per deck in the current state, as the box does. It listens on 127.0.0.1 only.

  python3 tools/mock_box.py 8080 [state]    then open http://127.0.0.1:8080/
  GET /__state/<name>                       switches the state while it runs (then reload the page)

States: satellite (the default), twodecks, dock, nonet, choose, old, docknodeck, mtlong, mtopen, mttwo, looking,
satnodeck, dockpart, problems, limit, limit4, ethboth, wifioff, wifinojoin, wifinotfound.
MOCK_PAGE=<file> serves another copy of the page (a packed one, say).
"""
import copy
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.environ.get("MOCK_PAGE", os.path.join(ROOT, "components", "webui", "www", "index.html"))


def deck(model, serial, hub, menu=False, screen=True, ignored=False):
    return {"device_id": "streamdeck:" + serial, "model": model, "serial": serial, "fw": "1.01.000",
            "high_speed": not hub, "hub_depth": 1 if hub else 0, "port": 2 if hub else 0, "menu": menu, "screen": screen,
            "ignored": ignored}


def base():
    return {
        "name": "CompanionBox-8D1C", "uptime_s": 3600, "mode": "satellite",
        "net": {"route": "wifi", "ip": "192.168.8.225", "eth_addr": "", "sta_addr": "192.168.8.225", "eth_dhcp": True,
                "eth_link": False, "wifi_connected": True, "wifi_enabled": True,
                "sta_net": "main", "sta_ssid": "Studio WiFi", "sta_rssi": -58, "ap_on": True, "ap_forced": False,
                "ap_ssid": "CompanionBox-8D1C", "ap_ip": "192.168.4.1", "ap_clients": 0},
        "companion": {"state": "connected", "version": "5.1.1", "api_version": "1.14.0", "host": "192.168.8.155",
                      "port": 16622, "auto": True, "pinned_id": "x", "selection": "connect", "instances": 1},
        "dock": [], "decks": [deck("Stream Deck Module 32", "AB1CD2345678EF", False)], "problems": [],
        "message": {"active": False},
        "firmware": {"version": "0.2.0", "partition": "ota_0", "pending_verify": False,
                     "project": "companion_box", "build": "Nov  2 2026 00:10:00"},
        "c6": {"running": "3.0.9", "carried": "3.0.9"}, "heap": {}, "maintenance": False, "deckLimit": 2, "decksIgnored": 0,
        "maintenanceDeck": {"serial": "AB1CD2345678EF", "model": "Stream Deck Module 32"},
    }


def states():
    s = {"satellite": base()}
    d = base()
    d.update(mode="dock", dock=[{"dock": 0, "port": 5343, "listening": True, "model": "Stream Deck Module 32",
                                 "serial": "AB1CD2345678EF", "companion": True, "peer": "192.168.8.155"}])
    s["dock"] = d
    n = base()
    n.update(net={"route": "none", "ip": "", "eth_addr": "", "sta_addr": "", "eth_dhcp": True, "eth_link": False,
                  "wifi_connected": False, "wifi_enabled": True,
                  "sta_net": "none", "sta_ssid": "", "sta_rssi": 0, "ap_on": True, "ap_forced": True,
                  "ap_ssid": "CompanionBox-8D1C", "ap_ip": "192.168.4.1", "ap_clients": 1}, decks=[], maintenanceDeck=None)
    s["nonet"] = n
    c = base()
    c["companion"] = {"state": "connecting", "host": "", "port": 16622, "auto": True, "selection": "ambiguous", "instances": 2}
    s["choose"] = c
    o = base()
    o["companion"] = {"state": "too_old", "version": "4.2.1", "api_version": "1.4.0", "host": "192.168.8.40",
                      "port": 16622, "auto": False}
    s["old"] = o
    k = base()
    k.update(mode="dock", decks=[], maintenanceDeck=None, dock=[{"dock": i, "port": 5343 + 2 * i, "listening": False, "model": "",
                                          "serial": "", "companion": False, "peer": ""} for i in range(6)])
    s["docknodeck"] = k
    m = base()  # the longest model name the box knows, its box menu open; a deck without a screen (no control)
    m.update(decks=[deck("Stream Deck Mini Discord Edition", "BL20K1A01234", True, True), deck("Stream Deck Module 32", "AB1CD2345678EF", False),
                    deck("Stream Deck Pedal", "FL31K1A00007", True, False, False)], maintenance=True,
             maintenanceDeck={"serial": "BL20K1A01234", "model": "Stream Deck Mini Discord Edition"})
    s["mtlong"] = m
    o2 = base()
    o2.update(maintenance=True, decks=[deck("Stream Deck Module 32", "AB1CD2345678EF", False, True)])
    s["mtopen"] = o2
    t2 = base()  # open on two of three decks
    t2.update(maintenance=True, decks=[deck("Stream Deck XL", "CL12K1A00042", False, True), deck("Stream Deck Module 32", "AB1CD2345678EF", False, True),
                                       deck("Stream Deck Mini", "BL20K1A05555", True)], maintenanceDeck={"serial": "CL12K1A00042", "model": "Stream Deck XL"})
    s["mttwo"] = t2
    lk = base()  # Satellite, Companion not found yet
    lk["companion"] = {"state": "connecting", "host": "", "port": 16622, "auto": True, "selection": "none", "instances": 0}
    s["looking"] = lk
    tw = base()  # Satellite, connected, two decks
    tw["decks"] = [deck("Stream Deck XL", "CL12K1A00042", False), deck("Stream Deck Module 32", "AB1CD2345678EF", False)]
    s["twodecks"] = tw
    sn = base()  # Satellite, connected, no deck
    sn.update(decks=[], maintenanceDeck=None)
    s["satnodeck"] = sn
    dp = copy.deepcopy(d)  # dock mode, one of two decks added in Companion
    dp["decks"] = [deck("Stream Deck Module 32", "AB1CD2345678EF", False), deck("Stream Deck XL", "CL12K1A00042", False)]
    dp["dock"] = dp["dock"] + [{"dock": 1, "port": 5345, "listening": True, "model": "Stream Deck XL", "serial": "CL12K1A00042",
                                "companion": False, "peer": ""}]
    s["dockpart"] = dp
    pr = base()  # Ready, with two recent deck problems
    pr["problems"] = ["Stream Deck Module 32 stopped responding — unplug and replug it if it doesn't recover",
                      "Unknown Elgato device 0x0099"]
    s["problems"] = pr
    li = base()  # a third deck over the limit of 2, ignored
    li["decks"] = [deck("Stream Deck XL", "CL12K1A00042", False), deck("Stream Deck Module 32", "AB1CD2345678EF", False),
                   deck("Stream Deck Mini", "BL20K1A05555", True, screen=False, ignored=True)]
    li["decksIgnored"] = 1
    li["problems"] = ["Too many Stream Decks (max 2): BL20K1A05555 ignored"]
    s["limit"] = li
    l4 = copy.deepcopy(li)  # ... at 4 (unlocked): a fifth
    l4["deckLimit"] = 4
    l4["decks"] = [deck("Stream Deck XL", "CL12K1A00042", False), deck("Stream Deck Module 32", "AB1CD2345678EF", False),
                   deck("Stream Deck +", "PL31K1A00001", False), deck("Stream Deck MK.2", "MK21K1A00002", True),
                   deck("Stream Deck Mini", "BL20K1A05555", True, screen=False, ignored=True)]
    l4["problems"] = ["Too many Stream Decks (max 4): BL20K1A05555 ignored"]
    s["limit4"] = l4
    # Ethernet routes (static) with the main Wi-Fi joined behind it, the hotspot with 2 devices
    eb = base()
    eb["net"].update(route="eth", ip="192.168.100.200", eth_link=True, eth_addr="192.168.100.200", eth_dhcp=False, ap_clients=2)
    s["ethboth"] = eb
    wo = base()  # Ethernet routes, Wi-Fi off, the hotspot off
    wo["net"].update(route="eth", ip="192.168.100.200", eth_link=True, eth_addr="192.168.100.200", wifi_enabled=False,
                     wifi_connected=False, sta_net="none", sta_ssid="", sta_rssi=0, sta_addr="", ap_on=False, ap_ip="")
    s["wifioff"] = wo
    wn = base()  # Wi-Fi on, not joined; Ethernet linked without an address yet
    wn["net"].update(route="none", ip="", eth_link=True, eth_addr="", wifi_connected=False, sta_net="none", sta_ssid="",
                     sta_rssi=0, sta_addr="")
    s["wifinojoin"] = wn
    nf = copy.deepcopy(wn)  # the network being tried wasn't found
    nf["net"]["sta_not_found"] = True
    s["wifinotfound"] = nf
    return s


STATES = states()
CUR = {"name": sys.argv[2] if len(sys.argv) > 2 else "satellite"}


def answer(method):
    st = STATES[CUR["name"]]
    return {
        "Sys.GetStatus": st,
        "Device.GetInfo": {"id": "companionbox-8d1c", "name": st["name"], "version": st["firmware"]["version"],
                           "model": "ESP32-P4-WIFI6-POE-ETH", "auth_en": False, "lastReset": "power-on"},
        "Auth.GetStatus": {"auth_en": False, "pin": True, "authed": True, "locked_s": 0},
        "Companion.GetConfig": {"companionAuto": st["companion"].get("auto", True), "companionHost": "192.168.8.155",
                                "companionPort": 16622, "companionPinId": "", "mode": st["mode"]},
        "Companion.List": [{"id": "a", "host": "192.168.8.155", "port": 16622, "version": "5.1.1", "pinned": True, "name": "Studio-Mac"},
                           {"id": "b", "host": "192.168.8.40", "port": 16622, "version": "5.0.2", "pinned": False, "name": "FOH-PC"}],
        "Eth.GetConfig": {"ethDhcp": st["net"].get("eth_dhcp", True), "ethIp": "", "ethMask": "", "ethGw": "", "ethDns": ""},
        "Wifi.GetConfig": {"wifiSsid": "Studio WiFi", "wifiHasPass": True, "wifiBackupSsid": "Office 5G",
                           "wifiBackupHasPass": True, "wifiEnabled": st["net"].get("wifi_enabled", True)},
        "Wifi.Scan": {"results": [{"ssid": "Studio WiFi", "rssi": -52, "auth": "WPA2", "channel": 6},
                                  {"ssid": "Office 5G", "rssi": -67, "auth": "WPA2", "channel": 36},
                                  {"ssid": "Cafe", "rssi": -80, "auth": "open", "channel": 1}]},
        "Ap.GetConfig": {"apEnabled": True, "apSecured": True, "apPass": "k7m2p9xqwe", "apSsid": "CompanionBox-8D1C"},
        "Ota.GetStatus": {"state": "idle", "running": "ota_0", "version": st["firmware"]["version"], "next": "ota_1",
                          "pending_verify": False,
                          "sync": {"state": "in_sync", "pct": 100, "other": {"label": "ota_1", "project": "companion_box",
                                   "version": st["firmware"]["version"], "date": "Nov  2 2026", "time": "00:10:00",
                                   "valid": True, "foreign": False}}},
        "Device.GetConfig": {"boxName": st["name"], "mdnsEnabled": True, "deckBrightness": 70, "deckLimit": st.get("deckLimit", 2), "companionAdopt": True},
        "Feedback.GetConfig": {"speakerEnabled": True, "speakerVolume": 50, "soundReady": True, "soundConnected": True,
                               "soundProblem": True, "soundDeckPlug": True, "soundKeyClick": True, "ledEnabled": True,
                               "ledFlash": True, "ledPin": 23, "ledBrightness": 30,
                               "ledPins": [0, 1, 2, 3, 4, 5, 20, 21, 22, 23, 26, 27, 33, 46, 47, 48]},
        "Log.Get": {"seq": 3, "text": "I (1) boot: ready\nI (4659) main: ready in 4659 ms\n"},
    }.get(method, {})


def maint(p):
    """Deck.ShowMaintenance on the current state: a serial opens / closes that deck's menu; none: true opens on the
    first deck with a screen (the box picks the biggest), false closes them all."""
    st = STATES[CUR["name"]]
    decks = [d for d in st["decks"] if d.get("screen")]
    on, ser = bool(p.get("on")), p.get("serial")
    pick = [d for d in decks if d["serial"] == ser] if ser else decks[:1]
    if ser and not pick:
        return {"code": -32000, "message": "no such deck"}
    for d in (pick if (on or ser) else decks):
        d["menu"] = on
    st["maintenance"] = any(d.get("menu") for d in decks)
    r = {"on": st["maintenance"], "open": [d["serial"] for d in decks if d.get("menu")]}
    if on and pick:
        r.update(serial=pick[0]["serial"], model=pick[0]["model"])
    return r


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path.startswith("/__state/"):
            name = self.path.split("/")[-1]
            if name in STATES:
                CUR["name"] = name
                return self.send(200, json.dumps({"state": name}), "application/json")
            return self.send(404, "no such state", "text/plain")
        if self.path.startswith("/rpc/"):
            return self.send(200, json.dumps(copy.deepcopy(answer(self.path[5:].split("?")[0]))), "application/json")
        if self.path in ("/", "/index.html") or self.path.startswith("/?") or self.path.startswith("/#"):
            with open(PAGE, "rb") as f:
                return self.send(200, f.read(), "text/html; charset=utf-8")
        return self.send(404, "not found", "text/plain")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            frame = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return self.send(400, json.dumps({"code": -32700, "message": "bad JSON"}), "application/json")
        m = frame.get("method", "")
        if m == "Deck.ShowMaintenance":
            return self.send(200, json.dumps({"id": frame.get("id"), "result": maint(frame.get("params") or {})}), "application/json")
        result = answer(m)
        if m.endswith(("Set", "SetConfig", "Choose", "Use", "Sync", "BootOther", "Restart", "Identify", "Test",
                       "ShowMessage", "ClearMessage", "ShowMaintenance", "SetAuth", "Login", "Logout")):
            result = {"ok": True}
        return self.send(200, json.dumps({"id": frame.get("id"), "result": result}), "application/json")


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
