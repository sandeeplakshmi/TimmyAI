# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Weather from Open-Meteo (free, no key). The eyes squint in sun, droop in rain, shiver in snow."""

import json
import urllib.parse
import urllib.request

WMO = {0: "clear", 1: "mostly clear", 2: "partly cloudy", 3: "overcast", 45: "foggy", 48: "foggy",
       51: "light drizzle", 53: "drizzle", 55: "heavy drizzle", 56: "freezing drizzle", 57: "freezing drizzle",
       61: "light rain", 63: "rain", 65: "heavy rain", 66: "freezing rain", 67: "freezing rain",
       71: "light snow", 73: "snow", 75: "heavy snow", 77: "snow", 80: "rain showers", 81: "rain showers",
       82: "heavy showers", 85: "snow showers", 86: "heavy snow showers", 95: "thunderstorms",
       96: "thunderstorms", 99: "thunderstorms"}


def _get(url):
    with urllib.request.urlopen(url, timeout=12) as r:
        return json.load(r)


def geocode(name):
    q = urllib.parse.urlencode({"name": name.strip(" .,"), "count": 1})
    res = (_get("https://geocoding-api.open-meteo.com/v1/search?" + q).get("results") or [None])[0]
    return res and {"city": res["name"], "latitude": res["latitude"], "longitude": res["longitude"]}


def look_for(code, temp_now, units):
    """The eyes for a weather code: sun, rain, snow, storm, cloud."""
    if code in (71, 73, 75, 77, 85, 86) or code in (56, 57, 66, 67):
        return "snow"
    if code >= 95:
        return "storm"
    if code >= 51:
        return "rain"
    if code <= 1:
        return "sun"
    return "cloud"


def fetch(kit, place=None):
    """{'city', 'now', 'temp', 'high', 'low', 'rain', 'code', 'unit'} for the configured or named place."""
    cfg = kit.config
    loc = geocode(place) if place else {"city": cfg.get("city"), "latitude": cfg.get("latitude"),
                                         "longitude": cfg.get("longitude")}
    if not loc or loc.get("latitude") is None:
        return None
    units = cfg.get("units", "fahrenheit")
    q = urllib.parse.urlencode({
        "latitude": loc["latitude"], "longitude": loc["longitude"], "timezone": "auto", "forecast_days": 1,
        "current": "temperature_2m,weather_code",
        "daily": "temperature_2m_max,temperature_2m_min,precipitation_probability_max,weather_code",
        "temperature_unit": units})
    j = _get("https://api.open-meteo.com/v1/forecast?" + q)
    c, d = j["current"], j["daily"]
    return {"city": loc["city"], "code": c["weather_code"], "now": WMO.get(c["weather_code"], "unclear"),
            "temp": round(c["temperature_2m"]), "high": round(d["temperature_2m_max"][0]),
            "low": round(d["temperature_2m_min"][0]), "rain": d["precipitation_probability_max"][0] or 0,
            "today": WMO.get(d["weather_code"][0], "unclear"), "unit": "degrees"}


def sentence(w):
    s = f"It's {w['temp']} and {w['now']} in {w['city']}, with a high of {w['high']} and a low of {w['low']}."
    if w["rain"] >= 30:
        s += f" {w['rain']} percent chance of rain or snow."
    return s


def handle(kit, req):
    city = req.match.get("city")
    if city:
        loc = geocode(city)
        if not loc:
            return f"I couldn't find {city}."
        for k, v in loc.items():
            kit.set_config(k, v)
        return f"Okay, I'll give you the weather for {loc['city']}."
    try:
        w = fetch(kit, req.match.get("place"))
    except Exception:
        return "I couldn't get the weather right now."
    if not w:
        if not req.match.get("place"):
            return "Tell me your city first: say set my city to, then the name."
        return f"I couldn't find {req.match.get('place')}."
    kit.eyes(look_for(w["code"], w["temp"], None), secs=12)
    return sentence(w)
