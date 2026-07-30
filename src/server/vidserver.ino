//  Use an AtomS3R, an Atomic SD Card base, and Tail Battery as a file server 
//  for MJPEG videos. The videos play on a client running on an M5 
//  Stopwatch.
//
//  This is part of a baby gender reveal project.
//
//  The M5 Stopwatch will request a file to be opened. The AtomS3R will 
//  populate as many frames as possible into a 512KiB buffer (in PSRAM),
//  which is transmitted over a private network. The M5 Stopwatch requests
//  the next batch of frames until the entire file has been read from the 
//  SD Card.
//
//  SD Card rules:
//
//  1.  The SD Card must be formatted for FAT32. Use the largest allocation
//      block size the card allows as this will improve read performance.
//
//  2.  Limit filenames to 8.3 format in upper case e.g. FILE.TXT. This prevents
//      the filenames taking more space on the SD Card than needed.
//
//  3.  The Atomic TF Card Base has a 16GB limit on the size of the SD Card, but 
//      32GB and 64GB cards seem to work. 
//
//  4.  File data over 4GiB cannot be accessed as file pointers are capped
//      at 32-bits. Keep file sizes under 4GiB.
//
//  The AtomS3R can run ~1 hour on a full Tail Battery charge.
//
//  License:  MIT
//

// Force maximum speed optimizations
#pragma GCC push_options
#pragma GCC optimize ("-O3")

//  Note: the SD.h and SPI.h files must be included before
//        the M5Unified.h file
#include <SD.h>
#include <SPI.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <AsyncTCP.h>

#include <atomic>
#include <queue>

#define DEBUG 1
#if (DEBUG)
#define DEBUGOUT(fmt, ...)  Serial.printf(fmt, ##__VA_ARGS__);
#else   // DEBUG
#define DEBUGOUT(fmt, ...)
#endif  // DEBUG

//  Buffer sizes
constexpr size_t bufSize    = 512*1024;
constexpr size_t chunkSize  = 2*1024;

//  File buffer
uint8_t *buffer = nullptr;

//  Server and AP information
#define SVPORT  8080
#define SVHOST  "vserver"
#define APSSID  "vserver"
#define APPASS  "topsecret"
#define APCHAN  1

//  Our only client
AsyncClient* ourClient = nullptr;

//  Pins for SD Card access
#define SD_SPI_CS_PIN   -1
#define SD_SPI_SCK_PIN  7
#define SD_SPI_MISO_PIN 8
#define SD_SPI_MOSI_PIN 6

//  File to read
fs::File inFile;

//  Requests from the payload
enum class requests : uint8_t
{
  openfile,
  sendfile,
  closefile,
  idle,
};

//  Application states
enum class appStates : uint8_t
{
  uninit,
  waitclient,
  waitrequest,
  sendchunk,
  waitack,
  clearqueue,
  idle,
};

appStates appStatus = appStates::uninit;
appStates ackStatus = appStates::uninit;

//  When we reach the EOF of the current file, we indicate 
//  there is no data by sending a blank JPEG frame
char noData [] = {0xFF, 0xD8, 0xFF, 0xD9};

