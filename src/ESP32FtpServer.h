#ifndef FTP_SERVERESP_H
#define FTP_SERVERESP_H

#include "SD_MMC.h" 
#include <FS.h>
#include <WiFiClient.h>

#define FTP_SERVER_VERSION "FTP-KodeDot-v2"
#define FTP_CTRL_PORT    21
#define FTP_DATA_PORT_PASV 50009
#define FTP_TIME_OUT  5
#define FTP_CMD_SIZE 255 + 8
#define FTP_CWD_SIZE 255 + 8
#define FTP_FIL_SIZE 255
#define FTP_BUF_SIZE 4096 

// VITAL: Must match the mount point used in main.cpp
#define FTP_MOUNT_POINT "/sdcard"

class FtpServer {
  public:
    FtpServer();
    void    begin(String uname, String pword);
    int     handleFTP();

  private:
    void    iniVariables();
    void    clientConnected();
    void    disconnectClient();
    boolean userIdentity();
    boolean userPassword();
    boolean processCommand();
    boolean dataConnect();
    boolean doRetrieve();
    boolean doStore();
    void    closeTransfer();
    void    abortTransfer();
    boolean makePath( char * fullname );
    boolean makePath( char * fullName, char * param );
    int8_t  readChar();
    
    // Helper to convert virtual FTP path "/" to physical path "/sdcard/"
    String  getRealpath(const char* path);

    IPAddress      dataIp;
    WiFiClient client;
    WiFiClient data;
    File file;

    boolean  dataPassiveConn;
    uint16_t dataPort;
    char     buf[ FTP_BUF_SIZE ];
    char     cmdLine[ FTP_CMD_SIZE ];
    char     cwdName[ FTP_CWD_SIZE ];
    char     command[ 5 ];
    boolean  rnfrCmd;
    char * parameters;
    uint16_t iCL;
    int8_t   cmdStatus, transferStatus;
    uint32_t millisTimeOut, millisDelay, millisEndConnection;
    // Speed calculation variables
    uint32_t millisBeginTrans, lastLogTime, bytesTransfered;
    String   _FTP_USER;
    String   _FTP_PASS;
};
#endif