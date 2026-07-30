//  Use an AtomS3R, an Atomic SD Card base, and Tail Battery as a file server 
//  for MJPEG videos. The videos play on a client running on an M5 
//  Stopwatch.
//
//  This is part of a baby gender reveal project.
//
//  The M5 Stopwatch has magnets, on the back plate, which can be used
//  to wear the M5 Stopwatch as a badge with a strip of iron or another
//  magnet. The idea is for someone to wear the M5 Stopwatch and walk
//  around the reveal party to build some momentum. At the right moment,
//  pressing one of the M5 Stopwatch buttons will play a video revealing
//  the gender of the baby.
//
//  The M5 Stopwatch will request a file to be opened. The AtomS3R will 
//  populate as many frames as possible into a 512KiB buffer (in PSRAM),
//  which is transmitted over a private network. The M5 Stopwatch requests
//  the next batch of frames until the entire file has been read from the 
//  SD Card.
//
//  MJPEG videos are a sequence of JPEG images. This requires an MP4 video
//  to be resized and converted to MJPEG format. On Ubuntu Linux, this can 
//  be done by installing the ffmpeg package (sudo apt install ffmpeg) and 
//  then running the following commands:
//
//    ffmpeg -y -i input.mp4 -vf scale="300:300" tmp.mp4
//    ffmpeg -y -i tmp.mp4 -vcodec mjpeg -q:v 31 -r 25 output.mjpeg
//
//  The first ffmpeg command rescales the MP4 input to 300 x 300 pixels, 
//  which fits nicely on the M5 Stopwatch screen.
//
//  The second ffmpeg command converts the MP4 to MJPEG format, drops the 
//  quality to the lowest possible (-q:v 31) and uses 25 frames/sec (-r 25).
//
//  Note that ffmpeg needs the correct file extension to be used on input and
//  output files, so don't change the .mpg or .mjpeg extensions.
//
//  After connecting to the AtomS3R private access point, the M5 Stopwatch requests
//  a full 512KiB buffer from the server. This helps provide a good upfront video
//  buffer for playback. 
//
//  During this time, a 5 second countdown is displayed and then a sequence of
//  colored question mark symbols (?) are output to give the user something to
//  look at while the buffer is filling. 
//
//  From testing, the buffer fills around ~2KiB a request, so 512KiB takes a while.
//
//  The initially, the file GUESS.MJP is shown. The sample video is ~4 minutes and
//  is a collecting of animations and text to keep the audience guessing. This
//  video will repeat on a loop until some presses the blue button (boy) or the
//  yellow (almost peach) button (girl). This triggers an intro video (INTRO.MJP), 
//  which is ~30 seconds, followed by ~1-2 minutes of boy (BOY.MJP) or girl (GIRL.MJP)
//  specific video for the actual reveal.
//
//  Once the reveal video completes, a loop video will play for a boy (BLOOP.MJP) or 
//  girl (GLOOP.MJP).
//
//  The M5 Stopwatch has good battery life, but the AtomS3R only lasts ~1 hour on
//  a fully charges Tail Battery. 
//
//  If the one of the reveal buttons is pressed by accident, you have ~30 seconds to
//  power off the M5 Stopwatch by pressing the power button (red button) twice in a
//  row quickly. The AtomS3R can then be power cycled and the M5 Stopwatch powered
//  on (red button).
//
//  The AtomS3R and M5 Stopwatch should be kept close together (1-2M). It's possible
//  to get farther away, but the WiFi signal is not that strong.
//
//  Plan accordingly if you ever decide to use this at a reveal party.
//
//  License:  MIT
//

// Force maximum speed optimizations
#pragma GCC push_options
#pragma GCC optimize ("-O3")

#include <M5Unified.h>
#include <WiFi.h>
#include <AsyncTCP.h>

#include <atomic>

#define DEBUG 0
#if (DEBUG)
#define DEBUGOUT(fmt, ...)  Serial.printf(fmt, ##__VA_ARGS__);
#else   // DEBUG
#define DEBUGOUT(fmt, ...)
#endif  // DEBUG

//  Buffer sizes
constexpr size_t  bufSize     = 512*1024;

//  Frame rates and delay
constexpr int     msPerSec    = 1000;
constexpr int     framePerSec = 25;
constexpr int     msPerFrame  = msPerSec / framePerSec;

//  Countdown time
constexpr int     CDStart     = 5;

