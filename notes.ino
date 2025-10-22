#include <WiFi.h>
#include <WebServer.h>
#include <vector>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>

// WiFi credentials
const char* ssid = "REPLACE_WITH_SSID";
const char* password = "REPLACE_WITH_PASSWORD";

// SD Card pins (adjust these based on your SD card module)
#define SD_CS 5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18

// ENCRYPTION SETTINGS - CHANGE THIS TO YOUR OWN SECRET KEY!
// This key is used to encrypt your notes on the SD card
// Keep it secret and don't share it with anyone!
const char* ENCRYPTION_KEY = "REPLACE_WITH_STRONG_SECRET_KEY"; // Must be 32 characters for AES-256

WebServer server(80);
Preferences preferences;

// Storage mode
bool useSDCard = false;
const char* NOTES_FILE = "/notes.enc";
const char* FOLDERS_FILE = "/folders.enc";

// Data structures
struct Note {
  String id;
  String title;
  String content;
  String folder;
  unsigned long timestamp;
};

std::vector<Note> notes;
std::vector<String> folders;

// Energy saving settings
#define LIGHT_SLEEP_ENABLED true
const int SLEEP_DURATION = 100;

// Encryption functions
void deriveKey(const char* password, unsigned char* key) {
  // Use SHA-256 to derive a proper 32-byte key from the password
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)password, strlen(password));
  mbedtls_md_finish(&ctx, key);
  mbedtls_md_free(&ctx);
}

String encryptData(String plaintext) {
  if (plaintext.length() == 0) return "";
  
  unsigned char key[32];
  deriveKey(ENCRYPTION_KEY, key);
  
  // Add padding to make length multiple of 16 (AES block size)
  int paddingLength = 16 - (plaintext.length() % 16);
  for (int i = 0; i < paddingLength; i++) {
    plaintext += (char)paddingLength;
  }
  
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 256);
  
  unsigned char* input = (unsigned char*)plaintext.c_str();
  unsigned char* output = (unsigned char*)malloc(plaintext.length());
  
  // Encrypt in blocks of 16 bytes
  for (size_t i = 0; i < plaintext.length(); i += 16) {
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input + i, output + i);
  }
  
  mbedtls_aes_free(&aes);
  
  // Convert to Base64-like encoding for storage
  String encrypted = "";
  for (size_t i = 0; i < plaintext.length(); i++) {
    char hex[3];
    sprintf(hex, "%02X", output[i]);
    encrypted += hex;
  }
  
  free(output);
  return encrypted;
}

