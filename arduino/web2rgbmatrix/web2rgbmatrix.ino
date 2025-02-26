/*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
* You can download the latest version of this code from:
* https://github.com/kconger/MiSTer_web2rgbmatrix
*/
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <AnimatedGIF.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ESP32FtpServer.h>
#include <ESP32Ping.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>

#define DEFAULT_SSID "MY_SSID"
char ssid[80] = DEFAULT_SSID;

#define DEFAULT_PASSWORD "password"
char password[80] = DEFAULT_PASSWORD;

#define DEFAULT_AP_SSID "rgbmatrix"
char ap[80] = DEFAULT_AP_SSID;

#define DEFAULT_AP_PASSWORD "password"
char ap_password[80] = DEFAULT_PASSWORD;

#define DEFAULT_HOSTNAME "rgbmatrix"
char hostname[80] = DEFAULT_HOSTNAME;

#define DEFAULT_BRIGHTNESS 255
const uint8_t brightness = DEFAULT_BRIGHTNESS;

#define DEFAULT_PING_FAIL_COUNT 6 // Set to '0' to disable client ping check
int ping_fail_count = DEFAULT_PING_FAIL_COUNT;

#define DEFAULT_SD_GIF_FOLDER "/gifs/"
char gif_folder[80] = DEFAULT_SD_GIF_FOLDER;

#define DEFAULT_SD_ANIMATED_GIF_FOLDER "/agifs/"
char animated_gif_folder[80] = DEFAULT_SD_ANIMATED_GIF_FOLDER;

#define DBG_OUTPUT_PORT Serial

#define VERSION 1.3

// SD Card reader pins
// ESP32-Trinity Pins
#define SD_SCLK 33
#define SD_MISO 32
#define SD_MOSI 21
#define SD_SS 22

SPIClass spi = SPIClass(HSPI);

// Matrix Config
// See the "displaySetup" method for more display config options
MatrixPanel_I2S_DMA *dma_display = nullptr;
const int panelResX = 64;        // Number of pixels wide of each INDIVIDUAL panel module.
const int panelResY = 32;        // Number of pixels tall of each INDIVIDUAL panel module.
const int panels_in_X_chain = 2; // Total number of panels in X
const int panels_in_Y_chain = 1; // Total number of panels in Y
const int totalWidth  = panelResX * panels_in_X_chain;
const int totalHeight = panelResY * panels_in_Y_chain;
int16_t xPos = 0, yPos = 0; // Top-left pixel coord of GIF in matrix space

WebServer server(80);
IPAddress my_ip;
IPAddress no_ip(0,0,0,0);
IPAddress client_ip(0,0,0,0);
FtpServer ftp_server;

// Style for HTML pages
String style =
  "<style>#file-input,input{width:100%;height:44px;border-radius:4px;margin:10px auto;font-size:15px}"
  "a{outline:none;text-decoration:none;padding: 2px 1px 0;color:#777;}a:link{color:#777;}a:hover{solid;color:#3498db;}"
  "input{background:#f1f1f1;border:0;padding:0 15px}body{background:#3498db;font-family:sans-serif;font-size:14px;color:#777}"
  "#file-input{padding:0;border:1px solid #ddd;line-height:44px;text-align:left;display:block;cursor:pointer}"
  "#bar,#prgbar{background-color:#f1f1f1;border-radius:10px}#bar{background-color:#3498db;width:0%;height:10px}"
  "form{background:#fff;max-width:258px;margin:75px auto;padding:30px;border-radius:5px;text-align:center}"
  ".btn{background:#3498db;color:#fff;cursor:pointer}.btn:disabled,.btn.disabled {background:#ddd;color:#fff;cursor:not-allowed;pointer-events: none}"
  ".otabtn{background:#218838;color:#fff;cursor:pointer}.btn.disabled {background:#ddd;color:#fff;cursor:not-allowed;pointer-events: none}"
  ".rebootbtn{background:#c82333;color:#fff;cursor:pointer}.btn.disabled {background:#ddd;color:#fff;cursor:not-allowed;pointer-events: none}"
  "input[type=\"checkbox\"]{margin:0px;width:22px;height:22px;}"
  "</style>";

String homepage = "https://github.com/kconger/MiSTer_web2rgbmatrix";
const char *secrets_filename = "/secrets.json";
const char *gif_filename = "/temp.gif";

String wifi_mode = "AP";
String sd_status = "";
bool card_mounted = false;
bool config_display_on = true;
bool tty_client = false;
unsigned long last_seen, start_tick;
int ping_fail = 0;
String sd_filename = "";
File gif_file, upload_file;
String new_command = "";
String current_command = "";
AnimatedGIF gif;

