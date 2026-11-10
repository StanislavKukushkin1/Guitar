
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <FastLED.h>
#include <mbedtls/md.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>

// =====================================================
// HARDWARE
// =====================================================

#define DATA_PIN 23
#define NUM_LEDS 90
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
#define BRIGHTNESS 10

// =====================================================
// BLE
// =====================================================

#define DEVICE_NAME "GuitarLED"

#define SERVICE_UUID \
    "12345678-1234-1234-1234-1234567890ab"

#define CHARACTERISTIC_UUID \
    "abcdefab-1234-1234-1234-abcdefabcdef"

// Должен совпадать с ключом в GuitarLEDBLEController.cs
static const char* APP_SHARED_SECRET =
    "3b853edd8910e09e44231bf6e6b29c187abcc575de619f6e20af077836d62acb";

static const uint32_t AUTH_TIMEOUT_MS = 8000;
static const uint32_t ADV_RESTART_DELAY_MS = 300;

// =====================================================
// GLOBAL VARIABLES
// =====================================================

CRGB leds[NUM_LEDS];

BLEServer* bleServer = nullptr;
BLECharacteristic* commandCharacteristic = nullptr;
BLEAdvertising* bleAdvertising = nullptr;

volatile bool clientConnected = false;
volatile bool clientAuthenticated = false;
volatile bool advertisingRestartRequested = false;
volatile bool disconnectRequested = false;

volatile uint32_t connectedAtMs = 0;
volatile uint16_t activeConnectionId = 0;

String pendingClientNonce;
String pendingServerNonce;

uint32_t lastDebugTime = 0;

// =====================================================
// DEBUG HELPERS
// =====================================================

void debugHeader(const char* title) {
    Serial.println();
    Serial.println("========================================");
    Serial.println(title);
    Serial.println("========================================");
}

void debugPacket(const String& value) {
    Serial.println("[RX] -------- BLE PACKET --------");

    Serial.printf("[RX] Length: %u bytes\n",
                  (unsigned int)value.length());

    Serial.print("[RX] Text: ");
    Serial.println(value);

    Serial.print("[RX] HEX: ");

    for (size_t i = 0; i < value.length(); i++) {
        Serial.printf("%02X ",
                      (uint8_t)value[i]);
    }

    Serial.println();
    Serial.println("[RX] ----------------------------");
}

void debugResult(const char* result) {
    Serial.print("[RESULT] ");
    Serial.println(result);
}

// =====================================================
// AUTHENTICATION HELPERS
// =====================================================

String toHex(const uint8_t* data, size_t len) {
    static const char hex[] = "0123456789abcdef";

    String result;
    result.reserve(len * 2);

    for (size_t i = 0; i < len; ++i) {
        result += hex[(data[i] >> 4) & 0x0F];
        result += hex[data[i] & 0x0F];
    }

    return result;
}

bool fromHex(
    const String& input,
    uint8_t* output,
    size_t outputLen
) {
    if (input.length() != outputLen * 2) {
        return false;
    }

    for (size_t i = 0; i < outputLen; ++i) {
        char a = input[i * 2];
        char b = input[i * 2 + 1];

        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        int hi = nibble(a);
        int lo = nibble(b);

        if (hi < 0 || lo < 0) {
            return false;
        }

        output[i] = (uint8_t)((hi << 4) | lo);
    }

    return true;
}

String makeNonce() {
    uint8_t bytes[16];

    for (size_t i = 0; i < sizeof(bytes); i += 4) {
        uint32_t value = esp_random();
        memcpy(bytes + i, &value, 4);
    }

    return toHex(bytes, sizeof(bytes));
}