//  Current buffer used, offset and buffer pointers
//
//  Note that this client can burn through a lot of frames
//  and is constantly requesting and receiving data from the
//  server.
//
//  As buffer access occurs on a mixture of main application thread,
//  video playback thread and AsyncTCP callbacks, some variables
//  are marked as atomic.
//
bool        askData     = false;
std::atomic<size_t>
            buffXfer    = 0;
size_t      buffOffset  = 0;
uint8_t    *buffer      = nullptr;

std::mutex  moveLock;

//  Common output buffer to send request codes
//  to the AtomS3R server
char outBuffer [16] = {0x0};

//  The network interface to the AtomS3R server
AsyncClient *ourClient = nullptr;

//  Server and AP information
#define SVPORT  8080
#define SVHOST  "vserver"
#define APSSID  "vserver"
#define APPASS  "topsecret"

//  Requests
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
  connectserver,
  waitconnection,
  startreveal,
  countdown,
  openteaser,
  checkteaser,
  batchteaser,
  playteaser,
  endreveal,
  boysync,
  boyopen,  
  boybatch,
  boyreveal,
  boyloopopen,
  boyloopbatch,
  boyloop,
  girlsync,
  girlopen,  
  girlbatch,
  girlreveal,
  girlloopopen,
  girlloopbatch,
  girlloop,
  waitack,
  idle
};

appStates appStatus = appStates::uninit;
appStates ackStatus = appStates::uninit;

