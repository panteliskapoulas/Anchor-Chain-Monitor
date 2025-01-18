#include <ESP8266WiFi.h>       // Βιβλιοθήκη για WiFi
#include <ESP8266WebServer.h>  // Βιβλιοθήκη για Web Server
#include <EEPROM.h>           // Βιβλιοθήκη για EEPROM
#include "index_html.h"        // Ενσωμάτωση HTML κώδικα
#include <PubSubClient.h>     // Βιβλιοθήκη για MQTT

#define ENABLE_DEMO 1       // Επιλογή λειτουργίας Demo Mode  Επιλέξτε 1 για Ενεργοποίηση/0 για Απενεργοποίηση 
#define SAFETY_STOP 2        // Όριο ασφαλείας για το ανέβασμα της αλυσίδας
#define MAX_CHAIN_LENGTH 100  // Μέγιστο μήκος αλυσίδας

#define WiFiMode_AP_STA 1   // Επιλογή λειτουργίας WiFi (0: AP, 1: Client)
const char *ssid = "ASRock";     // Όνομα WiFi δικτύου
const char *password = "P@ssword"; // Κωδικός WiFi δικτύου

ESP8266WebServer server(80);  // Δημιουργία Web Server στην θύρα 80

#define Chain_Calibration_Value 0.33 // Μετατροπή παλμών σε μέτρα
#define Chain_Counter_Pin 2      // Pin για τον μετρητή παλμών
unsigned long Last_int_time = 0; // Χρόνος τελευταίου παλμού
unsigned long Last_event_time = 0; // Χρόνος τελευταίου συμβάντος
long ChainCounter = 0;          // Μετρητής παλμών
long LastSavedCounter = 0;      // Τελευταία αποθηκευμένη τιμή του μετρητή

#define Chain_Up_Pin 5    // Pin για το ρελέ προς τα πάνω
#define Chain_Down_Pin 4  // Pin για το ρελέ προς τα κάτω
int UpDown = 1;          // Κατεύθυνση αλυσίδας (1: κάτω, -1: πάνω)
int OnOff = 0;           // Κατάσταση ρελέ (0: off, 1: on)
unsigned long Watchdog_Timer = 0; // Χρονοδιακόπτης αδράνειας
unsigned long lastSaveTime = 0; // Χρόνος τελευταίας αποθήκευσης
bool mqtt_connected = false; // Κατάσταση σύνδεσης MQTT
volatile char currentCommand[10] = ""; // Τρέχουσα εντολή από MQTT (μέγιστο 9 χαρακτήρες)

// MQTT Settings
const char* mqtt_server = "192.168.1.21"; // Διεύθυνση IP MQTT broker
const int mqtt_port = 1883;           // Θύρα MQTT broker
const char* mqtt_user = "pi";           // Όνομα χρήστη MQTT
const char* mqtt_password = "password"; // Κωδικός χρήστη MQTT

WiFiClient espClient;             // Δημιουργία WiFi client
PubSubClient client(espClient);    // Δημιουργία MQTT client

// EEPROM Address definitions
const int EEPROM_CHAIN_COUNTER_ADDR = 0; // Διεύθυνση για ChainCounter στην EEPROM
const int EEPROM_ON_OFF_ADDR = sizeof(long); // Διεύθυνση για OnOff στην EEPROM
const int EEPROM_CHECKSUM_ADDR = sizeof(long) + sizeof(int);  // Διεύθυνση για checksum στην EEPROM


// Συνάρτηση ελέγχου εγκυρότητας δεδομένων EEPROM
bool isEEPROMDataValid()
{
  long savedChainCounter = 0;
  int savedOnOff = 0;
  long checksum = 0;
    // Ανάγνωση δεδομένων από EEPROM
  EEPROM.get(EEPROM_CHAIN_COUNTER_ADDR, savedChainCounter);
  EEPROM.get(EEPROM_ON_OFF_ADDR, savedOnOff);
  EEPROM.get(EEPROM_CHECKSUM_ADDR, checksum);

  long calculatedChecksum = calculateChecksum(savedChainCounter,savedOnOff); // Υπολογισμός checksum

  return checksum == calculatedChecksum;
}

// Συνάρτηση υπολογισμού checksum
long calculateChecksum(long chainCounter, int onOff) {
    // Απλό XOR checksum
   long checksum = 0;
   for(int i=0; i<sizeof(chainCounter); i++)
   {
    checksum ^= ((chainCounter >> (i * 8)) & 0xFF);
   }
   for(int i=0; i<sizeof(onOff); i++)
    {
      checksum ^= ((onOff >> (i*8)) & 0xFF);
    }
   return checksum;
}

