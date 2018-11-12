#define ARDUINO 18070

//==============================================================================
//
//-------------------- PLEASE REVIEW FOLLOW VARIABLES ------------------
//
// SECURITY WARNING : uncomment ENABLE_CORS=0 in production!
//---------------------
// I used CORS policy to allow me write index.htm and app.js outside atmega controller from pc
// using atmega webapis
//
#define ENABLE_CORS 1

// choose one of follow two interface
//#define USE_ENC28J60 // not enough RAM to manage
#define USE_W5500

#define MACADDRESS 0x33, 0xcf, 0x8d, 0x9f, 0x5b, 0x89
#define MYIPADDR 10, 10, 4, 111
#define MYIPMASK 255, 255, 255, 0
#define MYDNS 10, 10, 0, 6
#define MYGW 10, 10, 0, 1
#define LISTENPORT 80
#define MAX_HEADER_SIZE 80
#define ONE_WIRE_BUS 3
// EDIT DebugMacros to set SERIAL_SPEED and enable/disable DPRINT_SERIAL

// storage size each day = temp_device_count * sizeof(float) * 60*60*24 / TEMPERATURE_HISTORY_INTERVAL_SEC
// sizeof(float) = 4
// example (interval 5sec) 4 devices occupies 270k/day or 96mb/year
#define TEMPERATURE_HISTORY_INTERVAL_SEC 5

// set to false in production
bool TEMPERATURE_HISTORY_RESET = true;

#define SD_CARD_CSPIN 4
#define ETH_CSPIN 10

#define NONETH_ACTIVITY_LED_PIN 8
#define ETH_ACTIVITY_LED_PIN 7

//
//==============================================================================

#include <Arduino.h>

// loadtime(ms)/bufsize tests : 5700/1 ; 466/8; 275/16; 162/32; 106/64; 74/128; 62/256
// bufsize 64 enough
#define SD_CARD_READ_BUFSIZE 64

#define TEMPERATURE_INTERVAL_MS 5000
unsigned long lastTemperatureRead, lastTemperatureHistoryRecord;

//-------------------------

#include <OneWire.h>
#include <DallasTemperature.h>
#include <mywifikey.h>

// (https://github.com/greiman/SdFat) save 1k flash vs Sd.h
#include <SdFat.h>
SdFat SD;

#ifdef USE_ENC28J60
#include <UIPEthernet.h>
// edit UIPEthernet/utility/uipethernet-conf.h to customize
// - define UIP_CONF_UDP=0 to reduce flash size
#endif

#ifdef USE_W5500
#include <Ethernet.h>
#endif

#include <DPrint.h>
#include <Util.h>
using namespace SearchAThing::Arduino;

DPrintCls print;

unsigned char *header;
EthernetServer server = EthernetServer(LISTENPORT);

#define TEMPERATURE_ADDRESS_BYTES 8

OneWire tempOneWire(ONE_WIRE_BUS);
DallasTemperature DS18B20(&tempOneWire);

int temperatureDeviceCount = 0;
float *temperatures = NULL;    // current temp
DeviceAddress *tempDevAddress; // DeviceAddress defined as uint8_t[8]
char **tempDevHexAddress;

void noneth_ledon()
{
  digitalWrite(NONETH_ACTIVITY_LED_PIN, HIGH);
}

void noneth_ledoff()
{
  digitalWrite(NONETH_ACTIVITY_LED_PIN, LOW);
}

void eth_ledon()
{
  digitalWrite(ETH_ACTIVITY_LED_PIN, HIGH);
}

void eth_ledoff()
{
  digitalWrite(ETH_ACTIVITY_LED_PIN, LOW);
}

void printFreeram()
{
  DPrintF(F("FR:"));
  DPrintLongln(FreeMemorySum());
}