String decryptData(String ciphertext) {
  if (ciphertext.length() == 0 || ciphertext.length() % 2 != 0) return "";
  
  unsigned char key[32];
  deriveKey(ENCRYPTION_KEY, key);
  
  // Convert hex string back to bytes
  int dataLength = ciphertext.length() / 2;
  unsigned char* input = (unsigned char*)malloc(dataLength);
  
  for (int i = 0; i < dataLength; i++) {
    String byteString = ciphertext.substring(i * 2, i * 2 + 2);
    input[i] = strtol(byteString.c_str(), NULL, 16);
  }
  
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, key, 256);
  
  unsigned char* output = (unsigned char*)malloc(dataLength);
  
  // Decrypt in blocks of 16 bytes
  for (int i = 0; i < dataLength; i += 16) {
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, input + i, output + i);
  }
  
  mbedtls_aes_free(&aes);
  
  // Remove padding
  int paddingLength = output[dataLength - 1];
  if (paddingLength > 0 && paddingLength <= 16) {
    dataLength -= paddingLength;
  }
  
  String decrypted = "";
  for (int i = 0; i < dataLength; i++) {
    decrypted += (char)output[i];
  }
  
  free(input);
  free(output);
  return decrypted;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=================================");
  Serial.println("Notes App with Encryption");
  Serial.println("=================================");
  
  // Try to initialize SD card
  Serial.println("Checking for SD card...");
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  // More robust SD card detection - check both initialization and root directory
  if (SD.begin(SD_CS) && SD.exists("/")) {
    Serial.println("SD card detected and validated!");
    Serial.println("Encryption enabled for SD card storage");
    useSDCard = true;
    
    // Initialize preferences to check for existing data
    preferences.begin("notes-app", false);
    
    // Check if there's data in ESP32 flash that needs migration
    int noteCount = preferences.getInt("noteCount", 0);
    if (noteCount > 0) {
      Serial.println("Migrating data from ESP32 to encrypted SD card...");
      migrateToSDCard();
    }
    
    preferences.end();
    
    // Load data from SD card
    loadDataFromSD();
    Serial.println("Using encrypted SD card for storage");
  } else {
    Serial.println("No valid SD card detected, fallback to flash.");
    Serial.println("Using ESP32 flash storage (unencrypted)");
    useSDCard = false;
    
    // Initialize preferences for persistent storage
    preferences.begin("notes-app", false);
    
    // Load data from flash
    loadDataFromFlash();
  }
  
  // Configure WiFi as Access Point
  WiFi.softAP(ssid, password);
  WiFi.setSleep(LIGHT_SLEEP_ENABLED);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.println("\n=================================");
  Serial.print("AP IP address: ");
  Serial.println(IP);
  Serial.println("=================================\n");
  
  // Setup routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/notes", HTTP_GET, handleGetNotes);
  server.on("/api/notes", HTTP_POST, handleCreateNote);
  server.on("/api/notes/delete", HTTP_POST, handleDeleteNote);
  server.on("/api/folders", HTTP_GET, handleGetFolders);
  server.on("/api/folders", HTTP_POST, handleCreateFolder);
  server.on("/api/folders/delete", HTTP_POST, handleDeleteFolder);
  server.on("/api/save", HTTP_POST, handleSaveData);
  server.on("/api/status", HTTP_GET, handleGetStatus);
  
  server.begin();
  Serial.println("HTTP server started");
  
  // Reduce CPU frequency for power saving
  setCpuFrequencyMhz(80);
}

void loop() {
  server.handleClient();
  
  if (LIGHT_SLEEP_ENABLED) {
    delayMicroseconds(SLEEP_DURATION);
  }
}

// Migration function
void migrateToSDCard() {
  Serial.println("Starting migration...");
  
  // Load folders from flash
  int folderCount = preferences.getInt("folderCount", 0);
  for (int i = 0; i < folderCount; i++) {
    String key = "folder" + String(i);
    String folder = preferences.getString(key.c_str(), "");
    if (folder.length() > 0) {
      folders.push_back(folder);
    }
  }
  
  if (folders.size() == 0) {
    folders.push_back("General");
  }
  
  // Load notes from flash
  int noteCount = preferences.getInt("noteCount", 0);
  for (int i = 0; i < noteCount; i++) {
    Note note;
    String prefix = "note" + String(i);
    note.id = preferences.getString((prefix + "id").c_str(), "");
    note.title = preferences.getString((prefix + "title").c_str(), "");
    note.content = preferences.getString((prefix + "content").c_str(), "");
    note.folder = preferences.getString((prefix + "folder").c_str(), "General");
    note.timestamp = preferences.getULong((prefix + "time").c_str(), 0);
    
    if (note.id.length() > 0) {
      notes.push_back(note);
    }
  }
  
  // Save to encrypted SD card
  saveDataToSD();
  
  // Clear flash storage after successful migration
  preferences.clear();
  
  Serial.printf("Migration complete! Migrated %d folders and %d notes\n", folders.size(), notes.size());
  Serial.println("All data is now encrypted on SD card");
}

