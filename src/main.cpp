/**
* This is an application for esp8266 sniffing wifi probes.
* Can be configured to stay in a single channel or hop through 1-14 2.4G wifi channels.
* Maintains a buffer for seen MAC-addresses and has the possibility to filter out local MACs.
* In "static mode" (STATIC_MODE true) buffer acts as a rollbuffer of defined size (BUFFER_SIZE).
* When channel hopping is used (STATIC_MODE false) buffer is resetted after every sweep 1-14 channels.
* SPI functions are not tested.
* Based on https://github.com/kalanda
* Author: jajupoik
*/

#include <Arduino.h>
#include <SPISlave.h>

extern "C" {
  #include <user_interface.h>
}

// Configurable definitions:
#define IGNORE_LOCAL_MACS true            // true --> locally administred MAC-addresses are ignored.
#define CHANNEL_HOP_INTERVAL_MS   30000   // timer for channel hopping.
#define STATIC_MODE false                 // if set true channel hopping is disabled --> static scannig mode
#define INITIAL_WIFI_CHANNEL 1            // channel to be used in static- and starting channel for dynamic mode
#define BUFFER_SIZE 100                    // MAC entry buffer size
#define SPI_SEND_ADDRESSES false          // Send MAC-addresses with SPI when they are first seen.
#define SPI_SEND_CLIENT_COUNT false        // Send client count in dynamic mode after 1-14 channels are scanned.

#define DATA_LENGTH           112

#define TYPE_MANAGEMENT       0x00
#define TYPE_CONTROL          0x01
#define TYPE_DATA             0x02
#define SUBTYPE_PROBE_REQUEST 0x04

// Sniffer packet data structure
struct RxControl {
 signed rssi:8; // signal intensity of packet
 unsigned rate:4;
 unsigned is_group:1;
 unsigned:1;
 unsigned sig_mode:2; // 0:is 11n packet; 1:is not 11n packet;
 unsigned legacy_length:12; // if not 11n packet, shows length of packet.
 unsigned damatch0:1;
 unsigned damatch1:1;
 unsigned bssidmatch0:1;
 unsigned bssidmatch1:1;
 unsigned MCS:7; // if is 11n packet, shows the modulation and code used (range from 0 to 76)
 unsigned CWB:1; // if is 11n packet, shows if is HT40 packet or not
 unsigned HT_length:16;// if is 11n packet, shows length of packet.
 unsigned Smoothing:1;
 unsigned Not_Sounding:1;
 unsigned:1;
 unsigned Aggregation:1;
 unsigned STBC:2;
 unsigned FEC_CODING:1; // if is 11n packet, shows if is LDPC packet or not.
 unsigned SGI:1;
 unsigned rxend_state:8;
 unsigned ampdu_cnt:8;
 unsigned channel:4; //which channel this packet in.
 unsigned:12;
};

// Sniffer packet structure
struct SnifferPacket{
    struct RxControl rx_ctrl;
    uint8_t data[DATA_LENGTH];
    uint16_t cnt;
    uint16_t len;
};

char macs[BUFFER_SIZE][18];
int clientCount = 0;

static void getMAC(char *addr, uint8_t* data, uint16_t offset) {
  sprintf(addr, "%02x:%02x:%02x:%02x:%02x:%02x", data[offset+0], data[offset+1], data[offset+2], data[offset+3], data[offset+4], data[offset+5]);
}

static boolean isLocalMAC(uint8_t* data) {
  uint8_t local = (data[10] & 0b00000010) >> 1;
  if (local) return true;
  return false;
}

static void printDataSpan(uint16_t start, uint16_t size, uint8_t* data) {
  for(uint16_t i = start; i < DATA_LENGTH && i < start+size; i++) {
    Serial.write(data[i]);
  }
}

// MAC buffer functions. These administer the local buffer so that each addresses
// is taken into account only once.
static boolean bufferCheckMAC(char newmac[]){
  for (int i=0;i<=clientCount;i++) {
    if (strcmp(newmac, macs[i]) == 0) {
      return true;
    }
  }
  return false;
}