//
// SETUP
//
void setup()
{
  pinMode(SD_CARD_CSPIN, OUTPUT);
  digitalWrite(SD_CARD_CSPIN, HIGH);

  pinMode(ETH_CSPIN, OUTPUT);
  digitalWrite(ETH_CSPIN, HIGH);

  pinMode(NONETH_ACTIVITY_LED_PIN, OUTPUT);
  pinMode(ETH_ACTIVITY_LED_PIN, OUTPUT);

  noneth_ledoff();
  eth_ledoff();

  delay(1);

  header = (unsigned char *)malloc(MAX_HEADER_SIZE + 1);

  DPrintFln(F("ST"));

  DPrintFln(F("ISD"));
  if (!SD.begin(SD_CARD_CSPIN))
  {
    DPrintFln(F("FAIL"));
    while (1)
      ;
  }

  lastTemperatureRead = lastTemperatureHistoryRecord = millis();

  SetupTemperatureDevices();

  uint8_t mac[6] = {MACADDRESS};
  uint8_t myIP[4] = {MYIPADDR};
  uint8_t myMASK[4] = {MYIPMASK};
  uint8_t myDNS[4] = {MYDNS};
  uint8_t myGW[4] = {MYGW};

  // dhcp
  //Ethernet.begin(mac);

  // static
  Ethernet.begin(mac, myIP, myDNS, myGW, myMASK);

  DPrintF(F("IP:"));
  for (int i = 0; i < 4; ++i)
  {
    DPrintInt16(Ethernet.localIP()[i]);
    if (i != 3)
      DPrintChar('.');
  }
  DPrintln();

  server.begin();

  printFreeram();
}

//
// TEMPERATURE SETUP
//
void SetupTemperatureDevices()
{
  noneth_ledon();

  DS18B20.begin();
  temperatureDeviceCount = DS18B20.getDeviceCount();
  DPrintF(F("TC:"));
  DPrintInt16ln(temperatureDeviceCount);
  if (temperatureDeviceCount > 0)
  {
    temperatures = new float[temperatureDeviceCount];
    tempDevAddress = new DeviceAddress[temperatureDeviceCount];
    tempDevHexAddress = (char **)malloc(sizeof(char *) * temperatureDeviceCount);

    for (int i = 0; i < temperatureDeviceCount; ++i)
    {
      tempDevHexAddress[i] = (char *)malloc(sizeof(char) * (TEMPERATURE_ADDRESS_BYTES * 2 + 1));
      DS18B20.getAddress(tempDevAddress[i], i);
      sprintf(tempDevHexAddress[i], "%02x%02x%02x%02x%02x%02x%02x%02x",
              tempDevAddress[i][0],
              tempDevAddress[i][1],
              tempDevAddress[i][2],
              tempDevAddress[i][3],
              tempDevAddress[i][4],
              tempDevAddress[i][5],
              tempDevAddress[i][6],
              tempDevAddress[i][7]);

      DPrintF(F("T["));
      DPrintInt16(i);
      DPrintF(F("]ADR="));
      DPrintStr(tempDevHexAddress[i]);
      DPrintln();

      DS18B20.setResolution(12);
    }
  }

  ReadTemperatures();

  noneth_ledoff();
}

//
// TEMPERATURE READ
//
void ReadTemperatures()
{
  noneth_ledon();

  DS18B20.requestTemperatures();
  for (int i = 0; i < temperatureDeviceCount; ++i)
  {
    auto temp = DS18B20.getTempC(tempDevAddress[i]);
    temperatures[i] = temp;
  }

  lastTemperatureRead = millis();

  noneth_ledoff();
}

#define CCTYPE_HTML 0
#define CCTYPE_JSON 1
#define CCTYPE_TEXT 2
#define CCTYPE_JS 3

void clientOk(EthernetClient &client, int type)
{
  eth_ledon();

  client.println(F("HTTP/1.1 200 OK"));
  client.print(F("Content-Type: "));
  switch (type)
  {
  case CCTYPE_HTML:
    client.println(F("text/html"));
    break;

  case CCTYPE_JSON:
    client.println(F("application/json"));
    break;

  case CCTYPE_JS:
    client.println(F("text/javascript"));
    break;

  default:
  case CCTYPE_TEXT:
    client.println(F("text/plain"));
    break;
  }

#if ENABLE_CORS == 1
  client.println(F("Access-Control-Allow-Origin: *"));
#endif

  client.println();

  eth_ledoff();
}