// SD Card functions with encryption
void loadDataFromSD() {
  // Load folders
  folders.clear();
  if (SD.exists(FOLDERS_FILE)) {
    File file = SD.open(FOLDERS_FILE, FILE_READ);
    if (file) {
      String encryptedData = "";
      while (file.available()) {
        encryptedData += (char)file.read();
      }
      file.close();
      
      // Decrypt the data
      String decryptedData = decryptData(encryptedData);
      
      // Parse folders
      int startPos = 0;
      int endPos = decryptedData.indexOf('\n');
      while (endPos != -1) {
        String line = decryptedData.substring(startPos, endPos);
        line.trim();
        if (line.length() > 0) {
          folders.push_back(line);
        }
        startPos = endPos + 1;
        endPos = decryptedData.indexOf('\n', startPos);
      }
      // Handle last line
      String lastLine = decryptedData.substring(startPos);
      lastLine.trim();
      if (lastLine.length() > 0) {
        folders.push_back(lastLine);
      }
    }
  }
  
  if (folders.size() == 0) {
    folders.push_back("General");
  }
  
  // Load notes
  notes.clear();
  if (SD.exists(NOTES_FILE)) {
    File file = SD.open(NOTES_FILE, FILE_READ);
    if (file) {
      String encryptedData = "";
      while (file.available()) {
        encryptedData += (char)file.read();
      }
      file.close();
      
      // Decrypt the data
      String decryptedData = decryptData(encryptedData);
      
      // Parse notes
      int startPos = 0;
      int endPos = decryptedData.indexOf('\n');
      while (endPos != -1) {
        String line = decryptedData.substring(startPos, endPos);
        line.trim();
        if (line.length() > 0) {
          Note note = parseNoteLine(line);
          if (note.id.length() > 0) {
            notes.push_back(note);
          }
        }
        startPos = endPos + 1;
        endPos = decryptedData.indexOf('\n', startPos);
      }
      // Handle last line
      String lastLine = decryptedData.substring(startPos);
      lastLine.trim();
      if (lastLine.length() > 0) {
        Note note = parseNoteLine(lastLine);
        if (note.id.length() > 0) {
          notes.push_back(note);
        }
      }
    }
  }
  
  Serial.printf("Loaded %d folders and %d notes from encrypted SD card\n", folders.size(), notes.size());
}

void saveDataToSD() {
  // Save folders
  String foldersData = "";
  for (size_t i = 0; i < folders.size(); i++) {
    foldersData += folders[i] + "\n";
  }
  
  String encryptedFolders = encryptData(foldersData);
  File file = SD.open(FOLDERS_FILE, FILE_WRITE);
  if (file) {
    file.print(encryptedFolders);
    file.close();
  }
  
  // Save notes
  String notesData = "";
  for (size_t i = 0; i < notes.size(); i++) {
    notesData += formatNoteLine(notes[i]) + "\n";
  }
  
  String encryptedNotes = encryptData(notesData);
  file = SD.open(NOTES_FILE, FILE_WRITE);
  if (file) {
    file.print(encryptedNotes);
    file.close();
  }
  
  Serial.println("Data encrypted and saved to SD card");
}

String formatNoteLine(const Note& note) {
  // Format: id|title|content|folder|timestamp
  String line = note.id + "|" + 
                escapeDelimiter(note.title) + "|" + 
                escapeDelimiter(note.content) + "|" + 
                escapeDelimiter(note.folder) + "|" + 
                String(note.timestamp);
  return line;
}

Note parseNoteLine(String line) {
  Note note;
  int pos1 = line.indexOf('|');
  int pos2 = line.indexOf('|', pos1 + 1);
  int pos3 = line.indexOf('|', pos2 + 1);
  int pos4 = line.indexOf('|', pos3 + 1);
  
  if (pos1 > 0 && pos2 > pos1 && pos3 > pos2 && pos4 > pos3) {
    note.id = line.substring(0, pos1);
    note.title = unescapeDelimiter(line.substring(pos1 + 1, pos2));
    note.content = unescapeDelimiter(line.substring(pos2 + 1, pos3));
    note.folder = unescapeDelimiter(line.substring(pos3 + 1, pos4));
    note.timestamp = line.substring(pos4 + 1).toInt();
  }
  
  return note;
}

String escapeDelimiter(String str) {
  str.replace("|", "\\|");
  str.replace("\n", "\\n");
  return str;
}

String unescapeDelimiter(String str) {
  str.replace("\\|", "|");
  str.replace("\\n", "\n");
  return str;
}