//  Send queue entry
//
//  The send queue solves a lot of network communication problems.
//
//  Once a file has been opened, the client can send a read request at any time as 
//  it's easy for the client to burn through JPEG frames very quickly. A single 
//  structure cannot handle multiple requests.
//
//  The AsyncTCP buffer might have 5KiB (or so) available, but only sends data 
//  in 2KiB chunks. The sender can move 5KiB into the AsyncTCP buffer, but
//  needs to wait for that 5KiB to drain before moving the next chunk.
//
//  Here's an example walkthrough of transferring data between the server
//  and client (assume a file has been opened):
//
//  Client                      Server
//  ======                      ======
//
//  Request 64KiB   ---->       Push new request onto the send queue
//                                chunkInfo { bytesWanted = 64KiB, bytesRead = 0, 
//                                              bytesSent = 0, drainBytes = 0, drainWait = 0}
//                  <----       ACK
//
//                              Read 64KiB from SD Card, get available space in AsyncTCP (5KiB),
//                              give 5KiB to AsyncTCP and wait to drain
//                                chunkInfo { bytesWanted = 64KiB, bytesRead = 64KiB, 
//                                              bytesSent = 5KiB, drainBytes = 0, drainWait = 5KiB}
//                  <----       Data packet [2KiB]
//
//  ACK [2KiB]      ----->      Get the ACK and adjust the drainBytes with length of the ACK
//                              packet.
//                                chunkInfo { bytesWanted = 64KiB, bytesRead = 64KiB, 
//                                              bytesSent = 5KiB, drainBytes = 2KiB, drainWait = 5KiB}
//                  <----       Data packet [2KiB]
//
//  ACK [2KiB]      ----->      Get the ACK and adjust the drainBytes with length of the ACK
//                              packet.
//                                chunkInfo { bytesWanted = 64KiB, bytesRead = 64KiB, 
//                                              bytesSent = 5KiB, drainBytes = 4KiB, drainWait = 5KiB}
//                  <----       Data packet [1KiB]
//
//  ACK [1KiB]      ----->      Get the ACK and adjust the drainBytes with length of the ACK
//                              packet.
//                                chunkInfo { bytesWanted = 64KiB, bytesRead = 64KiB, 
//                                              bytesSent = 5KiB, drainBytes = 5KiB, drainWait = 5KiB}
//                              Drained data now fully sent, so the next 5KiB block can go
//                                chunkInfo { bytesWanted = 64KiB, bytesRead = 64KiB, 
//                                              bytesSent = 10KiB, drainBytes = 0, drainWait = 5KiB}
//                  <----       Data packet [2KiB]
//
//  This sequence continues until chunkInfo.bytesRead == chunkInfo.bytesSent. Once this goal is reached,
//  the active send requst is popped off queue, and the next send request becomes active.
//
//  The client never requests more that the total buffer size. The server may read less than the client
//  requested (bytesWanted). This usually happens when the last part of the file is being read and the
//  client asks for 10KiB, but there is only 2KiB left in the file. The client has no about file sizes,
//  and is send a blank JPEG frame to indicate the EOF has been reached.
//

typedef struct chunkInfo
{
  int bytesWanted;
  int bytesRead;
  int bytesSent;
  int drainBytes;
  int drainWait;
} chunkInfo_t;

std::queue<chunkInfo_t> sendQueue;

//  The std::queue is not thread safe, and we can get concurrent access 
//  from the main application thread and network callbacks, which means
//  certain parts of the send queue require locks
std::mutex sendLock;
std::mutex drainLock;


//  Mount the SD Card
bool MountSDCard (void)
{
  DEBUGOUT("Attempt mount\n");

  // SD Card Initialization
  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) 
  {
    DEBUGOUT("SD card not detected\n");
    return false;
  } 

  DEBUGOUT("SD card detected\n");
  return true;
}

void ResetCounters (void)
{
  if (inFile)
  {
    inFile.close();
  }
}

//  Add a read request to the send queue
void AddSendQueue (chunkInfo_t &sendInfo)
{
  std::lock_guard<std::mutex> lock(sendLock);
  sendQueue.push(sendInfo);
}

//  Get the first request from the send queue
bool GetSendQueue (chunkInfo_t *&sendData)
{
  std::lock_guard<std::mutex> lock(sendLock);
  if (sendQueue.empty())
  {
    return false;
  }

  sendData = &sendQueue.front();
  return true;
}


//  Remove a request from the send queue
void PopSendQueue ()
{
  std::lock_guard<std::mutex> lock(sendLock);
  sendQueue.pop();
  DEBUGOUT("Pop send queue %d\n", sendQueue.empty());
}


//  Clear the entire send queue
void ClearSendQueue ()
{
  std::lock_guard<std::mutex> lock(sendLock);
  sendQueue = {};
}


//  Set the number of bytes waiting to drain from
//  the active send request
void SetDrainBytes (chunkInfo_t *sendInfo, int value)
{
  std::lock_guard<std::mutex> lock(drainLock);
  sendInfo->drainBytes = value;
}