static void bufferRollBack() {
  if (clientCount >= BUFFER_SIZE) {
    Serial.println("Buffer rollback.");
    for (int i=1; i<clientCount; i++) {
      strcpy(macs[i-1], macs[i]);
    }
    clientCount--;
  }
}

static void bufferAdd(char newmac[]) {
  bufferRollBack();
  strcpy(macs[clientCount++],newmac);
}

static void bufferReset() {
  Serial.println("Resetting buffer.");
  for (int i=0; i<clientCount; i++) {
    strcpy(macs[i], "");
  }
  clientCount = 0;
}

static void showMetadata(SnifferPacket *snifferPacket) {

  unsigned int frameControl = ((unsigned int)snifferPacket->data[1] << 8) + snifferPacket->data[0];

  uint8_t version      = (frameControl & 0b0000000000000011) >> 0;
  uint8_t frameType    = (frameControl & 0b0000000000001100) >> 2;
  uint8_t frameSubType = (frameControl & 0b0000000011110000) >> 4;
  uint8_t toDS         = (frameControl & 0b0000000100000000) >> 8;
  uint8_t fromDS       = (frameControl & 0b0000001000000000) >> 9;

  // Only look for probe request packetsBUFFER_SIZE
  if (frameType != TYPE_MANAGEMENT ||
      frameSubType != SUBTYPE_PROBE_REQUEST)
        return;

  if (isLocalMAC(snifferPacket->data) && IGNORE_LOCAL_MACS) return;

  char addr[] = "00:00:00:00:00:00";
  getMAC(addr, snifferPacket->data, 10);
  RxControl rxControl = snifferPacket->rx_ctrl;

  if (!bufferCheckMAC(addr)) {
    bufferAdd(addr);

    char msg [50];
    sprintf(msg, "MAC: %s RSSI: %d Ch: %d cnt: %d", addr, rxControl.rssi, rxControl.channel, clientCount);
    Serial.println(msg);
    if (SPI_SEND_ADDRESSES) SPISlave.setData(addr);
  }
}

/**
 * Callback for promiscuous mode
 */
static void ICACHE_FLASH_ATTR sniffer_callback(uint8_t *buffer, uint16_t length) {
  struct SnifferPacket *snifferPacket = (struct SnifferPacket*) buffer;
  showMetadata(snifferPacket);
}

static os_timer_t channelHop_timer;

/**
 * Callback for channel hoping
 */
void channelHop()
{
  // hoping channels 1-14
  uint8 new_channel = wifi_get_channel() + 1;
  if (new_channel > 14) {
    new_channel = 1;
    Serial.print("Total clients:");
    Serial.println(clientCount);

    if(SPI_SEND_CLIENT_COUNT) {
      char msg [5];
      sprintf(msg, "%d",clientCount);
      SPISlave.setData(msg);
      bufferReset();
    }
  }

  wifi_set_channel(new_channel);

  Serial.print("Channel: ");
  Serial.println(wifi_get_channel());

}

#define DISABLE 0
#define ENABLE  1

void setup() {
  // set the WiFi chip to "promiscuous" mode aka monitor mode
  Serial.begin(115200);
  delay(10);
  wifi_set_opmode(STATION_MODE);
  wifi_set_channel(INITIAL_WIFI_CHANNEL);
  wifi_promiscuous_enable(DISABLE);
  delay(10);
  wifi_set_promiscuous_rx_cb(sniffer_callback); // set callback for wifi packets
  delay(10);
  wifi_promiscuous_enable(ENABLE);

  // setup the channel hoping callback timer if not in static mode.
  if (!STATIC_MODE) {
    os_timer_disarm(&channelHop_timer);
    os_timer_setfn(&channelHop_timer, (os_timer_func_t *) channelHop, NULL);
    os_timer_arm(&channelHop_timer, CHANNEL_HOP_INTERVAL_MS, 1);
  }
}

void loop() {
//  delay(10);
}