// Flash storage functions (unencrypted)
void loadDataFromFlash() {
  // Load folders
  int folderCount = preferences.getInt("folderCount", 0);
  for (int i = 0; i < folderCount; i++) {
    String key = "folder" + String(i);
    String folder = preferences.getString(key.c_str(), "");
    if (folder.length() > 0) {
      folders.push_back(folder);
    }
  }
  
  if (folders.size() == 0) {
    folders.push_back("General");
  }
  
  // Load notes
  int noteCount = preferences.getInt("noteCount", 0);
  for (int i = 0; i < noteCount; i++) {
    Note note;
    String prefix = "note" + String(i);
    note.id = preferences.getString((prefix + "id").c_str(), "");
    note.title = preferences.getString((prefix + "title").c_str(), "");
    note.content = preferences.getString((prefix + "content").c_str(), "");
    note.folder = preferences.getString((prefix + "folder").c_str(), "General");
    note.timestamp = preferences.getULong((prefix + "time").c_str(), 0);
    
    if (note.id.length() > 0) {
      notes.push_back(note);
    }
  }
  
  Serial.printf("Loaded %d folders and %d notes from flash\n", folders.size(), notes.size());
}

void saveDataToFlash() {
  preferences.clear();
  
  // Save folders
  preferences.putInt("folderCount", folders.size());
  for (size_t i = 0; i < folders.size(); i++) {
    String key = "folder" + String(i);
    preferences.putString(key.c_str(), folders[i]);
  }
  
  // Save notes
  preferences.putInt("noteCount", notes.size());
  for (size_t i = 0; i < notes.size(); i++) {
    String prefix = "note" + String(i);
    preferences.putString((prefix + "id").c_str(), notes[i].id);
    preferences.putString((prefix + "title").c_str(), notes[i].title);
    preferences.putString((prefix + "content").c_str(), notes[i].content);
    preferences.putString((prefix + "folder").c_str(), notes[i].folder);
    preferences.putULong((prefix + "time").c_str(), notes[i].timestamp);
  }
  
  Serial.println("Data saved to flash");
}