bool calculateHmac(
    const String& clientNonce,
    const String& serverNonce,
    uint8_t output[32]
) {
    String message =
        String(DEVICE_NAME) + "|" +
        clientNonce + "|" +
        serverNonce;

    const mbedtls_md_info_t* info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (info == nullptr) {
        return false;
    }

    int result = mbedtls_md_hmac(
        info,
        reinterpret_cast<const unsigned char*>(APP_SHARED_SECRET),
        strlen(APP_SHARED_SECRET),
        reinterpret_cast<const unsigned char*>(message.c_str()),
        message.length(),
        output
    );

    return result == 0;
}

bool constantTimeEqual(
    const uint8_t* a,
    const uint8_t* b,
    size_t len
) {
    uint8_t diff = 0;

    for (size_t i = 0; i < len; ++i) {
        diff |= a[i] ^ b[i];
    }

    return diff == 0;
}

void clearAuthentication() {
    clientAuthenticated = false;
    pendingClientNonce = "";
    pendingServerNonce = "";
}

// =====================================================
// COMMAND: CLEAR / OFF
// =====================================================

bool commandClear() {
    Serial.println("[CMD] Executing CLEAR");

    FastLED.clear(true);

    Serial.println("[LED] All LEDs OFF");

    return true;
}

// =====================================================
// COMMAND: ON
// =====================================================

bool commandOn() {
    Serial.println("[CMD] Executing ON");

    fill_solid(leds, NUM_LEDS, CRGB::White);
    FastLED.show();

    Serial.printf("[LED] %d LEDs set to WHITE\n", NUM_LEDS);

    return true;
}

// =====================================================
// COMMAND: ALL:R:G:B
// =====================================================

bool commandAll(const String& command) {
    int r, g, b;

    if (sscanf(
        command.c_str(),
        "ALL:%d:%d:%d",
        &r, &g, &b
    ) != 3) {
        Serial.println("[ERROR] ALL: Invalid format");
        return false;
    }

    if (
        r < 0 || r > 255 ||
        g < 0 || g > 255 ||
        b < 0 || b > 255
    ) {
        Serial.println("[ERROR] ALL: RGB out of range");
        return false;
    }

    Serial.printf(
        "[CMD] ALL received: R=%d G=%d B=%d\n",
        r, g, b
    );

    fill_solid(leds, NUM_LEDS, CRGB(r, g, b));
    FastLED.show();

    Serial.printf(
        "[LED] All %d LEDs updated\n",
        NUM_LEDS
    );

    return true;
}

// =====================================================
// COMMAND: LED:INDEX:R:G:B
// =====================================================

bool commandSingleLED(const String& command) {
    int index, r, g, b;

    if (sscanf(
        command.c_str(),
        "LED:%d:%d:%d:%d",
        &index, &r, &g, &b
    ) != 4) {
        Serial.println("[ERROR] LED: Invalid format");
        return false;
    }

    if (index < 0 || index >= NUM_LEDS) {
        Serial.printf(
            "[ERROR] LED index out of range: %d\n",
            index
        );
        return false;
    }

    if (
        r < 0 || r > 255 ||
        g < 0 || g > 255 ||
        b < 0 || b > 255
    ) {
        Serial.println("[ERROR] LED: RGB out of range");
        return false;
    }

    Serial.printf(
        "[CMD] Single LED: index=%d R=%d G=%d B=%d\n",
        index, r, g, b
    );

    leds[index] = CRGB(r, g, b);
    FastLED.show();

    Serial.printf(
        "[LED] LED %d updated successfully\n",
        index
    );

    return true;
}

// =====================================================
// COMMAND: CHORD PACKET
//
// Format:
// C + 6 two-digit frets + 6 finger IDs
//
// Example:
// C030200000000123000
//
// String order:
// 0 = thickest string
// 5 = thinnest string
//
// LED index:
// (fret - 1) * 6 + stringIndex
//
// Finger colors:
// 1 = RED
// 2 = GREEN
// 3 = BLUE
// 4 = YELLOW
// =====================================================

