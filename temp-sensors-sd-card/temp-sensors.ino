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
//#define USE_ENC28J60
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

#define TEMPERATURE_HISTORY_INTERVAL_SEC 5

#define SD_CARD_CSPIN 4
#define ETH_CSPIN 10

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

// save 1k flash vs Sd.h
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

String header;
EthernetServer server = EthernetServer(LISTENPORT);

#define TEMPERATURE_ADDRESS_BYTES 8

OneWire tempOneWire(ONE_WIRE_BUS);
DallasTemperature DS18B20(&tempOneWire);

int temperatureDeviceCount = 0;
float *temperatures = NULL;    // current temp
DeviceAddress *tempDevAddress; // DeviceAddress defined as uint8_t[8]
char **tempDevHexAddress;

void printFreeram()
{
  DPrint(F("Freeram : "));
  DPrintln((long)FreeMemorySum());
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
  delay(1);

  DPrintln(F("STARTUP"));

  DPrintln(F("Init SD"));
  if (!SD.begin(SD_CARD_CSPIN))
  {
    DPrintln(F("SD begin failed"));
    while (1)
      ;
  }


  lastTemperatureRead = lastTemperatureHistoryRecord = millis();

  printFreeram();

  header.reserve(MAX_HEADER_SIZE);
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

  DPrint("my ip : ");
  for (int i = 0; i < 4; ++i)
  {
    DPrint(Ethernet.localIP()[i]);
    if (i != 3)
      DPrint('.');
  }
  DPrintln();

  server.begin();
}

//
// TEMPERATURE SETUP
//
void SetupTemperatureDevices()
{
  DS18B20.begin();
  temperatureDeviceCount = DS18B20.getDeviceCount();
  DPrint(F("temperature device count = "));
  DPrintIntln(temperatureDeviceCount);
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

      DPrint("sensor [");
      DPrintInt(i);
      DPrint("] address = ");
      DPrint(tempDevHexAddress[i]);
      DPrintln();

      DS18B20.setResolution(12);
    }
  }

  ReadTemperatures();
}

//
// TEMPERATURE READ
//
void ReadTemperatures()
{
  DS18B20.requestTemperatures();
  for (int i = 0; i < temperatureDeviceCount; ++i)
  {
    auto temp = DS18B20.getTempC(tempDevAddress[i]);
    DPrint(F("temperature sensor ["));
    DPrintInt(i);
    DPrint(F("] = "));
    DPrintln(temp, 4);
    temperatures[i] = temp;
  }

  lastTemperatureRead = millis();
}

#define CCTYPE_HTML 0
#define CCTYPE_JSON 1
#define CCTYPE_TEXT 2
#define CCTYPE_JS 3

void clientOk(EthernetClient &client, int type)
{
  client.println("HTTP/1.1 200 OK");
  switch (type)
  {
  case CCTYPE_HTML:
    client.println("Content-Type: text/html");
    break;

  case CCTYPE_JSON:
    client.println("Content-Type: application/json");
    break;

  case CCTYPE_TEXT:
    client.println("Content-Type: text/plain");
    break;

  case CCTYPE_JS:
    client.println("Content-Type: text/javascript");
    break;
  }

#if ENABLE_CORS == 1
  client.println("Access-Control-Allow-Origin: *");
#endif

  client.println();
}

