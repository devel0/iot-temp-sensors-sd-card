#define ARDUINO 18070

//==============================================================================
//
//-------------------- PLEASE REVIEW FOLLOW VARIABLES ------------------
//
// SECURITY WARNING : set to false ENABLE_CORS in production!
//---------------------
// I used CORS policy to allow me write index.htm and app.js outside atmega controller from pc
// using atmega webapis
//
#define ENABLE_CORS true

#define MACADDRESS 0x43, 0xcf, 0x8d, 0x9f, 0x5b, 0x89
#define MYIPADDR 10, 10, 4, 111
#define MYIPMASK 255, 255, 255, 0
#define MYDNS 10, 10, 0, 6
#define MYGW 10, 10, 0, 1
#define LISTENPORT 80
#define MAX_HEADER_SIZE 80
#define ONE_WIRE_BUS 3
#define SD_CARD_CS 4
#define ETH_CS 10

// EDIT DebugMacros to set SERIAL_SPEED and enable/disable DPRINT_SERIAL

//
//==============================================================================

// loadtime(ms)/bufsize tests : 5700/1 ; 466/8; 275/16; 162/32; 106/64; 74/128; 62/256
// bufsize 64 enough
#define SD_CARD_READ_BUFSIZE 64

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <mywifikey.h>

// enc28j60
//#include <UIPEthernet.h>

// w5500
#include <Ethernet.h>

#include <DPrint.h>
#include <Util.h>
using namespace SearchAThing::Arduino;

// save 1k flash vs Sd.h
#include <SdFat.h>
SdFat SD;

String header;
EthernetServer server(LISTENPORT);

#define TEMPERATURE_INTERVAL_MS 5000
#define TEMPERATURE_ADDRESS_BYTES 8

OneWire tempOneWire(ONE_WIRE_BUS);
DallasTemperature DS18B20(&tempOneWire);

int temperatureDeviceCount = 0;
float *temperatures = NULL;
DeviceAddress *tempDevAddress; // DeviceAddress defined as uint8_t[8]
char **tempDevHexAddress;

// save 800 flash bytes vs standard serial
DPrintCls print;

//
// SETUP
//
void setup()
{
  pinMode(SD_CARD_CS, OUTPUT);
  digitalWrite(SD_CARD_CS, HIGH);

  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);

  delay(1);

  DPrintln(F("Init SD"));
  if (!SD.begin(SD_CARD_CS))
  {
    DPrintln(F("SD begin failed"));
    while (1)
      ;
  }

  DPrintln(F("SD contents"));
  SD.ls(&print, LS_R);

  DPrintln(F("initialization done."));

  DPrintln(F("STARTUP"));

  header.reserve(MAX_HEADER_SIZE);
  SetupTemperatureDevices();

  Ethernet.init(ETH_CS);

  uint8_t mac[6] = {MACADDRESS};
  uint8_t myIP[4] = {MYIPADDR};
  uint8_t myMASK[4] = {MYIPMASK};
  uint8_t myDNS[4] = {MYDNS};
  uint8_t myGW[4] = {MYGW};

  // dhcp
  //Ethernet.begin(mac);

  // static
  Ethernet.begin(mac, myIP, myDNS, myGW, myMASK);

  if (Ethernet.hardwareStatus() == EthernetNoHardware)
  {
    DPrintln(F("eth hw not found"));
    while (true)
      ;
  }

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

unsigned long lastTemperatureRead;

//
// TEMPERATURE READ
//
void ReadTemperatures()
{
  DPrintln(F("reading temperatures"));
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
  }

  client.println("Access-Control-Allow-Origin: *");
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
      bool foundcmd = false;

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

      DPrint(F("Header ["));
      DPrint(header.c_str());
      DPrintln(']');

      foundcmd = false;

      //--------------------------
      // TEMPERATURE
      //--------------------------
      if (temperatureDeviceCount > 0)
      {
        String q = String("GET /tempdevices");
        if (header.indexOf(q) >= 0)
        {
          clientOk(client, CCTYPE_JSON);

          client.print("{\"tempdevices\":[");
          for (int i = 0; i < temperatureDeviceCount; ++i)
          {
            client.print('"');
            client.print(tempDevHexAddress[i]);
            client.print('"');

            client.print(",\"");
            client.print(tempDevHexAddress[i]);
            client.print('"');

            if (i != temperatureDeviceCount - 1)
              client.print(',');
          }
          client.print("]}");

          client.stop();
          break;
        }

        q = String("GET /temp/");
        if (header.indexOf(q) >= 0)
        {
          bool found = false;
          if (header.length() - q.length() >= 8)
          {
            clientOk(client, CCTYPE_JSON);

            for (int i = 0; i < temperatureDeviceCount; ++i)
            {
              if (strncmp(header.c_str() + q.length(), tempDevHexAddress[i],
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
      }

      //--------------------------
      // HELP
      //--------------------------
      if (header.indexOf("GET /") >= 0)
      {
        clientOk(client, CCTYPE_HTML);

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

        DPrint(F("pathfilename ["));
        DPrint(pathfilename.c_str());
        DPrintln(']');

        if (SD.exists(pathfilename.c_str()))
        {
          DPrintln(F("opening"));

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
      }
      client.stop();
    }
  }
  else if (TimeDiff(lastTemperatureRead, millis()) > TEMPERATURE_INTERVAL_MS)
  {
    DPrint(F("Freeram : "));
    DPrintln(FreeMemorySum());

    auto link = Ethernet.linkStatus();
    DPrint(F("Link status: "));
    switch (link)
    {
    case Unknown:
      DPrintln(F("Unknown"));
      break;
    case LinkON:
      DPrintln(F("ON"));
      break;
    case LinkOFF:
      DPrintln(F("OFF"));
      break;
    }

    DPrintln(F("SD contents"));
    SD.ls(&print, LS_R);

    ReadTemperatures();

    delay(1000);
  }
}