void ResetCounters (void)
{
  askData     = false;
  buffXfer    = 0;
  buffOffset  = 0;
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


//  Client disconnected callback
void NWDisconnect (void *arg, AsyncClient *client) 
{
	DEBUGOUT("%s: Client %s disconnected\n", __FUNCTION__, client->remoteIP().toString().c_str());
  //client->close();
  ResetCounters();
  delete(ourClient);
  ourClient = nullptr;
  appStatus = appStates::connectserver;
  delay(1000);
}


//  Received input data from the server. This is usually a the next
//  buffer we requested
void NWData (void *arg, AsyncClient *client, void *data, size_t len) 
{
  auto remaining = bufSize - buffXfer.load();
  if (len <= remaining)
  {
    //  Enough room in current buffer
    moveLock.lock();
    memcpy(buffer + buffXfer, data, len);
    buffXfer += len;
    moveLock.unlock();
  }
  else
  {
    //  TODO: Need to save the dropped buffer, so it can be added when
    //        space become available
    DEBUGOUT("Not enough room in buffer xfer %d offset %d\n", buffXfer.load(), buffOffset);
  }
}


//  Server received our last message
void NWAck (void *arg, AsyncClient *client, size_t len, uint32_t time) 
{
  //  Move the application onto the next stage
  appStatus = ackStatus;
}


//  We connected to the AtomS3r server
void NWConnect(void* arg, AsyncClient* client) 
{
	DEBUGOUT("$s: Client connected to %s on port %d\n", __FUNCTION__, SVHOST, SVPORT);

  //  Set correct status
  appStatus = appStates::countdown;
}


//  Request the server to open a new file
bool RequestOpen (String fname, appStates ackState)
{
  if (ourClient->space() > fname.length() + 1)
  {
      ackStatus = ackState;
      outBuffer [0] = (char) requests::openfile;
      strcpy(outBuffer + 1, fname.c_str());

      ourClient->write(outBuffer, fname.length() + 2);
      appStatus = appStates::waitack;
      return true;
  }

  return false;
}


//  Close the active video file
void RequestClose (void)
{
  outBuffer [0] = (char) requests::closefile;
  ourClient->write(outBuffer, 1);
}


//  Request the server stops sending data, closes the active 
//  file and goes idle.
//
//  This is used when the teaser video is being played and
//  someone presses one of the reveal buttons
//
void RequestIdle (void)
{
  outBuffer [0] = (char) requests::idle;
  ourClient->write(outBuffer, 1);
}


//  Separate thread to play videos
void PlayVideo (void *params)
{
  bool repeatTeaser = true;

  DEBUGOUT("Video thread started\n");

  while (true)
  {
    switch (appStatus)
    {
      //  In the reveal phase, so don't repeat the teaser video
      case appStates::boyloop:
      case appStates::girlloop:
      case appStates::boyreveal:
      case appStates::girlreveal:
        repeatTeaser = false;

      //  Keep playing the teaser video
      case appStates::playteaser:
        ackStatus = appStatus;
        break;

      //  No video - wait a bit
      default:
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
    }

    //  Kick off another data request if we are half way 
    //  through the current buffer
    //
    //  This is hows this works, assuming 16 bytes in the 
    //  buffer (buffXfer):
    //
    //  buffOffset  - 0..15
    //  buffXfer    - 16
    //  buffSize    - 16
    //
    //  So:
    //          -> BuffOffset
    //  0123456789ABCDEF
    //
    //  When buffOffset >= 8 (8 not processed):
    //
    //  1. Calculate the bytes to move e.g. buffXfer - buffOffset (toMove).
    //  2. Move the buffer from 8..15 down into 0..7.
    //  3. Set the total number of bytes in the buffer.
    //  4. Calculate the bytes to request from the server (reqBytes).
    //  4. Request the correct number of bytes from the server.
    //  5. Reset the buffOffset to 0.
    //
    //  Walkthrough with buffOffset 10 and buffXfer 16:
    //
    //  buffOffset  = 10 (0..10)
    //  buffXfer    = 16
    //  buffSize    = 16
    //  buffer      = 0123456789ABCDEF
    //
    //
    //  Calculate and move:
    //
    //  toMove      = 16 - 10 = 6
    //  memove
    //  buffer      = ABCDEF
    //
    //  Set bytes in buffer:
    // 
    //  buffXfer    = 6
    //
    //  Calculate and request:  
    //
    //  reqBytes    = 16 - 6 = 10
    //
    //  Reset buffer offset:
    //
    //  buffOffset  = 0
    //

    if (buffOffset >= bufSize / 2)
    {
      DEBUGOUT("Time to move : %d %d\n", buffOffset, buffXfer.load());

      //  Move the current buffer up
      moveLock.lock();
      auto toMove = buffXfer - buffOffset;
      //memmove(buffer, buffer + buffOffset, toMove);
      memcpy(buffer, buffer + buffOffset, toMove);

      //  Set where the new data will be read
      buffXfer = toMove;

      //  Calculate the request
      auto reqBytes = bufSize - toMove;

      //  Clear the upper buffer to prevent ghosting
      memset(buffer + buffXfer, 0, reqBytes);
      moveLock.unlock();

      //  Send the read request
      outBuffer [0] = (char) requests::sendfile;
      memcpy(outBuffer + 1, &reqBytes, sizeof(reqBytes));
      ourClient->write(outBuffer, sizeof(reqBytes) + 1);

      DEBUGOUT("Moved %d, in buffer %d, requested %d\n", toMove, buffXfer.load(), reqBytes);

      //  Process from start of buffer, after a short pause
      buffOffset = 0;
      vTaskDelay(pdMS_TO_TICKS(msPerFrame));
      continue;
    }

    //  Pause before displaying the frame. This pause is a little off
    //  as there is overhead between finding the frame and displaying
    //  it, but it's close enough    
    vTaskDelay(pdMS_TO_TICKS(msPerFrame));

    //  Frames start with a 0xFFD8 and end with 0xFFD9
    //
    //  Take a  current snapshot of the buffer counters as these could
    //  change under us
    //
    auto buffEndOffset = buffXfer.load();

    //  Start of the buffer
    auto buffStartPtr = &buffer [buffOffset];
    auto buffEndPtr = &buffer [buffEndOffset - 1];

    //  Bytes left
    auto bytesLeft = buffEndPtr - buffStartPtr;

    //  Pointers to the start and end of frames
    uint8_t *frameStartPtr = nullptr;
    uint8_t *frameEndPtr = nullptr;
    while (buffStartPtr < buffEndPtr)
    {
      //  Find the 0xFF marker
      uint8_t *tmpPtr = (uint8_t *) memchr(buffStartPtr, 0xFF, bytesLeft);
      if (tmpPtr == nullptr) 
      {
        //  No 0xFF marker found - this can happen if we are waiting
        //  for the rest of the buffer to fill after requesting a
        //  new chunk of the video
        break;
      }

      if (tmpPtr [1] == 0xD8)
      {
        //  Set the start frame and reposition the start of 
        //  the search buffer
        frameStartPtr = tmpPtr;
      }
      else
      if (tmpPtr [1] == 0xD9)
      {
        //  Set the end
        frameEndPtr = tmpPtr + 1;

        if (frameStartPtr != nullptr)
        {
          //  Already found the start frame
          break;
        }
      }

      //  Move the buffer pointer along and try again
      buffStartPtr = tmpPtr + 1;

      //  Adjust bytes left
      bytesLeft = buffEndPtr - buffStartPtr;
    }

    //  Did we find anything?
    if (frameStartPtr != nullptr && frameEndPtr != nullptr)
    {
      auto frameSize = (frameEndPtr - frameStartPtr) + 1;
      if (frameSize <= 4)
      {
        //  Send the close request
        RequestClose();

        //  Last frame received
        if (repeatTeaser)
        {
          ackStatus = appStates::countdown; 
        }
        else
        {
          if (appStatus == appStates::boyreveal
          ||  appStatus == appStates::boyloop)
          {
            ackStatus = appStates::boyloopopen;
            vTaskDelay(pdMS_TO_TICKS(250));
          }
          else
          if (appStatus == appStates::girlreveal
          ||  appStatus == appStates::girlloop)
          {
            ackStatus = appStates::girlloopopen;
            vTaskDelay(pdMS_TO_TICKS(250));
          }
          else
          {
            DEBUGOUT("Odd state %d\n", appStatus);
          }
        }

        appStatus = appStates::waitack;
        continue;
      }

      //  Output this frame
      M5.Display.drawJpg(frameStartPtr, frameSize, 70, 70);

      //  Next frame
      buffOffset += (frameEndPtr - &buffer [buffOffset]);
    }
    else
    {
      //  No frames found, so wait a little
      DEBUGOUT("No frames found!\n");
      vTaskDelay(pdMS_TO_TICKS(msPerFrame));
    }
  }
}


void setup (void) 
{
  //  Setup M5 Stopwatch
  M5.begin();
  Serial.begin(9600);

  buffer = (uint8_t *) ps_malloc(bufSize+16);
  if (buffer == nullptr)
  {
    //  Did not get buffer - cannot continue
    Serial.printf("Could not allocate buffers\n");
    while (true)
      delay(1000);
  }

	//  Attach to private access point on the AtomS3R
	WiFi.mode(WIFI_STA);
	WiFi.begin(APSSID, APPASS);
  DEBUGOUT("Waiting for AP.");
	while (WiFi.status() != WL_CONNECTED)
  {
		DEBUGOUT(".");
		delay(500);
	}
  DEBUGOUT("AP connected\n");

  //  Create a thread to handle video playback  
  xTaskCreate(PlayVideo, "PlayVideo", 2048, nullptr, 1, nullptr);

  //  Prepare the screen for the countdown
  M5.Display.clear(TFT_BLACK);
  M5.Display.setTextSize(20);
  M5.Display.setBrightness(75);

  DEBUGOUT("Ready...\n");

  appStatus = appStates::connectserver;
}

void ConnectServer (void)
{
	ourClient = new AsyncClient;
	ourClient->onData(&NWData, ourClient);
	ourClient->onError(&NWError, ourClient);
  ourClient->onConnect(&NWConnect, ourClient);
	ourClient->onDisconnect(&NWDisconnect, ourClient);
	ourClient->onTimeout(&NWTimeOut, ourClient);
  ourClient->onAck(&NWAck, ourClient);
	
  //if (!ourClient->connect(SVHOST, SVPORT))
  if (!ourClient->connect("192.168.4.1", SVPORT))
  {
    DEBUGOUT("Could not connect\n");
    while(true);
  }

  appStatus = appStates::waitconnection;
}


void OpenVideo (String fname, appStates batchState)
{
  if (RequestOpen(fname, batchState))
  {
    DEBUGOUT("Open video okay\n");
    askData = true;
  }
  else
  {
    DEBUGOUT("Open video fail\n");
  }
}


//  Pre-load as much of the video as possible
void BatchVideo (appStates ackState, appStates nextState)
{
  static int            batchCount  = 0;
  static unsigned long  batchTime   = 0;
  static size_t         lastBuffSz  = 0;
  static unsigned long  stuckTime   = 0;

  int QColors [] = {TFT_PURPLE, TFT_SKYBLUE, TFT_PINK};

  if (askData)
  {
    DEBUGOUT("Asking batch\n");

    askData   = false;
    ackStatus = ackState;

    outBuffer [0] = (char) requests::sendfile;
    memcpy(outBuffer + 1, &bufSize, sizeof(bufSize));
    ourClient->write(outBuffer, sizeof(bufSize) + 1);
    return;
  }

  if (appStatus == appStates::batchteaser
  ||  appStatus == appStates::boybatch
  ||  appStatus == appStates::girlbatch)
  {
    unsigned long now = millis();
    if (now > batchTime)
    {
      M5.Display.setTextColor(QColors [batchCount % 3], TFT_BLACK);
      M5.Display.setCursor(60, 150);
      M5.Display.printf(" ?");
      batchTime = now + 1000;
      batchCount ++;
    }
  }

  //  Enough buffer to play the video
  bool playNow = (buffXfer == bufSize);  

  //  Has the buffer size stalled?
  if (!playNow
  &&  buffXfer > 0
  &&  lastBuffSz == buffXfer)
  {
    // Has the buffer been stuck for 1 second?
    if (millis() - stuckTime > 1000)
    {
      //  Some files are smaller than the buffer size, which means we have to look
      //  for another means of determining we have received enough video to play
      playNow = true;
    }
    else
    {
      //  Try another cycle
      return;
    }
  }

  if (playNow)
  {
    //  As much data as possible is in the buffer. Time to
    //  play the video
    DEBUGOUT("Request play\n");
    switch (nextState)
    {
      case appStates::batchteaser:
      case appStates::boybatch:
      case appStates::girlbatch:
        M5.Display.clear(TFT_BLACK);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        batchTime   = 0;
        batchCount  = 0;
        break;
    }

    //  Clear old counters
    stuckTime = 0;
    lastBuffSz = 0;

    //  Move to next app state
    appStatus = nextState;
    return;
  }

  //  Save the last buffer size
  lastBuffSz = buffXfer.load();
  stuckTime = millis();
}


void loop (void)
{
  static int            CDCount = CDStart;
  static unsigned long  CDTime  = 0;

  M5.update();

  if (M5.BtnA.wasPressed())
  {
    //  Girl reveal
    if (appStatus > appStates::startreveal
    &&  appStatus < appStates::endreveal)  
    {
      ackStatus = appStates::girlsync;
      appStatus = appStates::waitack;
      RequestIdle();
      M5.Display.clear(TFT_BLACK);
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }

  if (M5.BtnB.wasPressed())
  {
    //  Boy reveal
    if (appStatus > appStates::startreveal
    &&  appStatus < appStates::endreveal)  
    {
      ackStatus = appStates::boysync;
      appStatus = appStates::waitack;
      RequestIdle();
      M5.Display.clear(TFT_BLACK);
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }

  switch (appStatus)
  {
    case appStates::connectserver:
      ConnectServer();
      break;

    case appStates::countdown:
      if (millis() > CDTime)
      {
        if (CDTime == 0)
        {
            M5.Display.clear(TFT_BLACK);
            M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

        }

        DEBUGOUT("Countdown %d %d %d\n", CDCount, buffXfer.load(), buffOffset);
   
        M5.Display.setCursor(60, 150);
        M5.Display.printf("%2d", CDCount);

        CDCount --;
        CDTime = millis() + 1000;
        if (CDCount == 0)
        {
          appStatus = appStates::openteaser;
        }
      }
      break;

    case appStates::openteaser:
      if (millis() > CDTime)
      {
        CDCount = CDStart;
        CDTime  = 0;
        ResetCounters();
        OpenVideo("/GUESS.MJP", appStates::batchteaser);
        M5.Display.clear(TFT_BLACK);
      }
      break;

    case appStates::batchteaser:
      BatchVideo(appStates::batchteaser, appStates::playteaser);
      break;

    case appStates::boysync:
      ackStatus = appStates::boyopen;
      appStatus = appStates::waitack;
      RequestClose();
      break;

    case appStates::boyopen:
      ResetCounters();
      OpenVideo("/BOY.MJP", appStates::boybatch);
      M5.Display.clear(TFT_BLACK);
      break;

    case appStates::boybatch:
      BatchVideo(appStates::boybatch, appStates::boyreveal);
      break;

    case appStates::girlsync:
      ackStatus = appStates::girlopen;
      appStatus = appStates::waitack;
      RequestClose();
      break;

    case appStates::girlopen:
      ResetCounters();
      OpenVideo("/GIRL.MJP", appStates::girlbatch);
      M5.Display.clear(TFT_BLACK);
      break;

    case appStates::girlbatch:
      BatchVideo(appStates::girlbatch, appStates::girlreveal);
      break;

    case appStates::boyloopopen:
      ResetCounters();
      OpenVideo("/BLOOP.MJP", appStates::boyloopbatch);
      M5.Display.clear(TFT_SKYBLUE);
      break;

    case appStates::boyloopbatch:
      BatchVideo(appStates::boyloopbatch, appStates::boyloop);
      break;

    case appStates::girlloopopen:
      ResetCounters();
      OpenVideo("/GLOOP.MJP", appStates::girlloopbatch);
      M5.Display.clear(TFT_PINK);
      break;

    case appStates::girlloopbatch:
      BatchVideo(appStates::girlloopbatch, appStates::girlloop);
      break;
  }
}

#pragma GCC pop_options