// Συνάρτηση αποθήκευσης στην EEPROM
void saveToEEPROM() {
    long checksum = calculateChecksum(ChainCounter, OnOff); // Υπολογισμός checksum
    EEPROM.put(EEPROM_CHAIN_COUNTER_ADDR, ChainCounter);    // Αποθήκευση ChainCounter
    EEPROM.put(EEPROM_ON_OFF_ADDR, OnOff);                  // Αποθήκευση OnOff
    EEPROM.put(EEPROM_CHECKSUM_ADDR, checksum);           // Αποθήκευση checksum
    EEPROM.commit();    // Εγγραφή στην EEPROM
    Serial.println("EEPROM Saved"); // Εμφάνιση μηνύματος αποθήκευσης
}

// Συνάρτηση φόρτωσης από την EEPROM
void loadFromEEPROM() {
   if(isEEPROMDataValid()) // Έλεγχος εγκυρότητας δεδομένων
    {
      EEPROM.get(EEPROM_CHAIN_COUNTER_ADDR, ChainCounter); // Ανάγνωση ChainCounter
      EEPROM.get(EEPROM_ON_OFF_ADDR, OnOff);             // Ανάγνωση OnOff
       Serial.println("EEPROM Loaded"); // Εμφάνιση μηνύματος φόρτωσης
    }
    else {  // Αν δεν είναι έγκυρα τα δεδομένα, αρχικοποίηση
        ChainCounter = 0;
        OnOff = 0;
      Serial.println("EEPROM data invalid. Init values."); // Εμφάνιση μηνύματος σφάλματος
    }
}

// Συνάρτηση διαχείρισης παλμών (Interrupt)
void IRAM_ATTR handleInterrupt() {
 Serial.println("handleInterrupt called");
  noInterrupts(); // Απενεργοποίηση interrupts
  if (millis() > Last_int_time + 10) { // Αποφυγή διπλοπαλμών
    ChainCounter += UpDown;          // Αύξηση ή μείωση μετρητή
     // Έλεγχος ορίου ασφαλείας ή μέγιστου μήκους
    if ( ( (ChainCounter <= SAFETY_STOP) && (UpDown == -1) && (OnOff == 1) ) ||
         ( (UpDown == 1) && (abs(ChainCounter) * Chain_Calibration_Value >= MAX_CHAIN_LENGTH) ) ) {
      digitalWrite(Chain_Up_Pin, LOW ); // Απενεργοποίηση ρελέ
      digitalWrite(Chain_Down_Pin, LOW );
      OnOff = 0;                      // Απενεργοποίηση ρελέ
       Serial.println("Safety or max length reached. Stopping");
    }
    Last_event_time = millis(); // Αποθήκευση χρόνου τελευταίας αλλαγής
  }
  Last_int_time = millis(); // Αποθήκευση χρόνου τελευταίου interrupt
  interrupts(); // Ενεργοποίηση interrupts
}

// Συνάρτηση Callback για μηνύματα MQTT
void callback(char* topic, byte* payload, unsigned int length) {
    Serial.print("Message arrived [");  // Εμφάνιση μηνύματος
    Serial.print(topic);             // Εμφάνιση topic
    Serial.print("] ");

     char tempMessage[10] = ""; // Δημιουργία προσωρινού buffer
    for (int i = 0; i < length && i < 9; i++) { // Αντιγραφή μηνύματος
      tempMessage[i] = (char)payload[i];
    }

    Serial.println(tempMessage); // Εμφάνιση μηνύματος

   if (strcmp(topic, "chain/command") == 0) // Έλεγχος topic
    {
    Serial.print("Command received: ");  // Εμφάνιση μηνύματος
      Serial.println(tempMessage);     // Εμφάνιση μηνύματος
       if (tempMessage[0] != '\0') // Έλεγχος για κενό μήνυμα
       {
          strncpy(const_cast<char*>(currentCommand), tempMessage, sizeof(currentCommand) -1 ); // Αντιγραφή εντολής
           currentCommand[sizeof(currentCommand)-1] = '\0';  // Εξασφάλιση τερματισμού
       }
        else
         currentCommand[0] = '\0';  // Αν είναι κενό, αποθήκευση κενού
     }
}