//  Get the number of bytes still waiting to drain
//  from the active send request
int GetDrainBytes (chunkInfo_t *sendInfo)
{
  std::lock_guard<std::mutex> lock(drainLock);
  return sendInfo->drainBytes;
}


//  Adjust the number of bytes waiting to drain from
//  the active send request
void AdjustDrainBytes (chunkInfo_t *sendInfo, int value)
{
  std::lock_guard<std::mutex> lock(drainLock);
  sendInfo->drainBytes += value;
}


//  Network error callback
void NWError (void *arg, AsyncClient *client, int8_t error) 
{
  //  TODO: No errors during testing. Need to implement when there is
  //        a real test case 
	DEBUGOUT("%s: %s\n", __FUNCTION__, client->errorToString(error));
}


//  Network timeout callback
void NWTimeOut (void *arg, AsyncClient *client, uint32_t time) 
{
  //  TODO: No timeouts during testing. Need to implement when there is
  //        a real test case 
	DEBUGOUT("%s: Timeout detected\n", __FUNCTION__);
}


//  Received input data from the client. This is usually a request
//  to open or close a file, or send the next buffer
void NWData (void *arg, AsyncClient *client, void *data, size_t len) 
{
  auto payload = (uint8_t *) data;

  switch (requests(*payload))
  {
    //  The client wants to open a file
    case requests::openfile:
    {
      String fname ((const char *) payload + 1);
      inFile = SD.open(fname);
      if (!inFile)
      {
        DEBUGOUT("Could not open %s\n", fname.c_str());
        ResetCounters();
        client->close();
      }

      appStatus = appStates::waitrequest;
      return;
    }

    //  The client wants us to send a file
    case requests::sendfile:
    {
      size_t bytesAsked;
      memcpy(&bytesAsked, payload + 1, sizeof(size_t));
      DEBUGOUT("Asked to send file %d\n", bytesAsked);

      chunkInfo_t sendInfo;
      memset(&sendInfo, 0, sizeof(sendInfo));

      //  Add the request to a send queue
      sendInfo.bytesWanted = bytesAsked;
      AddSendQueue(sendInfo);
      return;
    }

    //  The client wants us to close the current file
    case requests::closefile:
      DEBUGOUT("Asked to close file\n");
      ResetCounters();
      ClearSendQueue();
      appStatus = appStates::waitrequest;
      return;

    //  The client wants us to go idle. This happens
    //  when someone presses one of the reveal buttons
    //  and the next phase needs to start
    case requests::idle:
      appStatus = appStates::waitrequest;
      ackStatus = appStatus;
      ClearSendQueue();
      return;
  }
}


//  Client disconnected callback
void NWDisconnect (void *arg, AsyncClient *client) 
{
	DEBUGOUT("%s: Client %s disconnected\n", __FUNCTION__, client->remoteIP().toString().c_str());
  client->close();
  ResetCounters();
  ourClient = nullptr;
  appStatus = appStates::waitclient;
}


//  Client received our last message
void NWAck (void *arg, AsyncClient *client, size_t len, uint32_t time) 
{
  chunkInfo_t* sendInfo;
  if (GetSendQueue(sendInfo))
  {
    //  Reduce the number of bytes waiting to drain. The network ACK sends
    //  the data length the client received, which we adjust off the
    //  drained amount
    AdjustDrainBytes(sendInfo, len);
    DEBUGOUT("Drain bytes %d, wait %d\n", GetDrainBytes(sendInfo), sendInfo->drainWait);
  }
  appStatus = ackStatus;
}


//  New client connected
void NewClient (void *arg, AsyncClient *client) 
{
	DEBUGOUT("New client connected, ip: %s", client->remoteIP().toString().c_str());

  if (ourClient != nullptr)
  {
    //  We only allow one client at a time
    DEBUGOUT("Already have a client\n");
    client->close();
    return;
  }

	//  Register network event callbacks
	client->onData(&NWData, nullptr);
	client->onError(&NWError, nullptr);
	client->onDisconnect(&NWDisconnect, nullptr);
	client->onTimeout(&NWTimeOut, nullptr);
  client->onAck(&NWAck, nullptr);

	//  Save the client pointer
  ourClient = client;

  //  Set correct state
  appStatus = appStates::waitrequest;
}