//
// LOOP
//
void loop()
{
  size_t size;

  if (EthernetClient client = server.available())
  {

    while ((size = client.available()) > 0)
    {
      header = "";

      for (int i = 0; i < min(MAX_HEADER_SIZE, size); ++i)
      {
        char c = (char)client.read();

        if (c == '\r')
        {
          break;
        }
        header.concat(c);
      }

      //--------------------------
      // /tempdevices
      //--------------------------
      if (strncmp(header.c_str(), "GET /tempdevices", 16) == 0)
      {
        DPrintln("temp devices");
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
      if (strncmp(header.c_str(), "GET /temp/", 10) == 0)
      {
        auto hbasesize = 10; // "GET /temp/"
        bool found = false;

        if (header.length() - hbasesize >= 8)
        {
          clientOk(client, CCTYPE_TEXT);

          for (int i = 0; i < temperatureDeviceCount; ++i)
          {
            if (strncmp(header.c_str() + hbasesize, tempDevHexAddress[i],
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
          client.print(F("not found"));

        client.stop();
        break;
      }

      //--------------------------
      // /temphistory
      //--------------------------
      if (strncmp(header.c_str(), "GET /temphistory", 16) == 0)
      {
        clientOk(client, CCTYPE_JSON);

        /*
        DPrint(F("temperatureHistoryFillCnt:"));
        DPrintln(temperatureHistoryFillCnt);
        DPrint(F("temperatureHistoryOff:"));
        DPrintln(temperatureHistoryOff);

        client.print('[');
        for (int i = 0; i < temperatureDeviceCount; ++i)
        {
          client.print(F("{\""));
          client.print(tempDevHexAddress[i]);
          client.print(F("\":["));
          auto j = (temperatureHistoryFillCnt == TEMPERATURE_HISTORY_SIZE) ? temperatureHistoryOff : 0;
          auto size = min(temperatureHistoryFillCnt, TEMPERATURE_HISTORY_SIZE);
          for (int k = 0; k < size; ++k)
          {
            if (j == TEMPERATURE_HISTORY_SIZE)
              j = 0;
            client.print(temperatureHistory[i][j++]);
            if (k < size - 1)
              client.print(',');
          }
          client.print(F("]}"));
          if (i != temperatureDeviceCount - 1)
            client.print(',');
        }
        client.print(']');
*/
        client.stop();
        break;
      }

      //--------------------------
      // /info
      //--------------------------
      if (strncmp(header.c_str(), "GET /info", 9) == 0)
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
      //if (strncmp(header.c_str(), "GET / ", 6) == 0 || strncmp(header.c_str(), "GET /index.htm", 14) == 0)
      {
        DPrint(F("Freeram : "));
        DPrintln(FreeMemorySum());
        uint8_t buf[SD_CARD_READ_BUFSIZE];

        String pathfilename;
        {
          char path[80];
          int j = 0;
          int i = 5; // skip "GET /"
          auto hlen = header.length();
          auto hcstr = header.c_str();
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

        DPrint(F("serving ["));
        DPrint(pathfilename.c_str());
        DPrintln(']');

        if (SD.exists(pathfilename.c_str()))
        {
          if (pathfilename.indexOf(".js") >= 0)
            clientOk(client, CCTYPE_JS);
          else
            clientOk(client, CCTYPE_HTML);

          auto f = SD.open(pathfilename.c_str());
          while (f.available())
          {
            auto cnt = f.read(buf, SD_CARD_READ_BUFSIZE);
            client.write(buf, cnt);
          }
          f.close();
        }
        else
        {
          DPrint(F("couldn't find ["));
          DPrint(pathfilename.c_str());
          DPrintln(']');
        }

        client.stop();

        break;
      }
    }
  }

  if (TimeDiff(lastTemperatureRead, millis()) > TEMPERATURE_INTERVAL_MS)
  {
    printFreeram();
    ReadTemperatures();
  }

  if (TimeDiff(lastTemperatureHistoryRecord, millis()) > 1000UL * TEMPERATURE_HISTORY_INTERVAL_SEC)
  {
    DPrintln(F("SD contents"));
    SD.ls(&print, LS_R);

    /*
    if (temperatureHistoryFillCnt < TEMPERATURE_HISTORY_SIZE)
      ++temperatureHistoryFillCnt;

    if (temperatureHistoryOff == TEMPERATURE_HISTORY_SIZE)
      temperatureHistoryOff = 0;

    for (int i = 0; i < temperatureDeviceCount; ++i)
    {
      int8_t t = trunc(round(temperatures[i]));
      temperatureHistory[i][temperatureHistoryOff] = t;
    }
    ++temperatureHistoryOff;*/
    lastTemperatureHistoryRecord = millis();
  }
}