void setup(void) {    
  DBG_OUTPUT_PORT.begin(115200);
  DBG_OUTPUT_PORT.setDebugOutput(true);
  delay(1000);

  // Initialize internal filesystem
  DBG_OUTPUT_PORT.println("Loading LITTLEFS");
  if(!LittleFS.begin(true)){
    DBG_OUTPUT_PORT.println("LITTLEFS Mount Failed");
  }
  LittleFS.remove(gif_filename);

  // Initialize gif object
  gif.begin(LITTLE_ENDIAN_PIXELS);

  // Initialize Display
  displaySetup();

  // Initialize Wifi
  parseSecrets();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  DBG_OUTPUT_PORT.print("Connecting to ");DBG_OUTPUT_PORT.println(ssid);
    
  // Wait for connection
  uint8_t i = 0;
  while ((WiFi.status() != WL_CONNECTED) && (i++ < 20)) { //wait 10 seconds
    delay(500);
  }
  if (i == 21) {
    DBG_OUTPUT_PORT.print("Could not connect to ");DBG_OUTPUT_PORT.println(ssid);
    // Startup Access Point
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap, ap_password);
    my_ip = WiFi.softAPIP();
    DBG_OUTPUT_PORT.print("IP address: ");DBG_OUTPUT_PORT.println(my_ip.toString());
  } else {
    my_ip = WiFi.localIP();
    wifi_mode = "Infrastructure";
    DBG_OUTPUT_PORT.print("Connected! IP address: ");DBG_OUTPUT_PORT.println(my_ip.toString());
  }
  WiFi.onEvent(WiFiStationConnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_CONNECTED);
  WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(WiFiStationDisconnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // Initialize SD Card
  spi.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_SS);
  if (SD.begin(SD_SS, spi, 8000000)) {
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
      DBG_OUTPUT_PORT.println("No SD card");
      sd_status = "No card";
    } else {
      card_mounted = true;
      DBG_OUTPUT_PORT.print("SD Card Type: ");
      if (cardType == CARD_MMC) {
        DBG_OUTPUT_PORT.println("MMC");
      } else if (cardType == CARD_SD) {
        DBG_OUTPUT_PORT.println("SDSC");
      } else if (cardType == CARD_SDHC) {
        DBG_OUTPUT_PORT.println("SDHC");
      } else {
        DBG_OUTPUT_PORT.println("UNKNOWN");
      }
      uint64_t cardSize = SD.cardSize() / (1024 * 1024);
      DBG_OUTPUT_PORT.printf("SD Card Size: %lluMB\n", cardSize);
      sd_status = String(cardSize) + "MB";
      ftp_server.begin(SD, ap, ap_password); 
    }
  } else {
    DBG_OUTPUT_PORT.println("Card Mount Failed");
    sd_status = "Mount Failed";
  }

  // Startup MDNS 
  if (MDNS.begin(hostname)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("ftp", "tcp", 21);
    DBG_OUTPUT_PORT.println("MDNS responder started");
    DBG_OUTPUT_PORT.print("You can now connect to ");
    DBG_OUTPUT_PORT.print(hostname);
    DBG_OUTPUT_PORT.println(".local");
  }

  // Startup Webserver
  server.on("/", handleRoot);
  server.on("/ota", handleOTA);
  server.on("/update", HTTP_POST, [](){
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "OTA Update Failure\n" : "OTA Update Success\n");
    delay(2000);
    ESP.restart();
  }, handleUpdate);
  server.on("/sdcard", handleSD);
  server.on("/upload", HTTP_POST, [](){server.sendHeader("Connection", "close");}, handleUpload);
  server.on("/localplay", handleLocalPlay);
  server.on("/remoteplay", HTTP_POST, [](){server.send(200);}, handleRemotePlay);
  server.on("/text", handleText);
  server.on("/version", handleVersion);
  server.on("/clear", handleClear);
  server.on("/reboot", handleReboot);
  server.onNotFound(handleNotFound);
  const char *headerkeys[] = {"Content-Length"};
  size_t headerkeyssize = sizeof(headerkeys) / sizeof(char *);
  server.collectHeaders(headerkeys, headerkeyssize);
  server.begin();
  
  // Display boot status on matrix
  String display_string = "rgbmatrix.local\n" + my_ip.toString() + "\nWifi: " + wifi_mode + "\nSD: " + sd_status;
  showText(display_string);
  start_tick = millis();

  DBG_OUTPUT_PORT.println("Startup Complete");
} /* setup() */