bool commandChord(const String& command) {
    debugHeader("CHORD COMMAND");

    Serial.print("[CHORD] Raw packet: ");
    Serial.println(command);

    Serial.printf(
        "[CHORD] Packet length: %u\n",
        (unsigned int)command.length()
    );

    if (command.length() != 19 || command[0] != 'C') {
        Serial.println("[ERROR] Invalid chord packet length/header");
        return false;
    }

    int frets[6];
    int fingers[6];

    // Проверяем и разбираем значения.
    for (int i = 0; i < 6; ++i) {
        int pos = 1 + i * 2;

        char tens = command[pos];
        char ones = command[pos + 1];
        char finger = command[13 + i];

        if (
            tens < '0' || tens > '9' ||
            ones < '0' || ones > '9'
        ) {
            Serial.printf(
                "[ERROR] Invalid fret digits at string %d\n",
                i
            );
            return false;
        }

        if (finger < '0' || finger > '4') {
            Serial.printf(
                "[ERROR] Invalid finger ID at string %d\n",
                i
            );
            return false;
        }

        frets[i] = (tens - '0') * 10 + (ones - '0');
        fingers[i] = finger - '0';

        if (frets[i] > 15) {
            Serial.printf(
                "[ERROR] Fret out of range at string %d: %d\n",
                i, frets[i]
            );
            return false;
        }
    }

    Serial.println();
    Serial.println("[CHORD] Parsed values:");

    for (int i = 0; i < 6; ++i) {
        Serial.printf(
            "[CHORD] String %d: fret=%02d finger=%d\n",
            i,
            frets[i],
            fingers[i]
        );
    }

    // Сначала очищаем ленту.
    FastLED.clear();

    Serial.println();
    Serial.println("[CHORD] LED mapping:");

    int ledsLit = 0;

    for (int stringIndex = 0; stringIndex < 6; ++stringIndex) {
        int fret = frets[stringIndex];
        int finger = fingers[stringIndex];

        if (fret == 0 || finger == 0) {
            Serial.printf(
                "[CHORD] String %d: skipped (open/muted)\n",
                stringIndex
            );
            continue;
        }

        int ledIndex = (fret - 1) * 6 + stringIndex;

        if (ledIndex < 0 || ledIndex >= NUM_LEDS) {
            Serial.printf(
                "[ERROR] Calculated LED index invalid: %d\n",
                ledIndex
            );
            return false;
        }

        CRGB color;

        switch (finger) {
            case 1:
                color = CRGB::Red;
                break;

            case 2:
                color = CRGB::Green;
                break;

            case 3:
                color = CRGB::Blue;
                break;

            case 4:
                color = CRGB::Yellow;
                break;

            default:
                Serial.println("[ERROR] Unknown finger color");
                return false;
        }

        leds[ledIndex] = color;
        ledsLit++;

        Serial.printf(
            "[CHORD] String %d -> Fret %d -> LED %d -> Finger %d\n",
            stringIndex,
            fret,
            ledIndex,
            finger
        );
    }

    FastLED.show();

    Serial.println();
    Serial.printf(
        "[CHORD] LEDs illuminated: %d\n",
        ledsLit
    );

    Serial.println("[CHORD] Display completed");

    return true;
}

// =====================================================
// COMMAND ROUTER
// =====================================================

bool applyCommand(const String& command) {
    Serial.println();
    Serial.println("[CMD] Parsing command...");

    if (command == "CLEAR" || command == "OFF") {
        return commandClear();
    }

    if (command == "ON") {
        return commandOn();
    }

    if (command.startsWith("ALL:")) {
        return commandAll(command);
    }

    if (command.startsWith("LED:")) {
        return commandSingleLED(command);
    }

    if (command.startsWith("C")) {
        return commandChord(command);
    }

    Serial.println("[ERROR] Unknown command");
    return false;
}

// =====================================================
// BLE SERVER CALLBACKS
// =====================================================

class ServerCallbacks : public BLEServerCallbacks {