// Συνάρτηση σύνδεσης MQTT
void mqtt_connect() {
  client.setServer(mqtt_server, mqtt_port); // Ορισμός διεύθυνσης broker
  Serial.print("Connecting to MQTT Broker: "); // Εμφάνιση μηνύματος
  Serial.println(mqtt_server);  // Εμφάνιση διεύθυνσης broker

  String clientId = "esp8266-client-"; // Δημιουργία client ID
  clientId += String(random(0xffff), HEX); // Προσθήκη τυχαίων χαρακτήρων

  while (!client.connect(clientId.c_str(), mqtt_user, mqtt_password)) { // Προσπάθεια σύνδεσης
    Serial.print("."); // Δείχνει ότι προσπαθεί να συνδεθεί
    delay(1000); // Αναμονή
  }
  Serial.println("\nMQTT Connected"); // Εμφάνιση μηνύματος σύνδεσης
  client.subscribe("chain/command"); // Εγγραφή στο topic
  client.setCallback(callback); // Ορισμός callback συνάρτησης
   mqtt_connected = true; // Αλλαγή κατάστασης σύνδεσης
}

void setup() {
  int wifi_retry = 0;

  // Relay output
  pinMode(Chain_Up_Pin, OUTPUT); // Ορισμός pin εξόδου
  pinMode(Chain_Down_Pin, OUTPUT); // Ορισμός pin εξόδου
  digitalWrite(Chain_Up_Pin, LOW ); // Απενεργοποίηση ρελέ
  digitalWrite(Chain_Down_Pin, LOW ); // Απενεργοποίηση ρελέ

  // Init Chain Count measure with interrupt
  pinMode(Chain_Counter_Pin, INPUT_PULLUP); // Ορισμός pin ως είσοδος με pullup
  attachInterrupt(digitalPinToInterrupt(Chain_Counter_Pin), handleInterrupt, FALLING); // Ορισμός interrupt

  // Init serial
  Serial.begin(115200);
  Serial.print("");
  Serial.println("Start");

  EEPROM.begin(1024); // Έναρξη EEPROM
  loadFromEEPROM();  // Φόρτωση από EEPROM
  LastSavedCounter = ChainCounter;  // Αρχικοποίηση μεταβλητής

  // Init WLAN AP
  if (WiFiMode_AP_STA == 0) { // Έλεγχος αν είμαστε σε AP mode

    WiFi.mode(WIFI_AP);  // Ορισμός AP mode
    delay (100);
    WiFi.softAP(ssid, password); // Δημιουργία AP
    Serial.println("Start WLAN AP"); // Εμφάνιση μηνύματος
    Serial.print("IP address: "); // Εμφάνιση IP
    Serial.println(WiFi.softAPIP());
  } else { // Αλλιώς, client mode

    Serial.println("Start WLAN Client DHCP");
    WiFi.begin(ssid, password); // Σύνδεση με WiFi

    while (WiFi.status() != WL_CONNECTED) { // Έλεγχος σύνδεσης
      wifi_retry++;
      delay(500);
      Serial.print("."); // Δείχνει ότι προσπαθεί να συνδεθεί
      if (wifi_retry > 10) {  // Έλεγχος αποτυχιών
        Serial.println("\nReboot");
        ESP.restart(); // Επανεκκίνηση
      }
    }

    Serial.println("");
    Serial.println("WiFi connected");  // Εμφάνιση μηνύματος σύνδεσης
    Serial.println("IP address: ");  // Εμφάνιση IP
    Serial.println(WiFi.localIP());
    if(WiFi.status() == WL_CONNECTED) // Σύνδεση MQTT αν έχει συνδεθεί το WiFi
      mqtt_connect();
  }

  // Handle HTTP request events
  server.on("/", Event_Index); // Ορισμός διαδρομών Web Server
  server.on("/gauge.min.js", Event_js);
  server.on("/ADC.txt", Event_ChainCount);
  server.on("/up", Event_Up);
  server.on("/down", Event_Down);
  server.on("/stop", Event_Stop);
  server.on("/reset", Event_Reset);

  server.onNotFound(handleNotFound); // Χειρισμός 404 errors

  server.begin(); // Έναρξη Web Server
  Serial.println("HTTP Server started"); // Εμφάνιση μηνύματος έναρξης
}