void loop(void) {
  server.handleClient();
  ftp_server.handleFTP();
  checkSerialClient();
  // Clear initial boot display after 1min
  if (config_display_on && (millis() - start_tick >= 60*1000UL)){
    config_display_on = false;
    dma_display->clearScreen();
  }
  // Check if client who requested core image has gone away, if so clear screen
  if(client_ip != no_ip){
    if (ping_fail_count != 0){
      if (ping_fail == 0){
        if (millis() - last_seen >= 30*1000UL){
          last_seen = millis();
          bool success = Ping.ping(client_ip, 1);
          if(!success){
            DBG_OUTPUT_PORT.println("Initial Ping failed");
            ping_fail = ping_fail + 1;
          } else {
            DBG_OUTPUT_PORT.println("Ping success");
            ping_fail = 0;
          }
        }
      } else if (ping_fail >= 1 && ping_fail < ping_fail_count){ // Increase ping frequecy after first failure
        if (millis() - last_seen >= 10*1000UL){
          last_seen = millis();
          bool success = Ping.ping(client_ip, 1);
          if(!success){
            DBG_OUTPUT_PORT.print("Ping fail count: ");
            ping_fail = ping_fail + 1;
            DBG_OUTPUT_PORT.println(ping_fail);
          } else {
            DBG_OUTPUT_PORT.println("Ping success");
            ping_fail = 0;
          }
        }
      } else if (ping_fail >= ping_fail_count){
        // Client gone clear display
        DBG_OUTPUT_PORT.println("Client gone, clearing display and deleting the GIF.");
        dma_display->clearScreen();
        client_ip = {0,0,0,0};
        LittleFS.remove(gif_filename);
        sd_filename = "";
      }
    }
  }
} /* loop() */

bool parseSecrets() {
  // Open file for parsing
  File secretsFile = LittleFS.open(secrets_filename);
  if (!secretsFile) {
    DBG_OUTPUT_PORT.println("ERROR: Could not open secrets.json file for reading!");
    return false;
  }

  // Check if we can deserialize the secrets.json file
  StaticJsonDocument<200> doc;
  DeserializationError err = deserializeJson(doc, secretsFile);
  if (err) {
    DBG_OUTPUT_PORT.println("ERROR: deserializeJson() failed with code ");
    DBG_OUTPUT_PORT.println(err.c_str());

    return false;
  }

  // Read settings from secrets.json file
  strlcpy(ssid, doc["ssid"] | DEFAULT_SSID, sizeof(ssid));
  strlcpy(password, doc["password"] | DEFAULT_PASSWORD, sizeof(password));
       
  // Close the secrets.json file
  secretsFile.close();
  return true;
} /* parseSecrets() */

void WiFiStationConnected(WiFiEvent_t event, WiFiEventInfo_t info){
  DBG_OUTPUT_PORT.println("Connected to AP successfully!");
} /* WiFiStationConnected() */

void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info){
  DBG_OUTPUT_PORT.println("WiFi connected");
  DBG_OUTPUT_PORT.println("IP address: ");
  DBG_OUTPUT_PORT.println(WiFi.localIP());
  my_ip = WiFi.localIP();
} /* WiFiGotIP() */

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info){
  DBG_OUTPUT_PORT.print("WiFi lost connection. Reason: ");
  DBG_OUTPUT_PORT.println(info.wifi_sta_disconnected.reason);
  DBG_OUTPUT_PORT.println("Trying to Reconnect");
  WiFi.begin(ssid, password);
} /* WiFiStationDisconnected() */

void returnHTML(String html) {
  server.send(200, F("text/html"), html + "\r\n");
} /* returnHTML() */

void returnHTTPError(int code, String msg) {
  server.send(code, F("text/plain"), msg + "\r\n");
} /* returnHTTPError() */

