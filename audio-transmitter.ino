#include <WiFi.h>
#include <AsyncUDP.h>
#include "AudioTools.h"
#include "AudioTools/AudioLibs/SPDIFOutput.h"
#include <ESPmDNS.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

// Wi-Fi Credentials
const char* ssid = "put ur user";
const char* password = "put ur pass";

// Network Configuration
const int udpPort = 1234;

// Audio Format
const int SAMPLE_RATE = 44100;
const int CHANNELS = 2;
const int BITS_PER_SAMPLE = 16;

// Hardware
const int SPDIF_GPIO_PIN = 27;
const int STATUS_LED_PIN = 2;

size_t RING_BUFFER_SIZE = 16384;  // runtime size (set after PSRAM alloc)
constexpr size_t MIN_BUFFER_FOR_PLAYBACK = 4096;
constexpr size_t AUDIO_CHUNK_SIZE = 1024;

AsyncUDP udp;
SPDIFStream spdif;

// Ring Buffer
uint8_t* ringBuffer = nullptr;
volatile size_t write_pos = 0;
volatile size_t read_pos = 0;
volatile size_t bytes_in_buffer = 0;
portMUX_TYPE bufferMutex = portMUX_INITIALIZER_UNLOCKED;

// monitoring metrics
unsigned long lastStatsTime = 0;
size_t totalBytesProcessed = 0;
size_t packetsReceived = 0;

// IR blaster
IRsend irsend(25);  // GPIO 25 (breadboard LED)

// The sony configs i got are in the github too on why these values
const uint32_t SONY_POWER = 0xA90;   // 12-bit Sony
const uint32_t SONY_INPUT = 0x48;    // 12-bit Sony

void wakeAndSelectOptical() {
  Serial.println("[IR] blasting POWER + INPUT …");
  irsend.sendSony(SONY_POWER, 12);  // wake
  delay(1500);
  irsend.sendSony(SONY_INPUT, 12);  // not sure if this actually works
  delay(500);
  irsend.sendSony(SONY_INPUT, 12);  // back to "TV"
  Serial.println("[IR] done – bar should show 'TV'");
}

size_t getBufferedBytes() {
  size_t count;
  portENTER_CRITICAL(&bufferMutex);
  count = bytes_in_buffer;
  portEXIT_CRITICAL(&bufferMutex);
  return count;
}

size_t getFreeSpace() {
  return RING_BUFFER_SIZE - getBufferedBytes();
}

// Network Callback
void onPacket(AsyncUDPPacket packet) {
  packetsReceived++;
  const uint8_t* data = packet.data();
  size_t len = packet.length();
  if (getFreeSpace() < len) {
    Serial.println("Buffer overflow, dropping packet!");
    return;
  }

  portENTER_CRITICAL(&bufferMutex);
  for (size_t i = 0; i < len; i++) {
    ringBuffer[write_pos] = data[i];
    write_pos = (write_pos + 1) % RING_BUFFER_SIZE;
  }
  bytes_in_buffer += len;
  portEXIT_CRITICAL(&bufferMutex);
}

void setupUDPServer() {
  if (udp.listen(udpPort)) {
    Serial.printf("✓ UDP Listening on port %d\n", udpPort);
    udp.onPacket(onPacket);
  } else {
    Serial.println("✗ UDP listener failed!");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n ESP32 S/PDIF Transmitter + IR");

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(SPDIF_GPIO_PIN, LOW);
  Serial.println("GPIO27 configured as OUTPUT");

  // IR LED pin
  pinMode(25, OUTPUT);
  digitalWrite(25, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");

  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  auto cfg = spdif.defaultConfig();
  cfg.pin_data = SPDIF_GPIO_PIN;
  cfg.sample_rate = SAMPLE_RATE;
  cfg.channels = CHANNELS;
  cfg.bits_per_sample = BITS_PER_SAMPLE;

  if (!spdif.begin(cfg)) {
    Serial.println("ERROR: S/PDIF init failed!");
    while (1) {
      digitalWrite(SPDIF_GPIO_PIN, !digitalRead(SPDIF_GPIO_PIN));
      delay(100);
    }
  }

  Serial.println("✓ S/PDIF Output Initialized");

  if (MDNS.begin("esp32-spdif")) {
    Serial.println("✓ mDNS responder started: esp32-spdif.local");
  }

  // PSRAM buffer
  ringBuffer = (uint8_t*)ps_malloc(256 * 1024);
  if (!ringBuffer) {
    ringBuffer = (uint8_t*)malloc(32 * 1024);
  }

  RING_BUFFER_SIZE = ringBuffer ? 256 * 1024 : 32 * 1024;
  read_pos = write_pos = bytes_in_buffer = 0;

  Serial.printf("✓ Ring buffer allocated: %d KB (%s)\n",
                RING_BUFFER_SIZE / 1024,
                (RING_BUFFER_SIZE > 32 * 1024) ? "PSRAM" : "RAM");

  setupUDPServer();

  Serial.println("Serial commands: 'i' = IR test, 't' = LED test");
}

// Main Loop
void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'i') wakeAndSelectOptical();
  }

  // Audio streaming
  if (getBufferedBytes() >= MIN_BUFFER_FOR_PLAYBACK) {
    uint8_t audio_chunk[AUDIO_CHUNK_SIZE];
    size_t bytes_to_read = 0;

    portENTER_CRITICAL(&bufferMutex);
    if (bytes_in_buffer >= AUDIO_CHUNK_SIZE) {
      bytes_to_read = AUDIO_CHUNK_SIZE;
      for (size_t i = 0; i < bytes_to_read; i++) {
        audio_chunk[i] = ringBuffer[read_pos];
        read_pos = (read_pos + 1) % RING_BUFFER_SIZE;
      }
      bytes_in_buffer -= bytes_to_read;
    }
    portEXIT_CRITICAL(&bufferMutex);

    if (bytes_to_read > 0) {
      spdif.write(audio_chunk, bytes_to_read);
      totalBytesProcessed += bytes_to_read;
    }
  } else {
    delay(1);
  }

  // 3-second stats, helpful for testing
  if (millis() - lastStatsTime > 3000) {
    size_t buffered = getBufferedBytes();
    float bufferPercent = (float)buffered / RING_BUFFER_SIZE * 100;

    Serial.printf("[Stats] Buffer: %d/%d (%d%%) | Packets: %d | Data: %d KB\n",
                  buffered, RING_BUFFER_SIZE, (int)bufferPercent,
                  packetsReceived, totalBytesProcessed / 1024);

    packetsReceived = 0;
    totalBytesProcessed = 0;
    lastStatsTime = millis();

    if (buffered > 0) {
      digitalWrite(STATUS_LED_PIN, HIGH);
      delay(10);
      digitalWrite(STATUS_LED_PIN, LOW);
    }
  }
}
