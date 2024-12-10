#define TINY_GSM_MODEM_SIM800
#define SerialMon Serial
#define SerialAT Serial2
#define SerialGPS Serial1
#define TINY_GSM_DEBUG SerialMon
#define GSM_PIN ""
#define VEHICLE_ID "AEROX_768964" // Example Vehicle ID

// ESP32 and SIM800l pins
#define MODEM_TX 18
#define MODEM_RX 17

// ESP32 and GPS pins
#define GPS_TX_PIN 0
#define GPS_RX_PIN 15

// RELAY
#define RELAY_PIN 2

// Punya DHT
#define DHTPIN 32
#define DHTTYPE DHT22

// buat ADC voltase aki
#define ANALOG_IN_PIN 13 // ESP32 pin GPIO36 (ADC0) connected to voltage sensor
#define REF_VOLTAGE 3.3
#define ADC_RESOLUTION 4096.0
#define R1 30000.0 // resistor values in voltage sensor (in ohms)
#define R2 7500.0  // resistor values in voltage sensor (in ohms)

#define API_KEY "<your_api_key>"

#define USER_EMAIL "<your_email_auth_firebase>"
#define USER_PASSWORD "<your_password_auth_firebase>"
#define DATABASE_URL "<firebase_rtdb_url>"
#define DATABASE_SECRET "<firebase_rtdb_secret>"

#include <Arduino.h>
#include <TinyGsmClient.h>
#include <FirebaseClient.h> // https://github.com/mobizt/ESP_SSLClient
#include <TinyGPS++.h>
#include <DHT.h>

const char apn[] = "internet";
const char gprsUser[] = "";
const char gprsPass[] = "";

TinyGsm modem(SerialAT);
TinyGsmClient gsm_client1(modem, 0);
TinyGsmClient gsm_client2(modem, 1);
TinyGPSPlus gps;
DHT dht(DHTPIN, DHTTYPE);

ESP_SSLClient ssl_client1, ssl_client2;
GSMNetwork gsm_network(&modem, GSM_PIN, apn, gprsUser, gprsPass);
LegacyToken legacy_token(DATABASE_SECRET);
FirebaseApp app;
using AsyncClient = AsyncClientClass;
AsyncClient aClient1(ssl_client1, getNetwork(gsm_network)), aClient2(ssl_client2, getNetwork(gsm_network));
void asyncCB(AsyncResult &aResult);
void printResult(AsyncResult &aResult);

RealtimeDatabase Database;
unsigned long ms = 0;
bool masterSwitchValue = false;