//
// LOOP
//
void loop()
{
  size_t size;

  if (EthernetClient client = server.available())
  {
    DPrintFln(F("ETH"));
    eth_ledon();

    while ((size = client.available()) > 0)
    {
      header[0] = 0;
      {
        int i = 0;
        DPrintF(F("SZ:"));
        DPrintInt16ln(size);
        auto lim = min(MAX_HEADER_SIZE, size);
        while (i < lim)
        {
          char c = (char)client.read();

          if (c == '\r')
          {
            break;
          }
          header[i++] = c;
        }
        header[i] = 0;
      }

      if (strlen(header) < 5 || strncmp(header, "GET /", 5) < 0)
      {
        client.stop();
        break;
      }

      DPrintF(F("HDR["));
      DPrintStr(header);
      DPrintCharln(']');

      //--------------------------
      // /tempdevices
      //--------------------------
      if (strncmp(header, "GET /tempdevices", 16) == 0)
      {
        clientOk(client, CCTYPE_JSON);

        client.print(F("{\"tempdevices\":["));
        for (int i = 0; i < temperatureDeviceCount; ++i)
        {
          client.print('"');
          client.print(tempDevHexAddress[i]);
          client.print('"');

          if (i != temperatureDeviceCount - 1)
            client.print(',');
        }
        client.print("]}");

        client.stop();
        break;
      }

      //--------------------------
      // /temp/{id}
      //--------------------------
      if (strncmp(header, "GET /temp/", 10) == 0)
      {
        auto hbasesize = 10; // "GET /temp/"
        bool found = false;

        if (strlen(header) - hbasesize >= 8)
        {
          clientOk(client, CCTYPE_TEXT);

          for (int i = 0; i < temperatureDeviceCount; ++i)
          {
            if (strncmp(header + hbasesize, tempDevHexAddress[i],
                        2 * TEMPERATURE_ADDRESS_BYTES) == 0)
            {
              char tmp[20];
              FloatToString(tmp, temperatures[i], 6);

              client.print(tmp);

              found = true;
              break;
            }
          }
        }
        if (!found)
          client.print(F("NF"));

        client.stop();
        break;
      }

      //--------------------------
      // /temphistory
      //--------------------------
      if (strncmp(header, "GET /temphistory/", 17) == 0)
      {
        DPrintFln(F("TH"));
        // nr of TEMPERATURE_HISTORY_INTERVAL_SEC interval readings requested
        long backlogsize = 0;
        {
          char buf[16];
          int k = 0;
          for (int i = 17; i < strlen(header); ++i)
          {
            if (header[i] == ' ' || k == 15)
              break;
            buf[k++] = header[i];
          }
          buf[k] = 0;
          backlogsize = atol(buf);
        }

        clientOk(client, CCTYPE_JSON);

        auto floatsize = sizeof(float);

        // ensure bufsize multiple of sizeof(float)
        auto bufsize = ((int)(SD_CARD_READ_BUFSIZE / floatsize)) * floatsize;
        uint8_t buf[bufsize];

        client.print('[');
        for (int i = 0; i < temperatureDeviceCount; ++i)
        {
          client.print(F("{\""));
          client.print(tempDevHexAddress[i]);
          client.print(F("\":["));

          if (SD.exists(tempDevHexAddress[i]))
          {
            DPrintF(F("file:["));
            DPrintStr(tempDevHexAddress[i]);
            auto f = SD.open(tempDevHexAddress[i]);
            auto fsize = f.size();
            uint32_t foff = 0;
            if (fsize >= backlogsize * floatsize)
              foff = fsize - backlogsize * floatsize;
            DPrintF(F("] foff:"));
            DPrintUInt32(foff);
            DPrintF(F(" fsize:"));
            DPrintUInt32ln(fsize);

            f.seek(foff);

            char tmp[20];
            int serie = 0;
            while (f.available())
            {
              auto cnt = f.read(buf, bufsize);
              DPrintStr("rcnt:");
              DPrintInt16ln(cnt);
              printFreeram();

              for (int j = 0; j < cnt; j += floatsize)
              {
                float temp = 0.0;
                for (int u = 0; u < floatsize; ++u)
                {
                  ((uint8_t *)(&temp))[u] = buf[j + u];
                }
                DPrintF(F("tmp:"));
                DPrintFloatln(temp);

                FloatToString(tmp, temp, 6);
                if (serie > 0)
                  client.print(',');
                client.print(tmp);
                ++serie;
              }
            }

            f.close();
          }
          client.print(F("]}"));
          if (i != temperatureDeviceCount - 1)
            client.print(',');
        }
        client.print(']');

        client.stop();
        break;
      }

      //--------------------------
      // /info
      //--------------------------
      if (strncmp(header, "GET /info", 9) == 0)
      {
        clientOk(client, CCTYPE_JSON);

        client.print('{');

        client.print(F("\"freeram\":"));
        client.print((long)FreeMemorySum());

        client.print(F(", \"history_interval_sec\":"));
        client.print(TEMPERATURE_HISTORY_INTERVAL_SEC);

        client.print('}');

        client.stop();
        break;
      }

      //--------------------------
      // /
      //--------------------------
      if (strncmp(header, "GET /", 5) == 0)
      {
        printFreeram();

        uint8_t buf[SD_CARD_READ_BUFSIZE];

        String pathfilename;
        {
          char path[80];
          int j = 0;
          int i = 5; // skip "GET /"
          auto hlen = strlen(header);
          auto hcstr = header;
          // note : only html non escapable characters in filename
          while (i < hlen)
          {
            if (hcstr[i] == ' ' || hcstr[i] == '/')
              break;

            path[j++] = hcstr[i++];
          }

          path[j] = 0;

          pathfilename = j == 0 ? String("index.htm") : String(path);
        }

        if (SD.exists(pathfilename.c_str()))
        {
          if (pathfilename.indexOf(".js") >= 0)
            clientOk(client, CCTYPE_JS);
          else
            clientOk(client, CCTYPE_HTML);

          auto f = SD.open(pathfilename.c_str());
          auto size = f.size();

          DPrintF(F("RQ["));
          DPrintStr(pathfilename.c_str());
          DPrintF(F("]SZ:"));
          DPrintFloat(((float)size / 1024), 1);
          DPrintFln(F("K"));

          while (f.available())
          {
            auto cnt = f.read(buf, SD_CARD_READ_BUFSIZE);
            client.write(buf, cnt);
          }
          f.close();
        }
        else
        {
          DPrintF(F("NF["));
          DPrintStr(pathfilename.c_str());
          DPrintCharln(']');
        }

        DPrintFln(F("EINV"));
        client.stop();
        break;
      }
    }

    eth_ledoff();
  }
  else if (TimeDiff(lastTemperatureRead, millis()) > TEMPERATURE_INTERVAL_MS)
  {
    noneth_ledon();
    ReadTemperatures();
    noneth_ledoff();
  }
  else if (TimeDiff(lastTemperatureHistoryRecord, millis()) > 1000UL * TEMPERATURE_HISTORY_INTERVAL_SEC)
  {
    noneth_ledon();
    digitalWrite(NONETH_ACTIVITY_LED_PIN, HIGH);
    auto recbegin = millis();

    for (int i = 0; i < temperatureDeviceCount; ++i)
    {
      auto filename = tempDevHexAddress[i];
      auto t = temperatures[i];

      File f;

      auto exists = SD.exists(filename);
      // open ref
      // https://github.com/greiman/SdFat/blob/46aff556c5ecad832ca3f19e184fd06e765f5641/src/FatLib/FatFile.h#L508
      if (TEMPERATURE_HISTORY_RESET || !exists)
      {
        DPrintF(F("CR:"));
        DPrintStrln(filename);

        if (exists)
          SD.remove(filename);
        f = SD.open(filename, O_WRITE | O_CREAT);
      }
      else
        f = SD.open(filename, O_WRITE | O_APPEND);

      auto size = f.size();

      // save float as bytes
      uint8_t buf[sizeof(float)];
      for (int j = 0; j < sizeof(float); ++j)
      {
        buf[j] = ((uint8_t *)(&t))[j];
      }
      // write ref
      // https://github.com/greiman/SdFat/blob/46aff556c5ecad832ca3f19e184fd06e765f5641/src/FatLib/FatFile.h#L918
      f.write(buf, sizeof(float));
      f.flush();

      f.close();
    }
    TEMPERATURE_HISTORY_RESET = false;

    lastTemperatureHistoryRecord = millis();
    digitalWrite(NONETH_ACTIVITY_LED_PIN, LOW);

    noneth_ledoff();
  }
}