void handleRoot() {
  String gif_button = "";
  if(card_mounted){
    gif_button = "<input type=\"button\" class=btn onclick=\"location.href='/sdcard';\" value=\"GIF Upload\" />";
  }
  String image_status = "";
  if (LittleFS.exists(gif_filename)){
    image_status = "Client: " + client_ip.toString() + "<br><br>" +
    "Current Image<br><img src=\"" + String(gif_filename) + "\"><img><br>";
  } else if (sd_filename != ""){
    image_status = "Client: " + ((tty_client) ? "Serial" : client_ip.toString()) + "<br><br>" +
    "Current Image<br><img src=\"" + sd_filename + "\"><img><br>";
  }
  String html =
    "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
    "<head>"
    "<title>web2rgbmatrix</title>"
    "<meta http-equiv=\"refresh\" content=\"30\">"
    "<script>"
    "function myFunction() {"
    "var x = document.getElementById(\"idPassword\")\;"
    "if (x.type === \"password\") {"
    "x.type = \"text\"\;"
    "} else {"
    "x.type = \"password\"\;"
    "}"
    "}"
    "</script>" 
    + style +
    "</head>"
    "<body>"
    "<form action=\"/\">"
    "<a href=\""+ homepage + "\"><h1>web2rgbmatrix</h1></a><br>"
    "<p>"
    "<b>Status</b><br>"
    "Version: " + String(VERSION) + "<br>"
    "SD Card: " + sd_status + "<br>"
    "Wifi Mode: " + wifi_mode + "<br>"
    "rgbmatrix IP: " + my_ip.toString() + "<br>"
    + image_status + "<br>"
    "<p>"
    "<b>Wifi Client Settings</b><br>"
    "SSID<br>"
    "<input type=\"text\" name=\"ssid\" value=\"" + String(ssid) + "\"><br>"
    "Password<br>"
    "<input type=\"password\" name=\"password\" id=\"idPassword\" value=\"" + String(password) + "\"><br>"
    "<div>"
    "<label for=\"showpass\" class=\"chkboxlabel\">"
    "<input type=\"checkbox\"id=\"showpass\" onclick=\"myFunction()\">"
    " Show Password</label>"
    "</div>"
    "<input type=\"submit\" class=btn value=\"Save\"><br>"
    "</form>"
    "<form>"
    "<input type=\"button\" class=btn onclick=\"location.href='/clear';\" value=\"Clear Display\" />"
    + gif_button +
    "<input type=\"button\" class=otabtn onclick=\"location.href='/ota';\" value=\"OTA Update\" />"
    "<input type=\"button\" class=rebootbtn onclick=\"location.href='/reboot';\" value=\"Reboot\" />"
    "</form>"
    "</body>"
    "</html>";
  if (server.method() == HTTP_GET) {
    if (server.arg("ssid") != "" && server.arg("password") != "") {
      server.arg("ssid").toCharArray(ssid, sizeof(ssid));
      server.arg("password").toCharArray(password, sizeof(password));
      // Write secrets.json
      StaticJsonDocument<200> doc;
      doc["ssid"] = server.arg("ssid");
      doc["password"] = server.arg("password");
      File data_file = LittleFS.open(secrets_filename, FILE_WRITE);
      if (!data_file) {
        returnHTTPError(500, "Failed to open config file for writing");
      }
      serializeJson(doc, data_file);
      html =
        "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
        "<head>"
        "<title>Config Saved</title>"
        "<meta http-equiv=\"refresh\" content=\"3\;URL=\'/\'\" />"
        + style +
        "</head>"
        "<body>"
        "<form>"
        "<p>Config Saved</p>"
        "</form>"
        "</body>"
        "</html>";
    }
    returnHTML(html);
  } else {
    returnHTTPError(405, "SD Card Not Mounted");
  }
} /* handleRoot() */

void handleOTA(){
  if (server.method() == HTTP_GET) {
    String html = 
      "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
      "<head>"
      "<title>web2rgbmatrix - Update</title>"
      + style +
      "</head>"
      "<body>"
      "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
      "<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
      "<h1>OTA Update</h1>"
      "<input type='file' name='update' id='file' onchange='sub(this)' style=display:none>"
      "<label id='file-input' for='file'>   Choose file...</label>"
      "<input id='sub-button' type='submit' class=btn value='Update'>"
      "<br><br>"
      "<div id='prg'></div>"
      "<br><div id='prgbar'><div id='bar'></div></div><br>"
      "<input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" />"
      "</form>"
      "<script>"
      "function sub(obj){"
      "var fileName = obj.value.split('\\\\');"
      "document.getElementById('file-input').innerHTML = '   '+ fileName[fileName.length-1];"
      "};"
      "$('form').submit(function(e){"
      "e.preventDefault();"
      "var form = $('#upload_form')[0];"
      "var data = new FormData(form);"
      "$.ajax({"
      "url: '/update',"
      "type: 'POST',"
      "data: data,"
      "contentType: false,"
      "processData:false,"
      "xhr: function() {"
      "$('#sub-button').prop('disabled', true);"
      "$('#back-button').prop('disabled', true);"
      "var xhr = new window.XMLHttpRequest();"
      "xhr.upload.addEventListener('progress', function(evt) {"
      "if (evt.lengthComputable) {"
      "var per = evt.loaded / evt.total;"
      "if (evt.loaded != evt.total) {"
      "$('#prg').html('Upload Progress: ' + Math.round(per*100) + '%');"
      "} else {"
      "$('#prg').html('Applying update and rebooting');"
      "}"
      "$('#bar').css('width',Math.round(per*100) + '%');"
      "}"
      "}, false);"
      "return xhr;"
      "},"
      "success:function(d, s) {"
      "console.log('success!');"
      "setTimeout(function(){window.location.href = '/';}, 5000);"
      "},"
      "error: function (a, b, c) {"
      "console.log('ERROR!');"
      "}"
      "});"
      "});"
      "</script>"
      "</body>"
      "</html>";
    returnHTML(html);
  } else {
    returnHTTPError(405, "SD Card Not Mounted");
  }
} /* handleOTA() */

