
#define DEFAULT_STORAGE_TYPE_ESP8266 5

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SimpleFTPServer.h>

// ---------- WiFi Access Point ----------
const char* AP_SSID = "IHR-WiFi-Storage";
const char* AP_PASSWORD = "IHRStorage826";

// ---------- Login credentials ----------
const char* WEB_USER = "admin";
const char* WEB_PASSWORD = "change-web-password";
const char* FTP_USER = "ihr";
const char* FTP_PASSWORD = "change-ftp-password";

// ---------- Hardware pins ----------
#define SD_CS_PIN D0
#define OLED_SDA D2
#define OLED_SCL D1

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

ESP8266WebServer server(80);
FtpServer ftpServer;
Adafruit_SSD1306 display(
  SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1
);

File uploadFile;
String uploadPath;
bool sdReady = false;
unsigned long lastDisplayUpdate = 0;

// ---------- Authentication ----------
bool requireAuth() {
  if (server.authenticate(WEB_USER, WEB_PASSWORD)) {
    return true;
  }

  server.requestAuthentication();
  return false;
}

// ---------- Path protection ----------
String cleanPath(String path) {
  path.replace("\\", "/");

  if (path.length() == 0) path = "/";
  if (!path.startsWith("/")) path = "/" + path;

  while (path.indexOf("//") >= 0) {
    path.replace("//", "/");
  }

  if (path.indexOf("/../") >= 0 ||
      path.endsWith("/..") ||
      path == "/..") {
    return "";
  }

  return path;
}

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  return value;
}

String mimeType(String path) {
  path.toLowerCase();

  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".txt")) return "text/plain";
  if (path.endsWith(".pdf")) return "application/pdf";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg"))
    return "image/jpeg";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".gif")) return "image/gif";
  if (path.endsWith(".webp")) return "image/webp";
  if (path.endsWith(".mp3")) return "audio/mpeg";
  if (path.endsWith(".wav")) return "audio/wav";
  if (path.endsWith(".mp4")) return "video/mp4";

  return "application/octet-stream";
}