// Συνάρτηση για αίτημα προς τα πάνω
void Event_Up() {
  server.send(200, "text/plain", "-1000"); // Αποστολή απάντησης
  Serial.println("Up");
  digitalWrite(Chain_Up_Pin, HIGH ); // Ενεργοποίηση ρελέ
  digitalWrite(Chain_Down_Pin, LOW ); // Απενεργοποίηση ρελέ
  Last_event_time = millis(); // Αποθήκευση χρόνου τελευταίας αλλαγής
  UpDown = -1; // Κατεύθυνση προς τα πάνω
  OnOff = 1; // Κατάσταση ρελέ = ενεργό
}

// Συνάρτηση για αίτημα προς τα κάτω
void Event_Down() {
  server.send(200, "text/plain", "-1000"); // Αποστολή απάντησης
  Serial.println("Down");
  digitalWrite(Chain_Up_Pin, LOW );  // Απενεργοποίηση ρελέ
  digitalWrite(Chain_Down_Pin, HIGH ); // Ενεργοποίηση ρελέ
  Last_event_time = millis();   // Αποθήκευση χρόνου τελευταίας αλλαγής
  UpDown = 1;  // Κατεύθυνση προς τα κάτω
  OnOff = 1;    // Κατάσταση ρελέ = ενεργό
}

// Συνάρτηση για αίτημα σταματήματος
void Event_Stop() {
  server.send(200, "text/plain", "-1000"); // Αποστολή απάντησης
  Serial.println("Stop");
  digitalWrite(Chain_Up_Pin, LOW ); // Απενεργοποίηση ρελέ
  digitalWrite(Chain_Down_Pin, LOW ); // Απενεργοποίηση ρελέ
  OnOff = 0;   // Κατάσταση ρελέ = ανενεργό
}

// Συνάρτηση για αίτημα επαναφοράς
void Event_Reset() {
  ChainCounter = 0; // Μηδενισμός μετρητή
  server.send(200, "text/plain", "-1000"); // Αποστολή απάντησης
  Serial.println("Reset"); // Εμφάνιση μηνύματος
}

// Συνάρτηση για το αρχικό αίτημα
void Event_Index() {
  server.send(200, "text/html", indexHTML);
}

// Συνάρτηση για αίτημα JavaScript
void Event_js() {
  server.send(200, "text/html", gauge);
}

// Συνάρτηση για αίτημα μετρητή αλυσίδας
void Event_ChainCount() {
  float temp = (ChainCounter * Chain_Calibration_Value); // Υπολογισμός θέσης
  server.sendHeader("Cache-Control", "no-cache");  // Ορισμός header για cache
  server.send(200, "text/plain", String (temp)); // Αποστολή απάντησης
  Watchdog_Timer = millis(); // Ανανέωση χρονοδιακόπτη

  #if ENABLE_DEMO == 1 // Αν είναι ενεργοποιημένο το demo mode
    if (OnOff == 1) ChainCounter += UpDown;  // Αύξηση/μείωση μετρητή
    // Έλεγχος για όριο ασφαλείας ή μέγιστο μήκος
    if ( ( (ChainCounter <= SAFETY_STOP) && (UpDown == -1) && (OnOff == 1) ) ||
         ( (UpDown == 1) && (abs(ChainCounter) * Chain_Calibration_Value >= MAX_CHAIN_LENGTH) ) ) {
      digitalWrite(Chain_Up_Pin, LOW );  // Απενεργοποίηση ρελέ
      digitalWrite(Chain_Down_Pin, LOW );  // Απενεργοποίηση ρελέ
      OnOff = 0;
       Serial.println("Safety or max length reached. Stopping");
    }
    Last_event_time = millis(); // Αποθήκευση χρόνου τελευταίας αλλαγής
  #endif

   if (client.connected()) { // Έλεγχος σύνδεσης MQTT
        String message = String(temp); // Δημιουργία μηνύματος
        client.publish("chain/position", message.c_str()); // Αποστολή μηνύματος
        Serial.print("Published MQTT Message: "); // Εμφάνιση μηνύματος
        Serial.print(message);
        Serial.print(" to topic:");
        Serial.println("chain/position");
    }
    else {
      Serial.println("MQTT client not connected, can't send data"); // Εμφάνιση μηνύματος σφάλματος
    }
}

void handleNotFound() {
  server.send(404, "text/plain", "File Not Found\n\n"); // Αποστολή σφάλματος 404
}