// Unified save function
void saveData() {
  if (useSDCard) {
    saveDataToSD();
  } else {
    saveDataToFlash();
  }
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Notes</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: #36393f;
            color: #dcddde;
            display: flex;
            height: 100vh;
            overflow: hidden;
        }
        
        .sidebar {
            width: 240px;
            background: #2f3136;
            display: flex;
            flex-direction: column;
            border-right: 1px solid #202225;
        }
        
        .sidebar-header {
            padding: 16px;
            background: #27292d;
            border-bottom: 1px solid #202225;
            font-weight: 600;
            font-size: 16px;
        }
        
        .storage-status {
            padding: 8px 16px;
            background: #202225;
            font-size: 11px;
            color: #72767d;
            display: flex;
            align-items: center;
            gap: 6px;
        }
        
        .status-indicator {
            width: 8px;
            height: 8px;
            border-radius: 50%;
            background: #3ba55d;
        }
        
        .status-encrypted {
            background: #5865f2;
        }
        
        .folders {
            flex: 1;
            overflow-y: auto;
            padding: 8px;
        }
        
        .folder-item {
            padding: 8px 12px;
            margin: 2px 0;
            border-radius: 4px;
            cursor: pointer;
            transition: background 0.15s;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        
        .folder-item:hover {
            background: #393c43;
        }
        
        .folder-item.active {
            background: #5865f2;
            color: #fff;
        }
        
        .folder-name {
            flex: 1;
            overflow: hidden;
            text-overflow: ellipsis;
            white-space: nowrap;
        }
        
        .delete-folder {
            opacity: 0;
            color: #ed4245;
            font-size: 18px;
            cursor: pointer;
            padding: 0 4px;
        }
        
        .folder-item:hover .delete-folder {
            opacity: 1;
        }
        
        .add-folder {
            padding: 12px;
            background: #27292d;
            border-top: 1px solid #202225;
        }
        
        .add-folder input {
            width: 100%;
            padding: 8px;
            background: #40444b;
            border: none;
            border-radius: 4px;
            color: #dcddde;
            font-size: 14px;
        }
        
        .main-content {
            flex: 1;
            display: flex;
            flex-direction: column;
        }
        
        .top-bar {
            padding: 16px 20px;
            background: #2f3136;
            border-bottom: 1px solid #202225;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        
        .top-bar h2 {
            font-size: 20px;
            font-weight: 600;
        }
        
        .btn {
            padding: 8px 16px;
            background: #5865f2;
            color: white;
            border: none;
            border-radius: 4px;
            cursor: pointer;
            font-size: 14px;
            font-weight: 500;
            transition: background 0.15s;
        }
        
        .btn:hover {
            background: #4752c4;
        }
        
        .btn-danger {
            background: #ed4245;
        }
        
        .btn-danger:hover {
            background: #c03537;
        }
        
        .btn-success {
            background: #3ba55d;
        }
        
        .btn-success:hover {
            background: #2d7d46;
        }
        
        .notes-container {
            flex: 1;
            overflow-y: auto;
            padding: 20px;
        }
        
        .note-card {
            background: #2f3136;
            border-radius: 8px;
            padding: 16px;
            margin-bottom: 12px;
            border: 1px solid #202225;
            position: relative;
        }
        
        .note-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 12px;
        }
        
        .note-title {
            font-size: 18px;
            font-weight: 600;
            color: #fff;
        }
        
        .note-actions {
            display: flex;
            gap: 8px;
        }
        
        .note-content {
            color: #b9bbbe;
            line-height: 1.5;
            white-space: pre-wrap;
        }
        
        .note-time {
            color: #72767d;
            font-size: 12px;
            margin-top: 8px;
        }
        
        .modal {
            display: none;
            position: fixed;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background: rgba(0, 0, 0, 0.85);
            align-items: center;
            justify-content: center;
            z-index: 1000;
        }
        
        .modal.active {
            display: flex;
        }
        
        .modal-content {
            background: #36393f;
            border-radius: 8px;
            padding: 24px;
            max-width: 500px;
            width: 90%;
            max-height: 80vh;
            overflow-y: auto;
        }
        
        .modal-header {
            font-size: 20px;
            font-weight: 600;
            margin-bottom: 20px;
        }
        
        .form-group {
            margin-bottom: 16px;
        }
        
        .form-group label {
            display: block;
            margin-bottom: 8px;
            color: #b9bbbe;
            font-size: 14px;
            font-weight: 500;
        }
        
        .form-group input,
        .form-group textarea,
        .form-group select {
            width: 100%;
            padding: 10px;
            background: #40444b;
            border: 1px solid #202225;
            border-radius: 4px;
            color: #dcddde;
            font-size: 14px;
            font-family: inherit;
        }
        
        .form-group textarea {
            min-height: 150px;
            resize: vertical;
        }
        
        .modal-footer {
            display: flex;
            gap: 12px;
            justify-content: flex-end;
            margin-top: 20px;
        }
        
        .empty-state {
            text-align: center;
            padding: 60px 20px;
            color: #72767d;
        }
        
        .empty-state h3 {
            font-size: 20px;
            margin-bottom: 8px;
        }
        
        ::-webkit-scrollbar {
            width: 8px;
        }
        
        ::-webkit-scrollbar-track {
            background: #2f3136;
        }
        
        ::-webkit-scrollbar-thumb {
            background: #202225;
            border-radius: 4px;
        }
        
        ::-webkit-scrollbar-thumb:hover {
            background: #18191c;
        }
    </style>
</head>
<body>
    <div class="sidebar">
        <div class="sidebar-header">Folders</div>
        <div class="storage-status">
            <span class="status-indicator" id="statusIndicator"></span>
            <span id="storageStatus">Loading...</span>
        </div>
        <div class="folders" id="folderList"></div>
        <div class="add-folder">
            <input type="text" id="newFolderInput" placeholder="New folder..." onkeypress="if(event.key==='Enter') addFolder()">
        </div>
    </div>
    
    <div class="main-content">
        <div class="top-bar">
            <h2 id="currentFolderName">Notes</h2>
            <button class="btn" onclick="openNoteModal()">+ New Note</button>
        </div>
        <div class="notes-container" id="notesContainer"></div>
    </div>
    
    <div class="modal" id="noteModal">
        <div class="modal-content">
            <div class="modal-header">Create Note</div>
            <form onsubmit="saveNote(event)">
                <div class="form-group">
                    <label>Title</label>
                    <input type="text" id="noteTitle" required>
                </div>
                <div class="form-group">
                    <label>Folder</label>
                    <select id="noteFolder" required></select>
                </div>
                <div class="form-group">
                    <label>Content</label>
                    <textarea id="noteContent" required></textarea>
                </div>
                <div class="modal-footer">
                    <button type="button" class="btn btn-danger" onclick="closeNoteModal()">Cancel</button>
                    <button type="submit" class="btn btn-success">Save</button>
                </div>
            </form>
        </div>
    </div>
    
    <script>
        let currentFolder = 'General';
        let folders = [];
        let notes = [];
        
        async function loadStorageStatus() {
            const response = await fetch('/api/status');
            const status = await response.json();
            const statusEl = document.getElementById('storageStatus');
            const indicator = document.getElementById('statusIndicator');
            
            if (status.storage === 'SD') {
                statusEl.textContent = '🔒 SD Card (Encrypted)';
                indicator.classList.add('status-encrypted');
            } else {
                statusEl.textContent = '🔧 ESP32 Flash';
            }
        }
        
        async function loadFolders() {
            const response = await fetch('/api/folders');
            folders = await response.json();
            renderFolders();
            updateFolderSelect();
        }
        
        async function loadNotes() {
            const response = await fetch('/api/notes');
            notes = await response.json();
            renderNotes();
        }
        
        function renderFolders() {
            const list = document.getElementById('folderList');
            list.innerHTML = folders.map(folder => `
                <div class="folder-item ${folder === currentFolder ? 'active' : ''}" onclick="selectFolder('${folder}')">
                    <span class="folder-name"># ${folder}</span>
                    ${folder !== 'General' ? `<span class="delete-folder" onclick="event.stopPropagation(); deleteFolder('${folder}')">&times;</span>` : ''}
                </div>
            `).join('');
        }
        
        function updateFolderSelect() {
            const select = document.getElementById('noteFolder');
            select.innerHTML = folders.map(f => `<option value="${f}">${f}</option>`).join('');
        }
        
        function renderNotes() {
            const container = document.getElementById('notesContainer');
            const filteredNotes = notes.filter(n => n.folder === currentFolder);
            
            if (filteredNotes.length === 0) {
                container.innerHTML = '<div class="empty-state"><h3>No notes yet</h3><p>Create your first note to get started!</p></div>';
                return;
            }
            
            container.innerHTML = filteredNotes.map(note => `
                <div class="note-card">
                    <div class="note-header">
                        <div class="note-title">${escapeHtml(note.title)}</div>
                        <div class="note-actions">
                            <button class="btn btn-danger" onclick="deleteNote('${note.id}')">Delete</button>
                        </div>
                    </div>
                    <div class="note-content">${escapeHtml(note.content)}</div>
                    <div class="note-time">${formatTime(note.timestamp)}</div>
                </div>
            `).join('');
        }
        
        function selectFolder(folder) {
            currentFolder = folder;
            document.getElementById('currentFolderName').textContent = `# ${folder}`;
            renderFolders();
            renderNotes();
        }
        
        async function addFolder() {
            const input = document.getElementById('newFolderInput');
            const name = input.value.trim();
            if (!name) return;
            
            const response = await fetch('/api/folders', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'name=' + encodeURIComponent(name)
            });
            
            if (response.ok) {
                input.value = '';
                await loadFolders();
            }
        }
        
        async function deleteFolder(folder) {
            if (!confirm('Delete folder and all its notes?')) return;
            
            await fetch('/api/folders/delete', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'name=' + encodeURIComponent(folder)
            });
            
            if (currentFolder === folder) {
                currentFolder = 'General';
            }
            
            await loadFolders();
            await loadNotes();
        }
        
        function openNoteModal() {
            document.getElementById('noteTitle').value = '';
            document.getElementById('noteContent').value = '';
            document.getElementById('noteFolder').value = currentFolder;
            document.getElementById('noteModal').classList.add('active');
        }
        
        function closeNoteModal() {
            document.getElementById('noteModal').classList.remove('active');
        }
        
        async function saveNote(e) {
            e.preventDefault();
            
            const title = document.getElementById('noteTitle').value;
            const content = document.getElementById('noteContent').value;
            const folder = document.getElementById('noteFolder').value;
            
            await fetch('/api/notes', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: `title=${encodeURIComponent(title)}&content=${encodeURIComponent(content)}&folder=${encodeURIComponent(folder)}`
            });
            
            closeNoteModal();
            await loadNotes();
            await fetch('/api/save', {method: 'POST'});
        }
        
        async function deleteNote(id) {
            if (!confirm('Delete this note?')) return;
            
            await fetch('/api/notes/delete', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'id=' + encodeURIComponent(id)
            });
            
            await loadNotes();
            await fetch('/api/save', {method: 'POST'});
        }
        
        function escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text;
            return div.innerHTML;
        }
        
        function formatTime(timestamp) {
            const date = new Date(timestamp * 1000);
            return date.toLocaleString();
        }
        
        // Initialize
        loadStorageStatus();
        loadFolders();
        loadNotes();
    </script>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
}