// ---------- Web interface ----------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>IHR WiFi Storage</title>
<style>
body{font-family:Arial;background:#101827;color:#eee;margin:0;padding:18px}
main{max-width:850px;margin:auto}
h2{color:#63c5ff}
button,input{padding:10px;margin:4px;border-radius:7px;border:0}
button{background:#1976d2;color:white}
.item{background:#1e293b;padding:12px;margin:7px 0;border-radius:8px;
display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap}
small{color:#aaa}
#status{color:#8ee6a2}
</style>
</head>
<body>
<main>
<h2>IHR WiFi Storage</h2>
<p id="status">Connecting...</p>
<p id="path">/</p>
<button onclick="goUp()">⬆ Back</button>
<button onclick="newFolder()">＋ New folder</button>
<hr>
<input type="file" id="file">
<button onclick="upload()">Upload</button>
<div id="items"></div>
</main>
<script>
let current="/";

async function load(){
  document.getElementById("path").textContent="Folder: "+current;
  const response=await fetch("/api/list?dir="+encodeURIComponent(current));
  if(!response.ok){document.getElementById("status").textContent="Error loading files";return;}
  const data=await response.json();
  const box=document.getElementById("items");
  box.innerHTML="";
  document.getElementById("status").textContent=
    data.items.length+" items";

  data.items.forEach(item=>{
    const row=document.createElement("div");
    row.className="item";

    const info=document.createElement("span");
    info.textContent=(item.dir?"📁 ":"📄 ")+item.name+
      (item.dir?"":" ("+item.size+" bytes)");
    row.appendChild(info);

    const actions=document.createElement("span");

    if(item.dir){
      const open=document.createElement("button");
      open.textContent="Open";
      open.onclick=()=>{current=item.path;load();};
      actions.appendChild(open);
    }else{
      const open=document.createElement("button");
      open.textContent="Open / Download";
      open.onclick=()=>window.open(
        "/download?file="+encodeURIComponent(item.path),"_blank");
      actions.appendChild(open);
    }

    const rename=document.createElement("button");
    rename.textContent="Rename";
    rename.onclick=async()=>{
      const name=prompt("New name:",item.name);
      if(!name)return;
      const newPath=(current==="/"?"":current)+"/"+name;
      const body=new URLSearchParams({old:item.path,new:newPath});
      await fetch("/rename",{method:"POST",body});
      load();
    };
    actions.appendChild(rename);

    const del=document.createElement("button");
    del.textContent="Delete";
    del.onclick=async()=>{
      if(!confirm("Delete "+item.name+"?"))return;
      await fetch("/delete?path="+encodeURIComponent(item.path),
        {method:"POST"});
      load();
    };
    actions.appendChild(del);

    row.appendChild(actions);
    box.appendChild(row);
  });
}

function goUp(){
  if(current==="/")return;
  const parts=current.split("/");
  parts.pop();
  current=parts.join("/")||"/";
  load();
}

async function newFolder(){
  const name=prompt("Folder name:");
  if(!name)return;
  const path=(current==="/"?"":current)+"/"+name;
  await fetch("/mkdir",{method:"POST",
    body:new URLSearchParams({path})});
  load();
}

async function upload(){
  const file=document.getElementById("file").files[0];
  if(!file){alert("Select a file first");return;}
  const data=new FormData();
  data.append("file",file);
  const result=await fetch("/upload?dir="+encodeURIComponent(current),
    {method:"POST",body:data});
  alert(result.ok?"Upload complete":"Upload failed");
  load();
}

load();
</script>
</body>
</html>
)rawliteral";

// ---------- File list ----------
void handleList() {
  if (!requireAuth()) return;

  String dirPath = cleanPath(server.arg("dir"));
  if (dirPath == "" || !sdReady) {
    server.send(400, "application/json", "{\"items\":[]}");
    return;
  }

  File root = SD.open(dirPath);

  if (!root || !root.isDirectory()) {
    server.send(404, "application/json", "{\"items\":[]}");
    return;
  }

  String json = "{\"path\":\"" + jsonEscape(dirPath) +
                "\",\"items\":[";

  bool first = true;
  File entry = root.openNextFile();

  while (entry) {
    String name = String(entry.name());
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);

    String fullPath = dirPath == "/"
      ? "/" + name
      : dirPath + "/" + name;

    if (!first) json += ",";
    first = false;

    json += "{\"name\":\"" + jsonEscape(name) +
            "\",\"path\":\"" + jsonEscape(fullPath) +
            "\",\"dir\":" +
            String(entry.isDirectory() ? "true" : "false") +
            ",\"size\":" + String(entry.size()) + "}";

    entry.close();
    entry = root.openNextFile();
  }

  root.close();
  json += "]}";

  server.send(200, "application/json", json);
}

// ---------- Download / preview ----------
void handleDownload() {
  if (!requireAuth()) return;

  String path = cleanPath(server.arg("file"));

  if (path == "" || !SD.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File file = SD.open(path, FILE_READ);

  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "Cannot open file");
    return;
  }

  server.streamFile(file, mimeType(path));
  file.close();
}

// ---------- Create folder ----------
void handleMkdir() {
  if (!requireAuth()) return;

  String path = cleanPath(server.arg("path"));

  bool ok = path != "" && SD.mkdir(path);

  server.send(ok ? 200 : 400, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---------- Delete ----------
void handleDelete() {
  if (!requireAuth()) return;

  String path = cleanPath(server.arg("path"));
  bool ok = false;

  if (path != "" && path != "/") {
    if (SD.exists(path)) {
      ok = SD.remove(path);
      if (!ok) ok = SD.rmdir(path);
    }
  }

  server.send(ok ? 200 : 400, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---------- Rename ----------
void handleRename() {
  if (!requireAuth()) return;

  String oldPath = cleanPath(server.arg("old"));
  String newPath = cleanPath(server.arg("new"));

  bool ok = oldPath != "" && newPath != "" &&
            oldPath != "/" && newPath != "/" &&
            SD.exists(oldPath) && !SD.exists(newPath) &&
            SD.rename(oldPath, newPath);

  server.send(ok ? 200 : 400, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---------- Upload ----------
void handleUploadData() {
  if (!requireAuth()) return;

  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String dir = cleanPath(server.arg("dir"));

    String name = upload.filename;
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);

    name.replace("\\", "");
    name.replace("/", "");

    if (dir == "" || name.length() == 0 ||
        name == "." || name == "..") {
      uploadPath = "";
      return;
    }

    uploadPath = dir == "/" ? "/" + name : dir + "/" + name;

    if (SD.exists(uploadPath)) SD.remove(uploadPath);
    uploadFile = SD.open(uploadPath, FILE_WRITE);
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    if (uploadPath != "") SD.remove(uploadPath);
  }
}

// ---------- OLED ----------
void updateOLED() {
  if (millis() - lastDisplayUpdate < 1500) return;
  lastDisplayUpdate = millis();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  display.println("IHR WiFi Storage");
  display.println("----------------");
  display.print("IP: ");
  display.println(WiFi.softAPIP());
  display.print("Clients: ");
  display.println(WiFi.softAPgetStationNum());
  display.print("SD Card: ");
  display.println(sdReady ? "READY" : "ERROR");
  display.print("Uptime: ");
  display.print(millis() / 60000);
  display.println(" min");

  display.display();
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("IHR Storage");
    display.println("Starting...");
    display.display();
  }

  SPI.begin();

  sdReady = SD.begin(SD_CS_PIN);

  WiFi.mode(WIFI_AP);

  IPAddress localIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);

  WiFi.softAPConfig(localIP, gateway, subnet);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  server.on("/", HTTP_GET, []() {
    if (!requireAuth()) return;
    server.send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/list", HTTP_GET, handleList);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/mkdir", HTTP_POST, handleMkdir);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/rename", HTTP_POST, handleRename);

  server.on("/upload", HTTP_POST,
    []() {
      if (!requireAuth()) return;
      server.send(200, "application/json", "{\"ok\":true}");
    },
    handleUploadData
  );

  server.onNotFound([]() {
    if (!requireAuth()) return;
    server.send(404, "text/plain", "Not found");
  });

  server.begin();

  if (sdReady) {
    ftpServer.begin(FTP_USER, FTP_PASSWORD);
  }

  Serial.println();
  Serial.println("IHR WiFi Storage started");
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("SD: ");
  Serial.println(sdReady ? "READY" : "ERROR");
}

// ---------- Main loop ----------
void loop() {
  server.handleClient();

  if (sdReady) {
    ftpServer.handleFTP();
  }

  updateOLED();
  yield();
}