    void onConnect(BLEServer* server) override {
        clientConnected = true;
        connectedAtMs = millis();
        disconnectRequested = false;

        clearAuthentication();

        Serial.println();
        Serial.println("[BLE] Central connected");
        Serial.println("[BLE] Waiting for application authentication");
    }

    void onConnect(
        BLEServer* server,
        esp_ble_gatts_cb_param_t* param
    ) override {
        activeConnectionId = param->connect.conn_id;
        onConnect(server);
    }

    void onDisconnect(BLEServer* server) override {
        clientConnected = false;

        clearAuthentication();

        advertisingRestartRequested = true;

        Serial.println();
        Serial.println("[BLE] Central disconnected");
    }

    void onDisconnect(
        BLEServer* server,
        esp_ble_gatts_cb_param_t* param
    ) override {
        onDisconnect(server);
    }
};

// =====================================================
// BLE CHARACTERISTIC CALLBACKS
// =====================================================

class CommandCallbacks : public BLECharacteristicCallbacks {

    void onWrite(BLECharacteristic* characteristic) override {

        String value = characteristic->getValue();
        value.trim();

        debugPacket(value);

        if (!clientConnected) {
            Serial.println("[ERROR] Packet received without BLE link");
            characteristic->setValue("ERR:NO_LINK");
            return;
        }

        // -------------------------------------------------
        // HELLO
        // -------------------------------------------------

        if (value.startsWith("HELLO:")) {
            Serial.println("[AUTH] HELLO received");

            String nonce = value.substring(6);

            uint8_t check[16];

            if (!fromHex(nonce, check, sizeof(check))) {
                characteristic->setValue("ERR:BAD_NONCE");

                disconnectRequested = true;

                Serial.println("[AUTH] Rejected malformed HELLO");
                return;
            }

            clearAuthentication();

            pendingClientNonce = nonce;
            pendingServerNonce = makeNonce();

            String response = "CHALLENGE:" + pendingServerNonce;

            characteristic->setValue(response.c_str());

            Serial.println("[AUTH] Challenge issued");
            Serial.printf(
                "[AUTH] Challenge length: %u bytes\n",
                (unsigned int)response.length()
            );

            return;
        }

        // -------------------------------------------------
        // AUTH
        // -------------------------------------------------

        if (value.startsWith("AUTH:")) {
            Serial.println("[AUTH] AUTH received");

            if (
                pendingClientNonce.isEmpty() ||
                pendingServerNonce.isEmpty()
            ) {
                characteristic->setValue("ERR:NO_CHALLENGE");

                Serial.println("[AUTH] No pending challenge");
                return;
            }

            uint8_t received[32];
            uint8_t expected[32];

            String supplied = value.substring(5);

            bool validHex =
                fromHex(supplied, received, sizeof(received));

            bool hmacCalculated =
                calculateHmac(
                    pendingClientNonce,
                    pendingServerNonce,
                    expected
                );

            if (
                !validHex ||
                !hmacCalculated ||
                !constantTimeEqual(
                    received,
                    expected,
                    sizeof(expected)
                )
            ) {
                clearAuthentication();

                characteristic->setValue("ERR:AUTH_FAILED");

                disconnectRequested = true;

                Serial.println("[AUTH] Authentication FAILED");
                return;
            }

            clientAuthenticated = true;

            pendingClientNonce = "";
            pendingServerNonce = "";

            characteristic->setValue("AUTH_OK");

            Serial.println("[AUTH] Unity application authenticated");
            Serial.println("[AUTH] Commands are now allowed");

            return;
        }

        // -------------------------------------------------
        // COMMAND AUTHORIZATION
        // -------------------------------------------------

        if (!clientAuthenticated) {
            characteristic->setValue("ERR:UNAUTHORIZED");

            Serial.println("[AUTH] Command blocked: not authenticated");

            return;
        }

        // -------------------------------------------------
        // EXECUTE COMMAND
        // -------------------------------------------------

        debugHeader("COMMAND RECEIVED FROM UNITY");

        Serial.printf(
            "[BLE] Command size: %u bytes\n",
            (unsigned int)value.length()
        );

        bool success = applyCommand(value);

        if (success) {
            characteristic->setValue("OK");

            Serial.println();
            Serial.println("[BLE] COMMAND PROCESSED SUCCESSFULLY");
            debugResult("OK");
        }
        else {
            characteristic->setValue("ERR:BAD_COMMAND");

            Serial.println();
            Serial.println("[BLE] COMMAND PROCESSING FAILED");
            debugResult("ERR:BAD_COMMAND");
        }

        Serial.println("========================================");
    }
};