void handleGetNotes() {
  String json = "[";
  for (size_t i = 0; i < notes.size(); i++) {
    if (i > 0) json += ",";
    json += "{";
    json += "\"id\":\"" + notes[i].id + "\",";
    json += "\"title\":\"" + jsonEscape(notes[i].title) + "\",";
    json += "\"content\":\"" + jsonEscape(notes[i].content) + "\",";
    json += "\"folder\":\"" + jsonEscape(notes[i].folder) + "\",";
    json += "\"timestamp\":" + String(notes[i].timestamp);
    json += "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleCreateNote() {
  Note note;
  note.id = String(millis());
  note.title = server.arg("title");
  note.content = server.arg("content");
  note.folder = server.arg("folder");
  note.timestamp = millis() / 1000;
  
  notes.push_back(note);
  server.send(200, "text/plain", "OK");
}

void handleDeleteNote() {
  String id = server.arg("id");
  for (size_t i = 0; i < notes.size(); i++) {
    if (notes[i].id == id) {
      notes.erase(notes.begin() + i);
      break;
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleGetFolders() {
  String json = "[";
  for (size_t i = 0; i < folders.size(); i++) {
    if (i > 0) json += ",";
    json += "\"" + jsonEscape(folders[i]) + "\"";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleCreateFolder() {
  String name = server.arg("name");
  if (name.length() > 0) {
    folders.push_back(name);
    saveData();
  }
  server.send(200, "text/plain", "OK");
}

void handleDeleteFolder() {
  String name = server.arg("name");
  
  // Remove folder
  for (size_t i = 0; i < folders.size(); i++) {
    if (folders[i] == name) {
      folders.erase(folders.begin() + i);
      break;
    }
  }
  
  // Remove all notes in that folder
  for (int i = notes.size() - 1; i >= 0; i--) {
    if (notes[i].folder == name) {
      notes.erase(notes.begin() + i);
    }
  }
  
  saveData();
  server.send(200, "text/plain", "OK");
}

void handleSaveData() {
  saveData();
  server.send(200, "text/plain", "Saved");
}

void handleGetStatus() {
  String json = "{";
  json += "\"storage\":\"" + String(useSDCard ? "SD" : "Flash") + "\",";
  json += "\"encrypted\":" + String(useSDCard ? "true" : "false") + ",";
  json += "\"noteCount\":" + String(notes.size()) + ",";
  json += "\"folderCount\":" + String(folders.size());
  json += "}";
  server.send(200, "application/json", json);
}

String jsonEscape(String str) {
  str.replace("\\", "\\\\");
  str.replace("\"", "\\\"");
  str.replace("\n", "\\n");
  str.replace("\r", "\\r");
  str.replace("\t", "\\t");
  return str;
}