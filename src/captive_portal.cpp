#include "captive_portal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

extern void webconsole_print(String in);

// -----------------------------------------------------------------------------
// KitzSki Captive Portal
// -----------------------------------------------------------------------------
//
// Login aus dem Firefox-HAR:
//
// GET https://hotspot.hotainment.one/login?username=KitzSki&dst=/?nc=0
//
// Die AGB-Checkbox wird nur per JavaScript im Browser geprüft.
// Für den eigentlichen Login wird keine Checkbox-Variable übertragen.
// -----------------------------------------------------------------------------

namespace {

constexpr uint32_t CHECK_INTERVAL_MS = 10UL * 60UL * 1000UL;  // 10 Minuten

// Nach einem fehlgeschlagenen Login nicht sofort wieder versuchen.
constexpr uint32_t RETRY_INTERVAL_MS = 30UL * 1000UL;          // 30 Sekunden

// HTTP-Timeout bewusst kurz halten, damit der Hauptloop nicht lange hängt.
constexpr uint32_t HTTP_TIMEOUT_MS = 3000;

// Öffentliche Connectivity-Prüfung.
// Bei funktionierendem Internet erwarten wir HTTP 204.
const char* CONNECTIVITY_URL =
    "http://connectivitycheck.gstatic.com/generate_204";

// KitzSki Captive Portal Login.
const char* KITZSKI_LOGIN_URL =
    "https://hotspot.hotainment.one/login?username=KitzSki&dst=%2F%3Fnc%3D0";

uint32_t lastCheckMs = 0;
uint32_t lastRetryMs = 0;

bool internetReady = false;
bool firstCheck = true;

// -----------------------------------------------------------------------------
// Internet testen
// -----------------------------------------------------------------------------

bool checkInternet()
{
    if (WiFi.status() != WL_CONNECTED) {
        internetReady = false;
        return false;
    }

    HTTPClient http;

    http.setTimeout(HTTP_TIMEOUT_MS);

    // KEIN generate_204!
    const char* testUrl =
        "http://www.msftconnecttest.com/connecttest.txt";

    if (!http.begin(testUrl)) {
        webconsole_print("[KitzSki] Internet test: HTTP begin failed");
        internetReady = false;
        return false;
    }

    http.addHeader(
        "User-Agent",
        "Mozilla/5.0 (ESP32-S3; KitzSki)"
    );

    int code = http.GET();

    String response;

    if (code > 0) {
        response = http.getString();
    }

    http.end();

    String msg = "[KitzSki] Connectivity HTTP ";
    msg += String(code);
    webconsole_print(msg);

    // Microsoft Connectivity Check muss genau diesen
    // Inhalt liefern:
    //
    // Microsoft Connect Test
    //

    if (code == 200 &&
        response.indexOf("Microsoft Connect Test") >= 0) {

        webconsole_print(
            "[KitzSki] Echtes Internet erkannt"
        );

        internetReady = true;
        return true;
    }

    // Alles andere bedeutet:
    // Internet nicht sicher verfügbar.
    //
    // Das kann insbesondere das Captive Portal sein.

    if (code > 0) {
        String msg2 = "[KitzSki] Kein echter Internetzugang, HTTP ";
        msg2 += String(code);
        webconsole_print(msg2);
    } else {
        webconsole_print(
            "[KitzSki] Connectivity Test fehlgeschlagen"
        );
    }

    internetReady = false;
    return false;
}

// -----------------------------------------------------------------------------
// KitzSki Login
// -----------------------------------------------------------------------------

bool kitzskiLogin()
{
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    webconsole_print("[KitzSki] Captive Portal erkannt");
    webconsole_print("[KitzSki] Starte automatischen Login...");

    // Das Portal verwendet HTTPS.
    //
    // Der ESP32 kennt das öffentliche Portal-Zertifikat hier nicht fest.
    // Deshalb wird für genau diese Portal-Verbindung keine Zertifikatsprüfung
    // durchgeführt.
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    http.setTimeout(HTTP_TIMEOUT_MS);

    if (!http.begin(client, KITZSKI_LOGIN_URL)) {
        webconsole_print("[KitzSki] Login: HTTP begin failed");
        return false;
    }

    // Browser-ähnliche Header sind für das Portal normalerweise nicht nötig,
    // machen die Anfrage aber etwas näher am aufgezeichneten Browser-Request.
    http.addHeader(
        "User-Agent",
        "Mozilla/5.0 (ESP32-S3; KitzSki captive portal)"
    );

    http.addHeader(
        "Accept",
        "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
    );

    int code = http.GET();

    String response;

    if (code > 0) {
        response = http.getString();
    }

    http.end();

    webconsole_print("[KitzSki] Login HTTP response: %d\n" + String(code));

    if (code != 200) {
        webconsole_print("[KitzSki] Login fehlgeschlagen");
        return false;
    }

    // Der Mitschnitt liefert nach erfolgreichem Login:
    //
    // "You are logged in"
    //
    // Wir prüfen deshalb zusätzlich den Response-Inhalt.
    if (response.indexOf("You are logged in") >= 0) {
        webconsole_print("[KitzSki] Login erfolgreich");
        return true;
    }

    webconsole_print(
        "[KitzSki] HTTP 200 erhalten, aber Erfolgsantwort nicht erkannt"
    );

    return false;
}

} // namespace


// -----------------------------------------------------------------------------
// Öffentliche Funktionen
// -----------------------------------------------------------------------------

void kitzskiPortalTick()
{
    if (WiFi.status() != WL_CONNECTED) {
        internetReady = false;
        return;
    }

    uint32_t now = millis();

    // Direkt nach dem WLAN-Connect einmal prüfen.
    bool due = firstCheck;

    // Danach alle 10 Minuten.
    if (!firstCheck &&
        (now - lastCheckMs >= CHECK_INTERVAL_MS)) {
        due = true;
    }

    // Bei fehlgeschlagenem Login früher erneut versuchen.
    if (!internetReady &&
        lastRetryMs != 0 &&
        (now - lastRetryMs >= RETRY_INTERVAL_MS)) {
        due = true;
    }

    if (!due) {
        return;
    }

    firstCheck = false;
    lastCheckMs = now;

    webconsole_print("[KitzSki] Pruefe Internetverbindung...");

    // ---------------------------------------------------------
    // 1. Internet testen
    // ---------------------------------------------------------

    if (checkInternet()) {
        webconsole_print("[KitzSki] Internet OK - kein Portal-Login noetig");
        lastRetryMs = 0;
        return;
    }

    // ---------------------------------------------------------
    // 2. Kein Internet -> Captive Portal Login
    // ---------------------------------------------------------

    webconsole_print(
        "[KitzSki] Kein freier Internetzugang - versuche Portal-Login"
    );

    lastRetryMs = now;

    if (!kitzskiLogin()) {
        webconsole_print(
            "[KitzSki] Portal-Login fehlgeschlagen - neuer Versuch später"
        );
        internetReady = false;
        return;
    }

    // ---------------------------------------------------------
    // 3. Nach Login erneut testen
    // ---------------------------------------------------------

    delay(2000);

    webconsole_print("[KitzSki] Pruefe Internet nach Login...");

    if (checkInternet()) {
        webconsole_print("[KitzSki] KitzSki WLAN ist jetzt freigeschaltet");
        lastRetryMs = 0;
    } else {
        webconsole_print(
            "[KitzSki] Login wurde akzeptiert, Internet ist aber noch nicht verfuegbar"
        );
        internetReady = false;
    }
}


bool kitzskiInternetReady()
{
    return internetReady;
}