// =====================================================
// BLE INITIALIZATION
// =====================================================

void initializeBLE() {

    Serial.println("[BLE] Initializing BLE stack...");

    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setMTU(185);

    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new ServerCallbacks());

    BLEService* service =
        bleServer->createService(SERVICE_UUID);

    commandCharacteristic =
        service->createCharacteristic(
            CHARACTERISTIC_UUID,
            BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_WRITE
        );

    commandCharacteristic->setValue("GUITARLED_AUTH_V1");

    commandCharacteristic->setCallbacks(
        new CommandCallbacks()
    );

    service->start();

    bleAdvertising = BLEDevice::getAdvertising();

    bleAdvertising->addServiceUUID(SERVICE_UUID);
    bleAdvertising->setScanResponse(true);
    bleAdvertising->setMinPreferred(0x06);
    bleAdvertising->setMaxPreferred(0x12);

    BLEDevice::startAdvertising();

    Serial.println("[BLE] Advertising as GuitarLED");
    Serial.println("[BLE] GATT service ready");
    Serial.println("[BLE] Characteristic ready");
}

// =====================================================
// SETUP
// =====================================================

void setup() {

    Serial.begin(115200);
    delay(800);

    debugHeader("GUITAR LED CONTROLLER");

    Serial.println("[BOOT] Starting ESP32");

    Serial.printf("[BOOT] DATA_PIN: %d\n", DATA_PIN);
    Serial.printf("[BOOT] NUM_LEDS: %d\n", NUM_LEDS);
    Serial.printf("[BOOT] BRIGHTNESS: %d\n", BRIGHTNESS);

    FastLED.addLeds<
        LED_TYPE,
        DATA_PIN,
        COLOR_ORDER
    >(leds, NUM_LEDS);

    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear(true);

    Serial.println("[BOOT] LED strip initialized");

    initializeBLE();

    Serial.println();
    Serial.println("[BOOT] Ready");
    Serial.println("[BOOT] Waiting for Unity");
    Serial.println("[BOOT] Commands require authentication");

    debugHeader("SYSTEM READY");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {

    // Restart advertising after disconnect.
    if (advertisingRestartRequested) {

        advertisingRestartRequested = false;

        delay(ADV_RESTART_DELAY_MS);

        BLEDevice::startAdvertising();

        Serial.println("[BLE] Advertising restarted");
    }

    // Authentication timeout.
    if (
        clientConnected &&
        !clientAuthenticated &&
        millis() - connectedAtMs > AUTH_TIMEOUT_MS
    ) {
        Serial.println(
            "[AUTH] Timeout: disconnecting unauthenticated central"
        );

        disconnectRequested = true;
    }

    // Disconnect unauthorized client.
    if (
        disconnectRequested &&
        clientConnected &&
        bleServer != nullptr
    ) {
        disconnectRequested = false;

        Serial.println("[BLE] Disconnect requested");

        bleServer->disconnect(activeConnectionId);
    }

    // Periodic status.
    if (millis() - lastDebugTime >= 5000) {

        lastDebugTime = millis();

        Serial.printf(
            "[STATUS] BLE Link=%s | AppAuth=%s\n",
            clientConnected ? "CONNECTED" : "DISCONNECTED",
            clientAuthenticated ? "YES" : "NO"
        );
    }

    delay(10);
}