void setup()
{
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);
    analogSetAttenuation(ADC_11db);
    dht.begin();

    SerialMon.begin(115200);
    delay(10);
    SerialMon.println("Wait ...");
    SerialGPS.begin(9600, SERIAL_8N1, GPS_TX_PIN, GPS_RX_PIN);
    SerialAT.begin(115200, SERIAL_8N1, MODEM_TX, MODEM_RX);
    delay(3000);
    SerialMon.println("Initializing modem ...");
    modem.restart();

    String modemInfo = modem.getModemInfo();
    SerialMon.print("Modem Info: ");
    SerialMon.println(modemInfo);

    if (GSM_PIN && modem.getSimStatus() != 3)
    {
        modem.simUnlock(GSM_PIN);
    }
    SerialMon.print("Waiting for network...");
    if (!modem.waitForNetwork())
    {
        SerialMon.println(" fail");
        delay(10000);
        return;
    }
    SerialMon.println(" success");
    if (modem.isNetworkConnected())
    {
        DBG("Network connected");
    }

    String ccid = modem.getSimCCID();
    DBG("CCID:", ccid);
    delay(100);

    String imei = modem.getIMEI();
    DBG("IMEI:", imei);
    delay(100);

    String imsi = modem.getIMSI();
    DBG("IMSI:", imsi);
    delay(100);

    String cop = modem.getOperator();
    DBG("Operator:", cop);
    delay(100);

    SerialMon.print("Connecting to APN: ");
    SerialMon.print(apn);
    if (!modem.gprsConnect(apn, gprsUser, gprsPass))
    {
        SerialMon.println(" fail");
        ESP.restart();
    }
    SerialMon.println(" OK");
    delay(100);
    if (modem.isGprsConnected())
    {
        SerialMon.println("GPRS connected");
    }
    delay(100);

    IPAddress local = modem.localIP();
    String local_string = local.toString();
    DBG("Local IP:", local);
    delay(100);

    char signal_str[12];
    int csq = modem.getSignalQuality();
    itoa(csq, signal_str, 10);
    DBG("Signal quality:", csq);
    delay(1000);

    Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);

    ssl_client1.setInsecure();
    ssl_client1.setDebugLevel(1);
    ssl_client1.setBufferSizes(2048 /* rx */, 1024 /* tx */);
    ssl_client1.setClient(&gsm_client1);

    ssl_client2.setInsecure();
    ssl_client2.setDebugLevel(1);
    ssl_client2.setBufferSizes(2048 /* rx */, 1024 /* tx */);
    ssl_client2.setClient(&gsm_client2);

    Serial.println("Initializing app...");
    initializeApp(aClient1, app, getAuth(legacy_token), asyncCB, "authTask");
    app.getApp<RealtimeDatabase>(Database);
    Database.url(DATABASE_URL);
    Database.setSSEFilters("get,put,patch,keep-alive,cancel,auth_revoked");

    // Read Status Switchnya
    String firebasePath = String("/vehicle/") + VEHICLE_ID + "/master_switch/value";
    Database.get(aClient2, firebasePath.c_str(), asyncCB, true /* SSE mode */, "streamTask");

    // Write Modem ke Firebase
    JsonWriter modemWriter;
    object_t json, obj1, obj2, obj3, obj4, obj5;
    modemWriter.create(obj1, "IMEI", imei);
    modemWriter.create(obj2, "IMSI", imsi);
    modemWriter.create(obj3, "ip_address", local_string);
    modemWriter.create(obj4, "operator", cop);
    modemWriter.create(obj5, "signal_strength", signal_str);
    modemWriter.join(json, 5, obj1, obj2, obj3, obj4, obj5);

    String firebaseModemPath = String("/vehicle/") + VEHICLE_ID + "/modem/";
    Database.set<object_t>(aClient1, firebaseModemPath.c_str(), json, asyncCB, "setTask");
}

void loop()
{
    app.loop();
    Database.loop();

    if (millis() - ms > 20000 && app.ready())
    {
        // Data GPS ke Firebase guys
        char lat_str[12];
        char lng_str[12];
        unsigned long epoch = getEpochTime();
        char timestamp[12];

        if (epoch != 0)
        {
            dtostrf(epoch, 8, 0, timestamp);
            Serial.print("Timestamp = ");
            Serial.println(timestamp);
        }
        else
        {
            strcpy(timestamp, "0");
        }

        Serial.println("Getting GPS data: ");
        float lat = 0, lng = 0;

        for (int i = 2; i; i--)
        {
            while (SerialGPS.available() > 0)
            {
                gps.encode(SerialGPS.read());
            }
            delay(1000);
        }

        if (gps.location.isValid())
        {
            lat = gps.location.lat();
            lng = gps.location.lng();
            dtostrf(lat, 8, 6, lat_str);
            dtostrf(lng, 8, 6, lng_str);

            Serial.print("Latitude = ");
            Serial.println(lat_str);
            Serial.print("Longitude = ");
            Serial.println(lng_str);

            JsonWriter writer;
            object_t json, obj1, obj2, obj3;
            writer.create(obj1, "lat", lat_str);
            writer.create(obj2, "lng", lng_str);
            writer.create(obj3, "timestamp", timestamp);
            writer.join(json, 3, obj1, obj2, obj3);

            String firebasePath = String("/vehicle/") + VEHICLE_ID + "/location/";
            Database.set<object_t>(aClient1, firebasePath.c_str(), json, asyncCB, "setTask");
        }
        else
        {
            Serial.println(F("Invalid GPS data, skipping Firebase update."));
        }

        // Temperature ke Firebase rek
        char temp_str[12];
        char hum_str[12];
        char hall_str[12];
        float temperature = dht.readTemperature();
        float humidity = dht.readHumidity();

        if (!isnan(temperature) && !isnan(humidity))
        {
            dtostrf(temperature, 8, 2, temp_str);
            dtostrf(humidity, 8, 2, hum_str);
            strcpy(hall_str, "15");

            Serial.print("Temperature: ");
            Serial.print(temp_str);
            Serial.print("°C");
            Serial.print(" Humidity: ");
            Serial.print(hum_str);
            Serial.println("%");

            JsonWriter dhtWriter;
            object_t json, obj1, obj2, obj3;
            dhtWriter.create(obj1, "temperature", temp_str);
            dhtWriter.create(obj2, "humidity", hum_str);
            dhtWriter.create(obj3, "hall", hall_str);
            dhtWriter.join(json, 3, obj1, obj2, obj3);

            String firebaseMonitoringPath = String("/vehicle/") + VEHICLE_ID + "/monitoring/";
            Database.set<object_t>(aClient1, firebaseMonitoringPath.c_str(), json, asyncCB, "setTask");
        }
        else
        {
            Serial.println("Failed to read DHT sensor");
        }

        // Voltase Aki ke firebase
        char volt_str[12];
        int adc_value = analogRead(ANALOG_IN_PIN);
        float voltage_adc = ((float)adc_value * REF_VOLTAGE) / ADC_RESOLUTION;
        float voltage_in = voltage_adc * (R1 + R2) / R2;

        if (!isnan(voltage_in))
        {
            dtostrf(voltage_in, 8, 2, volt_str);

            JsonWriter voltWriter;
            object_t json, obj1;
            voltWriter.create(obj1, "voltage", volt_str);
            voltWriter.join(json, 1, obj1);

            String firebaseVoltagePath = String("/vehicle/") + VEHICLE_ID + "/electricity/";
            Database.set<object_t>(aClient1, firebaseVoltagePath.c_str(), json, asyncCB, "setTask");
        }
        else
        {
            Serial.println("Failed to read voltage sensor");
        }
        ms = millis();
    }
}