void handleUpdate(){
  // curl -F 'file=@web2rgbmatrix.ino.bin' http://rgbmatrix.local/update
  client_ip = {0,0,0,0};
  LittleFS.remove(gif_filename);
  sd_filename = "";
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    showTextLine("OTA Update Started");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // Start with max available size
      Update.printError(DBG_OUTPUT_PORT);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // Flashing firmware to ESP32
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(DBG_OUTPUT_PORT);
      showTextLine("OTA Update Error");
    } else {
      showTextLine("OTA Progress: " + String((Update.progress()*100)/Update.size()) + "%");
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      showTextLine("OTA Update Success\nRebooting...");
    } else {
      Update.printError(DBG_OUTPUT_PORT);
      showTextLine("OTA Update Error");
    }
  }
} /* handleUpdate() */

void handleSD() {
  if(card_mounted){
    if (server.method() == HTTP_GET) {
      String html = 
        "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
        "<head>"
        "<title>web2rgbmatrix - GIF Upload</title>"
        + style +
        "</head>"
        "<body>"
        "<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
        "<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
        "<h1>GIF Upload</h1>"
        "<input type='file' name='update' id='file' onchange='sub(this)' style=display:none>"
        "<label id='file-input' for='file'>   Choose file...</label>"
        "<input id='sub-button' type='submit' class=btn value='Upload'>"
        "<br><br>"
        "<div id='prg'></div>"
        "<br><div id='prgbar'><div id='bar'></div></div><br>"
        "<input id='back-button' type=\"button\" class=btn onclick=\"location.href='/';\" value=\"Back\" />"
        "</form>"
        "<script>"
        "function sub(obj){"
        "var fileName = obj.value.split('\\\\');"
        "document.getElementById('file-input').innerHTML = '   '+ fileName[fileName.length-1];"
        "};"
        "$('form').submit(function(e){"
        "e.preventDefault();"
        "var form = $('#upload_form')[0];"
        "var data = new FormData(form);"
        "$.ajax({"
        "url: '/upload',"
        "type: 'POST',"
        "data: data,"
        "contentType: false,"
        "processData:false,"
        "xhr: function() {"
        "var xhr = new window.XMLHttpRequest();"
        "xhr.upload.addEventListener('progress', function(evt) {"
        "if (evt.lengthComputable) {"
        "var per = evt.loaded / evt.total;"
        "if (evt.loaded != evt.total) {"
        "$('#sub-button').prop('disabled', true);"
        "$('#back-button').prop('disabled', true);"
        "$('#prg').html('Upload Progress: ' + Math.round(per*100) + '%');"
        "} else {"
        "$('#sub-button').prop('disabled', false);"
        "$('#back-button').prop('disabled', false);"
        "var inputFile = $('form').find(\"input[type=file]\");"
        "var fileName = inputFile[0].files[0].name;"
        "$('#prg').html('Upload Success, click GIF to play<br><a href=\"/localplay?file=' + fileName.split(\".\")[0] +'\"><img src=\"" + String(gif_folder) + "' + fileName.charAt(0) + '/' + fileName + '\"></a>');"
        "}"
        "$('#bar').css('width',Math.round(per*100) + '%');"
        "}"
        "}, false);"
        "return xhr;"
        "},"
        "success:function(d, s) {"
        "console.log('success!') "
        "},"
        "error: function (a, b, c) {"
        "}"
        "});"
        "});"
        "</script>"
        "</body>"
        "</html>";
      returnHTML(html);
    } else {
      returnHTTPError(500, "SD Card Not Mounted");
    }
  } else {
    returnHTTPError(405, "SD Card Not Mounted");
  }
} /* handleSD() */

void handleUpload() {
  if (card_mounted){
    HTTPUpload& uploadfile = server.upload();
    if(uploadfile.status == UPLOAD_FILE_START) {
      String filename = String(uploadfile.filename);
      if(!filename.startsWith("/")) filename = String(gif_folder) + filename.charAt(0) + "/" + String(uploadfile.filename);
      SD.remove(filename);
      upload_file = SD.open(filename, FILE_WRITE);
      filename = String();
    } else if (uploadfile.status == UPLOAD_FILE_WRITE) {
      if(upload_file) upload_file.write(uploadfile.buf, uploadfile.currentSize);
    } else if (uploadfile.status == UPLOAD_FILE_END) {
      if(upload_file) {                                    
        upload_file.close();
        returnHTML("SUCCESS");
      } else {
        returnHTTPError(500, "Couldn't create file");
      }
    }
  } else {
   returnHTTPError(500, "SD Card Not Mounted");
  }
} /* handleUpload() */