void loop() {
  int wifi_retry = 0;

  server.handleClient(); // Διαχείριση αιτημάτων web server

  if ( ( millis() > Watchdog_Timer + 1000 ) || // Έλεγχος χρονοδιακόπτη αδράνειας
       ( (OnOff == 1) && (millis() > Last_event_time + 1000)) )  { // Έλεγχος αν ο κινητήρας είναι ενεργός
    digitalWrite(Chain_Up_Pin, LOW );  // Απενεργοποίηση ρελέ
    digitalWrite(Chain_Down_Pin, LOW ); // Απενεργοποίηση ρελέ
    OnOff = 0; // Κατάσταση ρελέ = off
      Serial.println("Watchdog timeout. Relays OFF");
  }

  if ((ChainCounter != LastSavedCounter) || (millis() - lastSaveTime > 300000) )  // Έλεγχος αν άλλαξε ο μετρητής ή αν πέρασαν 5 λεπτά
  {
      saveToEEPROM();    // Αποθήκευση στην EEPROM
      LastSavedCounter = ChainCounter;  // Αποθήκευση τελευταίας τιμής
      lastSaveTime = millis();
  }

  if (WiFiMode_AP_STA == 1) {   // Έλεγχος αν είμαστε σε client mode
    while (WiFi.status() != WL_CONNECTED && wifi_retry < 5 ) {   // Προσπάθεια επανασύνδεσης
      wifi_retry++;
      Serial.println("WiFi not connected. Try to reconnect");
      WiFi.disconnect();
      WiFi.mode(WIFI_OFF);
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
      delay(100);
    }
    if (wifi_retry >= 5) { // Επανεκκίνηση αν δεν συνδεθεί
      Serial.println("\nReboot");
      ESP.restart();
    }
  }
  if (client.connected()) {  // Διαχείριση MQTT μηνυμάτων
        client.loop();
    }
   else if(WiFi.status() == WL_CONNECTED){
       mqtt_connect(); // Επανασύνδεση MQTT
   }

  char tempCommand[10] = "";
     strncpy(tempCommand, const_cast<const char*>(currentCommand), sizeof(tempCommand)-1); // Αντιγραφή εντολής
    tempCommand[sizeof(tempCommand)-1] = '\0'; // Εξασφάλιση τερματισμού
    Serial.print("Current Command: "); // Εμφάνιση εντολής
    Serial.println(tempCommand);
    if (tempCommand[0] != '\0') // Έλεγχος για κενή εντολή
      {
           if(strcmp(tempCommand, "up") == 0)  // Εκτέλεση εντολής "up"
           {
               digitalWrite(Chain_Up_Pin, HIGH ); // Ενεργοποίηση ρελέ
               digitalWrite(Chain_Down_Pin, LOW ); // Απενεργοποίηση ρελέ
                Last_event_time = millis();  // Αποθήκευση χρόνου τελευταίας αλλαγής
               UpDown = -1;   // Κατεύθυνση προς τα πάνω
               OnOff = 1;    // Κατάσταση ρελέ = ενεργό
               Serial.println("executing UP command");
             }
            else if (strcmp(tempCommand, "down") == 0)   // Εκτέλεση εντολής "down"
            {
                 digitalWrite(Chain_Up_Pin, LOW ); // Απενεργοποίηση ρελέ
               digitalWrite(Chain_Down_Pin, HIGH );  // Ενεργοποίηση ρελέ
               Last_event_time = millis(); // Αποθήκευση χρόνου τελευταίας αλλαγής
               UpDown = 1;    // Κατεύθυνση προς τα κάτω
               OnOff = 1;      // Κατάσταση ρελέ = ενεργό
               Serial.println("executing DOWN command");
            }
           else if (strcmp(tempCommand, "stop") == 0 || tempCommand[0] == '\0')  // Εκτέλεση εντολής "stop" ή κενού μηνύματος
           {
               digitalWrite(Chain_Up_Pin, LOW ); // Απενεργοποίηση ρελέ
               digitalWrite(Chain_Down_Pin, LOW ); // Απενεργοποίηση ρελέ
               OnOff = 0;   // Κατάσταση ρελέ = ανενεργό
                Serial.println("executing STOP command or Empty message received");
           }
         currentCommand[0] = '\0';   // Εκκαθάριση τρέχουσας εντολής
      }

  // Dummy to empty input buffer to avoid board to stuck with e.g. NMEA Reader
  if ( Serial.available() ) {
    Serial.read();
  }
}