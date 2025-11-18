#include "ESP32FtpServer.h"
#include <WiFi.h>

WiFiServer ftpServer(FTP_CTRL_PORT);
WiFiServer dataServer(FTP_DATA_PORT_PASV);

FtpServer::FtpServer() {}

void FtpServer::begin(String uname, String pword) {
    _FTP_USER = uname;
    _FTP_PASS = pword;
    ftpServer.begin();
    dataServer.begin();
    millisTimeOut = (uint32_t)FTP_TIME_OUT * 60 * 1000;
    millisDelay = 0;
    cmdStatus = 0;
    iniVariables();
}

void FtpServer::iniVariables() {
    dataPort = FTP_DATA_PORT_PASV;
    dataPassiveConn = true;
    strcpy(cwdName, "/");
    rnfrCmd = false;
    transferStatus = 0;
    lastLogTime = 0;
    bytesTransfered = 0;
}

// String FtpServer::getRealpath(const String& path) {
//     if (path.length() == 0) return String(FTP_MOUNT_POINT) + "/";
//     if (path[0] == '/') return String(FTP_MOUNT_POINT) + path;
//     return String(FTP_MOUNT_POINT) + "/" + path;
// }

int FtpServer::handleFTP() {
    if ((int32_t)(millisDelay - millis()) > 0) return 0;

    if (ftpServer.hasClient()) {
        if (!client.connected()) {
            client = ftpServer.available();
        } else {
            WiFiClient reject = ftpServer.available();
            reject.stop();
        }
    }

    if (cmdStatus == 0) {
        if (client.connected()) disconnectClient();
        cmdStatus = 1;
    } else if (cmdStatus == 1) {
        abortTransfer();
        iniVariables();
        cmdStatus = 2;
    } else if (cmdStatus == 2) {
        if (client.connected()) {
            clientConnected();
            millisEndConnection = millis() + 10000;
            cmdStatus = 3;
        }
    } else if (readChar() > 0) {
        if (cmdStatus == 3) {
            if (userIdentity()) cmdStatus = 4;
            else cmdStatus = 0;
        } else if (cmdStatus == 4) {
            if (userPassword()) {
                cmdStatus = 5;
                millisEndConnection = millis() + millisTimeOut;
            } else cmdStatus = 0;
        } else if (cmdStatus == 5) {
            if (!processCommand()) cmdStatus = 0;
            else millisEndConnection = millis() + millisTimeOut;
        }
    } else if (!client || !client.connected()) {
        cmdStatus = 1;
    }

    if (transferStatus == 1) {
        if (!doRetrieve()) transferStatus = 0;
    } else if (transferStatus == 2) {
        if (!doStore()) transferStatus = 0;
    } else if (cmdStatus > 2 && !((int32_t)(millisEndConnection - millis()) > 0)) {
        client.println("530 Timeout");
        millisDelay = millis() + 200;
        cmdStatus = 0;
    }
    return transferStatus != 0 || cmdStatus != 0;
}

void FtpServer::clientConnected() {
    client.println("220 FTP for Kode Dot Ready");
    iCL = 0;
}

void FtpServer::disconnectClient() {
    abortTransfer();
    client.println("221 Goodbye");
    client.stop();
}

boolean FtpServer::userIdentity() {
    if (strcmp(command, "USER") != 0) { client.println("500 Syntax error"); return true; }
    if (strcmp(parameters, _FTP_USER.c_str()) != 0) { client.println("530 user not found"); return false; }
    client.println("331 OK. Password required");
    strcpy(cwdName, "/");
    return true;
}

boolean FtpServer::userPassword() {
    if (strcmp(command, "PASS") != 0) { client.println("500 Syntax error"); return true; }
    if (!parameters || strcmp(parameters, _FTP_PASS.c_str()) != 0) {
        client.println("530 Login incorrect");
        return false;
    }
    client.println("230 OK.");
    return true;
}