void handleRemotePlay(){
  // To upload/play a GIF with curl
  // curl -F 'file=@MENU.gif' http://rgbmatrix.local/remoteplay
  static long contentLength = 0;
  HTTPUpload& uploadfile = server.upload();
  if(uploadfile.status == UPLOAD_FILE_START) {
    contentLength = server.header("Content-Length").toInt();
    if (contentLength > (LittleFS.totalBytes() - LittleFS.usedBytes())) {
      Serial.println("File too large: " + String(contentLength) + " > " + String(LittleFS.totalBytes() - LittleFS.usedBytes()));
      returnHTTPError(500, "File too large");
    }
    LittleFS.remove(gif_filename);
    upload_file = LittleFS.open(gif_filename, FILE_WRITE);
  } else if (uploadfile.status == UPLOAD_FILE_WRITE) {
    if(upload_file) upload_file.write(uploadfile.buf, uploadfile.currentSize);
  } else if (uploadfile.status == UPLOAD_FILE_END) {
    if(upload_file) {                                    
      upload_file.close();
      client_ip = server.client().remoteIP();
      returnHTML("SUCCESS");
      sd_filename = "";
      tty_client = false;
      showGIF(gif_filename,false);
    } else {
      returnHTTPError(500, "Couldn't create file");
    }
  }
} /* handleRemotePlay() */

void handleLocalPlay(){
  // To play a GIF from SD card with curl
  // curl http://rgbmatrix.local/localplay?file=MENU
  if (server.method() == HTTP_GET) {
    if (card_mounted){
      if (server.arg("file") != "") {
        // Check for and play animated GIF
        String agif_fullpath = String(animated_gif_folder) + server.arg("file").charAt(0) + "/" + server.arg("file") + ".gif";
        const char *agif_requested_filename = agif_fullpath.c_str();
        if (SD.exists(agif_requested_filename)) {
          returnHTML("Displaying local animated GIF");
          LittleFS.remove(gif_filename);
          tty_client = false;
          sd_filename = agif_fullpath;
          client_ip = server.client().remoteIP();
          showGIF(agif_requested_filename, true);
        }
        // Check for and play static GIF
        String fullpath = String(gif_folder) + server.arg("file").charAt(0) + "/" + server.arg("file") + ".gif";
        const char *requested_filename = fullpath.c_str();
        if (!SD.exists(requested_filename)) {
          returnHTTPError(404, "File Not Found");
          showTextLine(server.arg("file"));
        } else {
          returnHTML("Displaying local GIF");
          LittleFS.remove(gif_filename);
          tty_client = false;
          sd_filename = fullpath;
          client_ip = server.client().remoteIP();
          showGIF(requested_filename, true);
        } 
      } else {
        returnHTTPError(405, "Method Not Allowed");
      }
    } else {
      returnHTTPError(500, "SD Card Not Mounted");
    }
  } else {
    returnHTTPError(405, "Method Not Allowed");
  }
} /* handleLocalPlay() */

void handleText(){
  if (server.method() == HTTP_GET) {
    if (server.arg("line") != "") {
        showTextLine(server.arg("line"));
        returnHTML("SUCCESS");
      } else {
        returnHTTPError(405, "Method Not Allowed");
      }
  } else {
    returnHTTPError(405, "Method Not Allowed");
  }
} /* handleText() */

void handleVersion(){
  returnHTML(String(VERSION));
} /* handleVersion() */

void handleClear(){
  dma_display->clearScreen();
  sd_filename = "";
  config_display_on = false;
  LittleFS.remove(gif_filename);
  String html =
    "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
    "<head>"
    "<title>Display Cleared</title>"
    "<meta http-equiv=\"refresh\" content=\"3\;URL=\'/\'\" />"
    + style +
    "</head>"
    "<body>"
    "<form>"
    "<p>Display Cleared</p>"
    "</form>"
    "</body>"
    "</html>";
  returnHTML(html);
} /* handleClear() */

void handleReboot() {
  String html =
    "<html xmlns=\"http://www.w3.org/1999/xhtml\">"
    "<head>"
      "<title>Rebooting...</title>"
      "<meta http-equiv=\"refresh\" content=\"60\;URL=\'/\'\" />"
    + style +
    "</head>"
    "<body>"
    "<form>"
      "<p>Rebooting...</p>"
    "</form>"
    "</body>"
    "</html>";
  returnHTML(html);
  ESP.restart();
} /* handleReboot() */