//  Send the next chunk of data
void SendChunk (chunkInfo_t *sendInfo)
{
  if (sendInfo->bytesRead == 0)
  {
    //  No bytes read from the active file. We need to read as
    //  many as possible
    sendInfo->bytesRead = inFile.read(buffer, sendInfo->bytesWanted);
    if (sendInfo->bytesRead != sendInfo->bytesWanted)
    {
      DEBUGOUT("Wanted %d, read %d\n", sendInfo->bytesWanted, sendInfo->bytesRead);
    }
    else
    {
      DEBUGOUT("Read %d\n", sendInfo->bytesRead);
    }
  }

  if (GetDrainBytes(sendInfo) >= sendInfo->drainWait)
  {
    //  No more bytes to drain
    SetDrainBytes(sendInfo, 0);
    sendInfo->drainWait = 0;
  }

  if (GetDrainBytes(sendInfo) != sendInfo->drainWait)
  {
    //  Waiting for data to drain, so sleep a little
    vTaskDelay(pdMS_TO_TICKS(100)); 
    return;
  }

  if (sendInfo->bytesRead == sendInfo->bytesSent)
  {
    //  Finished sending - can remove the active send request
    PopSendQueue();

    if (sendQueue.empty() && inFile.available() == 0)
    {
      //  End of file
      DEBUGOUT("End of file\n");
      ourClient->write(noData, sizeof(noData));
      ackStatus = appStates::clearqueue;    
      appStatus = appStates::waitack;
      return;
    }

    appStatus = appStates::waitrequest;
    return;
  }

  //  Send data
  if (ourClient->space() > 0)
  {
    //  The network has space in the send buffer. Determine how
    //  much we can send
    int space = ourClient->space();
    auto remaining = sendInfo->bytesRead - sendInfo->bytesSent; 
    auto toSend = min(space, remaining);

    SetDrainBytes(sendInfo, 0);

    auto sent = ourClient->write((const char*) buffer + sendInfo->bytesSent, toSend);
    if (sent == 0)
    {
        DEBUGOUT("Did not send any bytes");
        appStatus = appStates::idle;
        return;
    }

    //  Adjust total bytes sent        
    sendInfo->drainWait = sent;
    sendInfo->bytesSent += sent;
  }
}


void setup (void) 
{
  //  Initialize the AtomS3R
  M5.begin();
  Serial.begin(9600);

  if (!MountSDCard())
  {
    //  Could not mount the SD Card
    M5.Display.clear(TFT_RED);
    while (true)
      delay(1000);
  }

  buffer = (uint8_t *) ps_malloc(bufSize + 16);
  if (buffer == nullptr)
  {
    //  Unable to allocate the file buffer
    DEBUGOUT("Could not allocate buffer\n");
    M5.Display.clear(TFT_RED);
    while (true)
      delay(1000);
  }

  //  Create an access point
  DEBUGOUT("Creating AP...");
	while (!WiFi.softAP(APSSID, APPASS, APCHAN, false))
  {
    DEBUGOUT(".");
		delay(500);
	}
  DEBUGOUT("\n");

  //  Start server
  AsyncServer* server = new AsyncServer(SVPORT);
	server->onClient(&NewClient, server);
	server->begin();

  //  Disable the screen to save more power
  M5.Display.sleep();
  M5.Display.powerSaveOn();

  //  Set correct status
  appStatus = appStates::waitclient;

  DEBUGOUT("Ready...\n");
}


void loop (void)
{
  M5.update();

  chunkInfo_t* sendInfo;
  if (GetSendQueue(sendInfo))
  {
    appStatus = appStates::sendchunk;
    ackStatus = appStatus;
  }
  
  switch (appStatus)
  {
    //  Send the next chunk of data
    case appStates::sendchunk:
      SendChunk(sendInfo);
      break;

    //  Stop sending and wait for next client request
    case appStates::clearqueue:
      ClearSendQueue();
      appStatus = appStates::waitrequest;
      ackStatus = appStatus;
      break;

    case appStates::waitack:
      //  TODO:  Add timeout here
      break;
  }
}

#pragma GCC pop_options