boolean FtpServer::processCommand() {
    if (!strcmp(command, "CDUP")) {
        client.println("250 Ok. Current directory is \"" + String(cwdName) + "\"");
    } else if (!strcmp(command, "CWD")) {
        String dir;
        if (parameters[0] == '/') dir = parameters;
        else if (!strcmp(cwdName, "/")) dir = String("/") + parameters;
        else dir = String(cwdName) + "/" + parameters;

        if (SD_MMC.exists((dir).c_str())) {
            strcpy(cwdName, dir.c_str());
            client.println("250 CWD Ok. \"" + String(dir) + "\"");
        } else {
            client.println("550 directory not found");
        }
    } else if (!strcmp(command, "PWD")) {
        client.println("257 \"" + String(cwdName) + "\"");
    } else if (!strcmp(command, "QUIT")) {
        disconnectClient();
        return false;
    } else if (!strcmp(command, "PASV")) {
        if (data.connected()) data.stop();
        dataIp = WiFi.localIP();
        dataPort = FTP_DATA_PORT_PASV;
        client.println("227 Entering Passive Mode (" +
            String(dataIp[0]) + "," + String(dataIp[1]) + "," +
            String(dataIp[2]) + "," + String(dataIp[3]) + "," +
            String(dataPort >> 8) + "," + String(dataPort & 255) + ").");
        dataPassiveConn = true;
    } else if (!strcmp(command, "LIST") || !strcmp(command, "NLST") || !strcmp(command, "MLSD")) {
        if (!dataConnect()) {
            client.println("425 No data connection");
        } else {
            client.println("150 Accepted data connection");
            File dir = SD_MMC.open((String(cwdName)).c_str());
            if (!dir || !dir.isDirectory()) {
                client.println("550 Can't open directory");
                data.stop();
                return true;
            }
            File file = dir.openNextFile();
            while (file) {
                String fn = file.name();
                if (fn.startsWith(FTP_MOUNT_POINT)) {
                    fn = fn.substring(strlen(FTP_MOUNT_POINT));
                    if (fn.isEmpty()) fn = "/";
                }
                if (fn.startsWith("/")) fn = fn.substring(1);
                if (file.isDirectory()) {
                    data.println("drwxr-xr-x 1 owner group 0 Jan 01 00:00 " + fn);
                } else {
                    data.println("-rw-r--r-- 1 owner group " + String(file.size()) + " Jan 01 00:00 " + fn);
                }
                file = dir.openNextFile();
                yield();
            }
            dir.close();
            client.println("226 Transfer complete");
            data.stop();
        }
    } else if (!strcmp(command, "RETR")) {
        char path[FTP_CWD_SIZE];
        if (makePath(path)) {
            String realPath = (String(path));
            file = SD_MMC.open(realPath.c_str(), "r");
            if (!file) {
                client.println("550 File not found");
            } else if (!dataConnect()) {
                client.println("425 No data connection");
                file.close();
            } else {
                client.println("150 Opening data connection");
                millisBeginTrans = millis();
                lastLogTime = millis();
                bytesTransfered = 0;
                transferStatus = 1;
            }
        }
    } else if (!strcmp(command, "STOR")) {
        char path[FTP_CWD_SIZE];
        if (makePath(path)) {
            String realPath = (String(path));
            file = SD_MMC.open(realPath.c_str(), "w");
            if (!file) {
                client.println("451 Can't create file");
            } else if (!dataConnect()) {
                client.println("425 No data connection");
                file.close();
            } else {
                client.println("150 Ok to send data");
                millisBeginTrans = millis();
                lastLogTime = millis();
                bytesTransfered = 0;
                transferStatus = 2;
            }
        }
    } else if (!strcmp(command, "DELE")) {
        char path[FTP_CWD_SIZE];
        if (makePath(path)) {
            if (SD_MMC.remove((String(path)).c_str())) {
                client.println("250 Deleted");
            } else {
                client.println("450 Delete failed");
            }
        }
    } else if (!strcmp(command, "RMD")) {
        char path[FTP_CWD_SIZE];
        if (makePath(path)) {
            if (SD_MMC.rmdir((String(path)).c_str())) {
                client.println("250 RMD successful");
            } else {
                client.println("550 Remove directory failed");
            }
        }
    } else if (!strcmp(command, "MKD")) {
        String dir;
        if (parameters[0] == '/') dir = parameters;
        else if (!strcmp(cwdName, "/")) dir = String("/") + parameters;
        else dir = String(cwdName) + "/" + parameters;
        if (SD_MMC.mkdir((dir).c_str())) {
            client.println("257 Created");
        } else {
            client.println("550 Create failed");
        }
    } else if (!strcmp(command, "RNFR")) {
        buf[0] = 0;
        if (makePath(buf)) {
            if (SD_MMC.exists((String(buf)).c_str())) {
                client.println("350 RNFR accepted - file exists");
                rnfrCmd = true;
            } else {
                client.println("550 File not found");
            }
        }
    } else if (!strcmp(command, "RNTO")) {
        char path[FTP_CWD_SIZE];
        if (!rnfrCmd) {
            client.println("503 Need RNFR before RNTO");
        } else if (makePath(path)) {
            if (SD_MMC.rename((String(buf)).c_str(), (String(path)).c_str())) {
                client.println("250 File renamed");
            } else {
                client.println("451 Rename failed");
            }
        }
        rnfrCmd = false;
    } else if (!strcmp(command, "TYPE")) {
        client.println("200 TYPE is now 8-bit binary");
    } else if (!strcmp(command, "FEAT")) {
        client.println("211-Features:");
        client.println(" PASV");
        client.println("211 End");
    } else if (!strcmp(command, "SIZE")) {
        char path[FTP_CWD_SIZE];
        if (makePath(path)) {
            File f = SD_MMC.open((String(path)).c_str(), "r");
            if (f) {
                client.println("213 " + String(f.size()));
                f.close();
            } else {
                client.println("550 Not found");
            }
        }
    } else {
        client.println("500 Unknown command");
    }
    return true;
}