void handleNotFound() {
  String path = server.uri();
  String data_type = "text/plain";
  if (path.endsWith("/")) {
    path += "index.html";
  }
  if (path.endsWith(".src")) {
    path = path.substring(0, path.lastIndexOf("."));
  } else if (path.endsWith(".htm")) {
    data_type = "text/html";
  } else if (path.endsWith(".html")) {
    data_type = "text/html";
  } else if (path.endsWith(".css")) {
    data_type = "text/css";
  } else if (path.endsWith(".js")) {
    data_type = "application/javascript";
  } else if (path.endsWith(".png")) {
    data_type = "image/png";
  } else if (path.endsWith(".gif")) {
    data_type = "image/gif";
  } else if (path.endsWith(".jpg")) {
    data_type = "image/jpeg";
  } else if (path.endsWith(".ico")) {
    data_type = "image/x-icon";
  } else if (path.endsWith(".xml")) {
    data_type = "text/xml";
  } else if (path.endsWith(".pdf")) {
    data_type = "application/pdf";
  } else if (path.endsWith(".zip")) {
    data_type = "application/zip";
  }
  
  File data_file;
  if (LittleFS.exists(path.c_str())) {
      data_file = LittleFS.open(path.c_str());
  } else if (card_mounted) {
    if (SD.exists(path.c_str())) {
      data_file = SD.open(path.c_str());
    } else {
      returnHTTPError(404, "File Not Found");
    }
  } 
 
  if (!data_file) {
    returnHTTPError(500, "Couldn't open file");
  }

  if (server.streamFile(data_file, data_type) != data_file.size()) {
    DBG_OUTPUT_PORT.println("Sent less data than expected!");
  }

  data_file.close();
} /* handleNotFound() */

void checkSerialClient() {
  if (DBG_OUTPUT_PORT.available()) {
    new_command = DBG_OUTPUT_PORT.readStringUntil('\n');
    new_command.trim();
  }  
  if (new_command != current_command) {
    if (new_command.endsWith("QWERTZ"));
    else if (new_command.startsWith("CMD"));
    else if (new_command == "cls");
    else if (new_command == "sorg");
    else if (new_command == "bye");
    else {
      tty_client = true;
      sd_filename = "";
      LittleFS.remove(gif_filename);
      client_ip = {0,0,0,0};
      if (card_mounted) {
        // Check for and play animated GIF
        String agif_fullpath = String(animated_gif_folder) + server.arg("file").charAt(0) + "/" + server.arg("file") + ".gif";
        const char *agif_requested_filename = agif_fullpath.c_str();
        if (SD.exists(agif_requested_filename)) {
          sd_filename = agif_fullpath;
          showGIF(agif_requested_filename, true);
        }
        // Check for and play static GIF
        String fullpath = String(gif_folder) + new_command.charAt(0) + "/" + new_command + ".gif";
        const char *requested_filename = fullpath.c_str();
        if (SD.exists(requested_filename)) {
          sd_filename = fullpath;
          showGIF(requested_filename, true);
        } else {
          showTextLine(new_command);
        }
      } else {
        showTextLine(new_command);
      }
    }
    current_command = new_command;
  }
} /* checkSerialClient() */

void displaySetup() {
  HUB75_I2S_CFG mxconfig(
    panelResX,           // module width
    panelResY,           // module height
    panels_in_X_chain    // Chain length
  );

  //Pins for a ESP32-Trinity
  mxconfig.gpio.e = 18;
  mxconfig.gpio.b1 = 26;
  mxconfig.gpio.b2 = 12;
  mxconfig.gpio.g1 = 27;
  mxconfig.gpio.g2 = 13;
  mxconfig.clkphase = false;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(brightness); //0-255
  dma_display->clearScreen();
} /* displaySetup() */

void showTextLine(String text){
  dma_display->clearScreen();
  dma_display->setCursor(0, 0);
  dma_display->println("\n" + text);
} /* showTextLine() */

void showText(String text){
  dma_display->clearScreen();
  dma_display->setCursor(0, 0);
  dma_display->println(text);
} /* showTextLine() */

// Copy a horizontal span of pixels from a source buffer to an X,Y position
// in matrix back buffer, applying horizontal clipping. Vertical clipping is
// handled in GIFDraw() below -- y can safely be assumed valid here.
void span(uint16_t *src, int16_t x, int16_t y, int16_t width) {
  if (x >= totalWidth) return;     // Span entirely off right of matrix
  int16_t x2 = x + width - 1;      // Rightmost pixel
  if (x2 < 0) return;              // Span entirely off left of matrix
  if (x < 0) {                     // Span partially off left of matrix
    width += x;                    // Decrease span width
    src -= x;                      // Increment source pointer to new start
    x = 0;                         // Leftmost pixel is first column
  }
  if (x2 >= totalWidth) {      // Span partially off right of matrix
    width -= (x2 - totalWidth + 1);
  }
  while(x <= x2) {
    dma_display->drawPixel(x++, y, *src++);
  } 
} /* span() */