void asyncCB(AsyncResult &aResult)
{
    // WARNING!
    // Do not put your codes inside the callback and printResult.
    printResult(aResult);
}

void printResult(AsyncResult &aResult)
{
    if (aResult.isEvent())
    {
        Firebase.printf("Event task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.appEvent().message().c_str(), aResult.appEvent().code());
    }

    if (aResult.isDebug())
    {
        Firebase.printf("Debug task: %s, msg: %s\n", aResult.uid().c_str(), aResult.debug().c_str());
    }

    if (aResult.isError())
    {
        Firebase.printf("Error task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
    }

    if (aResult.available())
    {
        RealtimeDatabaseResult &RTDB = aResult.to<RealtimeDatabaseResult>();
        if (RTDB.isStream())
        {
            Serial.println("----------------------------");
            Firebase.printf("task: %s\n", aResult.uid().c_str());
            Firebase.printf("event: %s\n", RTDB.event().c_str());
            Firebase.printf("path: %s\n", RTDB.dataPath().c_str());
            Firebase.printf("data: %s\n", RTDB.to<const char *>());
            Firebase.printf("type: %d\n", RTDB.type());

            // bool value = RTDB.to<bool>();

            if (RTDB.type() == realtime_database_data_type_boolean)
            {
                bool value = RTDB.to<bool>();
                masterSwitchValue = value;
                Serial.print("Change master switch status: ");
                Serial.println(masterSwitchValue);
                digitalWrite(RELAY_PIN, masterSwitchValue ? HIGH : LOW);
            }
        }
        else
        {
            Serial.println("----------------------------");
            Firebase.printf("task: %s, payload: %s\n", aResult.uid().c_str(), aResult.c_str());
        }
        Firebase.printf("Free Heap: %d\n", ESP.getFreeHeap());
    }
}

unsigned long getEpochTime()
{
    modem.NTPServerSync("id.pool.ntp.org", 20);
    int year, month, day, hour, minute, second;
    float timezone;

    if (modem.getNetworkTime(&year, &month, &day, &hour, &minute, &second, &timezone))
    {
        struct tm timeinfo;
        timeinfo.tm_year = year - 1900;
        timeinfo.tm_mon = month - 1;
        timeinfo.tm_mday = day;
        timeinfo.tm_hour = hour;
        timeinfo.tm_min = minute;
        timeinfo.tm_sec = second;

        return mktime(&timeinfo);
    }
    else
    {
        SerialMon.println("Gagal mendapatkan waktu jaringan.");
        return 0;
    }
}