boolean FtpServer::dataConnect() {
    if (data.connected()) return true;
    data.stop();
    unsigned long start = millis();
    while (!dataServer.hasClient() && (millis() - start) < 3000) {
        yield();
        if (cmdStatus < 3) return false;
    }
    if (dataServer.hasClient()) {
        data = dataServer.available();
        return data.connected();
    }
    return false;
}

boolean FtpServer::doRetrieve() {
    if (!data.connected()) {
        closeTransfer();
        return false;
    }

    int16_t nb = file.read((uint8_t*)buf, FTP_BUF_SIZE);
    if (nb > 0) {
        data.write((uint8_t*)buf, nb);
        bytesTransfered += nb;

        if (millis() - lastLogTime > 1000) {
            lastLogTime = millis();
            if (millis() > millisBeginTrans) {
                float speed = (float)bytesTransfered / (1024.0 * (millis() - millisBeginTrans) / 1000.0);
                Serial.printf("Downloading... %.2f KB/s\n", speed);
            }
        }
        yield();
        return true;
    }
    closeTransfer();
    return false;
}

boolean FtpServer::doStore() {
    if (!data.connected()) {
        closeTransfer();
        return false;
    }

    int len = data.available();
    if (len > 0) {
        if (len > FTP_BUF_SIZE) len = FTP_BUF_SIZE;
        int nb = data.read((uint8_t*)buf, len);
        if (nb > 0) {
            file.write((uint8_t*)buf, nb);
            bytesTransfered += nb;

            if (millis() - lastLogTime > 1000) {
                lastLogTime = millis();
                if (millis() > millisBeginTrans) {
                    float speed = (float)bytesTransfered / (1024.0 * (millis() - millisBeginTrans) / 1000.0);
                    Serial.printf("Uploading... %.2f KB/s\n", speed);
                }
            }
        }
        yield();
        return true;
    }
    closeTransfer();
    return false;
}

void FtpServer::closeTransfer() {
    uint32_t deltaT = millis() - millisBeginTrans;
    if (deltaT > 0 && bytesTransfered > 0 && deltaT < 1000000) {
        float speed = (float)bytesTransfered / (1024.0 * deltaT / 1000.0);
        client.println("226-File successfully transferred");
        client.println("226 " + String(deltaT) + " ms, " + String(speed, 2) + " KB/s");
        Serial.printf("Transfer Done: %u bytes in %u ms (%.2f KB/s)\n", bytesTransfered, deltaT, speed);
    } else {
        client.println("226 File successfully transferred");
        Serial.println("Transfer Done.");
    }
    file.close();
    data.stop();
}

void FtpServer::abortTransfer() {
    if (transferStatus > 0) {
        file.close();
        data.stop();
        client.println("426 Aborted");
    }
    transferStatus = 0;
}

int8_t FtpServer::readChar() {
    int8_t rc = -1;
    if (client.available()) {
        char c = client.read();
        if (c == '\\') c = '/';

        if (c == '\n') {
            if (iCL >= FTP_CMD_SIZE - 1) {
                iCL = 0;
                client.println("500 Command too long");
                return -2;
            }
            cmdLine[iCL] = 0;
            command[0] = 0;
            parameters = NULL;

            if (iCL == 0) {
                rc = 0;
            } else {
                rc = iCL;
                parameters = strchr(cmdLine, ' ');
                if (parameters != NULL) {
                    size_t cmdLen = parameters - cmdLine;
                    if (cmdLen > 4) {
                        rc = -2;
                    } else {
                        strncpy(command, cmdLine, cmdLen);
                        command[cmdLen] = 0;
                        while (*(++parameters) == ' ');
                    }
                } else if (strlen(cmdLine) > 4) {
                    rc = -2;
                } else {
                    strcpy(command, cmdLine);
                }
                iCL = 0;
            }

            if (rc > 0) {
                for (uint8_t i = 0; i < strlen(command); i++) {
                    command[i] = toupper(command[i]);
                }
            }
            if (rc == -2) {
                iCL = 0;
                client.println("500 Syntax error");
            }
        } else if (c != '\r') {
            if (iCL < FTP_CMD_SIZE - 1) {
                cmdLine[iCL++] = c;
            } else {
                rc = -2;
            }
        }
    }
    return rc;
}

boolean FtpServer::makePath(char* fullName) {
    return makePath(fullName, parameters);
}

boolean FtpServer::makePath(char* fullName, char* param) {
    if (!fullName || !param) return false;
    fullName[0] = '\0';

    if (strcmp(param, "/") == 0 || strlen(param) == 0) {
        strcpy(fullName, "/");
        return true;
    }

    if (param[0] != '/') {
        size_t len = strlen(cwdName);
        if (len + 1 + strlen(param) >= FTP_CWD_SIZE - 1) return false;
        strcpy(fullName, cwdName);
        if (len > 0 && fullName[len - 1] != '/') strcat(fullName, "/");
        strncat(fullName, param, FTP_CWD_SIZE - strlen(fullName) - 1);
    } else {
        if (strlen(param) >= FTP_CWD_SIZE) return false;
        strcpy(fullName, param);
    }
    return true;
}