// Draw a line of image directly on the matrix
void GIFDraw(GIFDRAW *pDraw) {
  uint8_t *s;
  uint16_t *d, *usPalette, usTemp[320];
  int x, y;

  y = pDraw->iY + pDraw->y; // current line

  // Vertical clip
  int16_t screenY = yPos + y; // current row on matrix
  if ((screenY < 0) || (screenY >= totalHeight)) return;

  usPalette = pDraw->pPalette;

  s = pDraw->pPixels;
  
  // Apply the new pixels to the main image
  if (pDraw->ucHasTransparency) { // if transparency used
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    int x, iCount;
    pEnd = s + pDraw->iWidth;
    x = 0;
    iCount = 0;                   // count non-transparent pixels
    while (x < pDraw->iWidth) {
      c = ucTransparent - 1;
      d = usTemp;
      while (c != ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent) { // done, stop
          s--;                    // back up to treat it like transparent
        } else {                  // opaque
          *d++ = usPalette[c];
          iCount++;
        }
      }                           // while looking for opaque pixels
      if (iCount) {               // any opaque pixels?
        span(usTemp, xPos + pDraw->iX + x, screenY, iCount);
        x += iCount;
        iCount = 0;
      }
      // no, look for a run of transparent pixels
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent)
          iCount++;
        else
          s--;
      }
      if (iCount) {
        x += iCount; // skip these
        iCount = 0;
      }
    }
  } else {                         //does not have transparency
    s = pDraw->pPixels;
    // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
    for (x = 0; x < pDraw->iWidth; x++) 
      usTemp[x] = usPalette[*s++];
    span(usTemp, xPos + pDraw->iX, screenY, pDraw->iWidth);
  }
} /* GIFDraw() */

void * GIFOpenFile(const char *fname, int32_t *pSize) {
  DBG_OUTPUT_PORT.print("Playing gif: ");DBG_OUTPUT_PORT.println(fname);
  gif_file = LittleFS.open(fname);
  if (gif_file) {
    *pSize = gif_file.size();
    return (void *)&gif_file;
  }
  return NULL;
} /* GIFOpenFile() */

void * GIFSDOpenFile(const char *fname, int32_t *pSize) {
  DBG_OUTPUT_PORT.print("Playing gif from SD: ");DBG_OUTPUT_PORT.println(fname);
  gif_file = SD.open(fname);
  if (gif_file) {
    *pSize = gif_file.size();
    return (void *)&gif_file;
  }
  return NULL;
} /* GIFOpenFile() */

void GIFCloseFile(void *pHandle) {
  File *gif_file = static_cast<File *>(pHandle);
  if (gif_file != NULL)
     gif_file->close();
} /* GIFCloseFile() */

int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
    int32_t iBytesRead;
    iBytesRead = iLen;
    File *gif_file = static_cast<File *>(pFile->fHandle);
    // Note: If you read a file all the way to the last byte, seek() stops working
    if ((pFile->iSize - pFile->iPos) < iLen)
       iBytesRead = pFile->iSize - pFile->iPos - 1; // <-- ugly work-around
    if (iBytesRead <= 0)
       return 0;
    iBytesRead = (int32_t)gif_file->read(pBuf, iBytesRead);
    pFile->iPos = gif_file->position();
    return iBytesRead;
} /* GIFReadFile() */

int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) { 
  int i = micros();
  File *gif_file = static_cast<File *>(pFile->fHandle);
  gif_file->seek(iPosition);
  pFile->iPos = (int32_t)gif_file->position();
  i = micros() - i;
  //DBG_OUTPUT_PORT.printf("Seek time = %d us\n", i);
  return pFile->iPos;
} /* GIFSeekFile() */

void showGIF(const char *name, bool sd) {
  config_display_on = false;
  dma_display->clearScreen();
  if (sd && card_mounted){
    if (gif.open(name, GIFSDOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
      DBG_OUTPUT_PORT.printf("Successfully opened GIF from SD; Canvas size = %d x %d\n", gif.getCanvasWidth(), gif.getCanvasHeight());
      DBG_OUTPUT_PORT.flush();
      while (gif.playFrame(true, NULL)) {}
      gif.close();
    }    
  } else {
    if (gif.open(name, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
      DBG_OUTPUT_PORT.printf("Successfully opened GIF; Canvas size = %d x %d\n", gif.getCanvasWidth(), gif.getCanvasHeight());
      DBG_OUTPUT_PORT.flush();
      while (gif.playFrame(true, NULL)) {}
      gif.close();
    }
  }
} /* showGIF() */