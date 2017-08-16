/* -LICENSE-START-
** Copyright (c) 2013 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <atomic>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <libgen.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <iostream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <cassert>
#include <list>
#include <mutex>
#include <queue>
#include <utility>
#include <random>
#include <memory>
#include "Playback.hh"
#include "exception.hh"
#include "chunk.hh"
#include "child_process.hh"
#include "system_runner.hh"

#include "h264_degrader.hh"

using std::chrono::time_point;
using std::chrono::high_resolution_clock;
using std::chrono::time_point_cast;
using std::chrono::microseconds;

const BMDTimeScale ticks_per_second = (BMDTimeScale)1000000; /* microsecond resolution */

void* frameBytes = NULL;

pthread_mutex_t         sleepMutex;
pthread_cond_t          sleepCond;
bool                    do_exit = false;

uint64_t memory_frontier = 0;
const uint64_t prefetch_buffer_size = 1 << 30; // 1 GB
const uint64_t prefetch_block_size = 1 << 28; // 0.0125 GB

const unsigned long     kAudioWaterlevel = 48000;
// std::ofstream debugf;

BMDTimeValue prev_decklink_frame_completed_timestamp;
BMDTimeValue prev_decklink_hardware_timestamp;

const size_t width = 1280;
const size_t height = 720;
const size_t bytes_per_pixel = 4;
const size_t frame_size = width*height*bytes_per_pixel;
const AVPixelFormat pix_fmt = AV_PIX_FMT_YUV422P;

const size_t bitrate = (2<<20);
const size_t quantization = 32;

static uint8_t *previousFrame = new uint8_t[frame_size];
std::thread runner;

void sigfunc(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        do_exit = true;
    }
    pthread_cond_signal(&sleepCond);
}

Playback::~Playback()
{
  delete degrader;
  delete previousFrame;
  close(beforeFile);
  close(afterFile);
  t.join();
  runner.join();
}

Playback::Playback(int m_deckLinkIndex,
		   int m_displayModeIndex,
		   BMDVideoOutputFlags m_outputFlags,
		   BMDPixelFormat m_pixelFormat,
		   const char* m_videoInputFile,
		   std::list<uint8_t*> &output,
		   std::mutex &output_mutex,
		   int framesDelay,
		   char* beforeFilename,
		   char* afterFilename) :
  
    m_refCount(1),
    m_running(false),
    m_deckLink(),
    m_deckLinkOutput(),
    m_displayMode(),
    m_frameWidth(0),
    m_frameHeight(0),
    m_frameDuration(0),
    m_frameTimescale(0),
    m_framesPerSecond(0),
    m_totalFramesScheduled(0),
    m_totalFramesDropped(0),
    m_totalFramesCompleted(0),
    m_deckLinkIndex(m_deckLinkIndex),
    m_displayModeIndex(m_displayModeIndex),
    m_outputFlags(m_outputFlags),
    m_pixelFormat(m_pixelFormat),
    m_videoInputFile(m_videoInputFile),
    output(output),
    output_mutex(output_mutex),
    record(),
    t(&Playback::WriteToDisk, this),
    m_logfile(),
    scheduled_timestamp_cpu(),
    scheduled_timestamp_decklink(),
    framesDelay(framesDelay)
{
    degrader = new H264_degrader(width, height, bitrate, quantization);

    beforeFile = open(beforeFilename, O_WRONLY|O_CREAT|O_TRUNC, 0664);
    if (beforeFile < 0) {
      std::cout << "Could not open file: " << beforeFilename << "\n";
      exit(1);
    }

    afterFile = open(afterFilename, O_WRONLY|O_CREAT|O_TRUNC, 0664);
    if (afterFile < 0) {
      std::cout << "Could not open file: " << afterFilename << "\n";
      exit(1);
    }
}

void Playback::WriteToDisk()
{
  while(true) {
    int rec_size;
    uint8_t *beforeFrame, *afterFrame;
    {
      std::lock_guard<std::mutex> rec_guard(record_mutex);
      rec_size = record.size();
    }
    if (rec_size > 0) {
        {
            std::lock_guard<std::mutex> rec_guard(record_mutex);
            beforeFrame = record.front().first;
            afterFrame = record.front().second;
            record.pop();
        }
        size_t ret = write(beforeFile, beforeFrame, frame_size);
        if (ret < 0) {
            std::cout << "Cannot write to first file\n";
        }
        ret = write(afterFile, afterFrame, frame_size);
      if (ret < 0) {
          std::cout << "Cannot write to second file\n";
      }
      delete[] beforeFrame;
      delete[] afterFrame;
    }
    else {
      usleep(10000);
    }
  }
}

bool Playback::Run()
{
    HRESULT                         result;
    int                             idx;
    bool                            success = false;

    IDeckLinkIterator*              deckLinkIterator = NULL;
    IDeckLinkConfiguration*         deckLinkConfiguration = NULL;
    IDeckLinkDisplayModeIterator*   displayModeIterator = NULL;
    char*                           displayModeName = NULL;
    //uint8_t* frame = nullptr; 

    // Get the DeckLink device
    deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (!deckLinkIterator)
    {
        fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
        goto bail;
    }

    idx = m_deckLinkIndex;

    while ((result = deckLinkIterator->Next(&m_deckLink)) == S_OK)
    {
        if (idx == 0)
            break;
        --idx;

        m_deckLink->Release();
    }

    if (result != S_OK || m_deckLink == NULL)
    {
        fprintf(stderr, "Unable to get DeckLink device %u\n", m_deckLinkIndex);
        goto bail;
    }

    if (m_deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&deckLinkConfiguration) != S_OK)
        goto bail;

    deckLinkConfiguration->SetInt(bmdDeckLinkConfigVideoOutputIdleOperation, (int64_t)bmdIdleVideoOutputBlack);
    deckLinkConfiguration->SetInt(bmdDeckLinkConfigCapturePassThroughMode, (int64_t)bmdDeckLinkCapturePassthroughModeDisabled);

    //Requires root:
    deckLinkConfiguration->WriteConfigurationToPreferences();

    // Get the output (display) interface of the DeckLink device
    if (m_deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&m_deckLinkOutput) != S_OK)
        goto bail;

    // Get the display mode
    idx = m_displayModeIndex;

    result = m_deckLinkOutput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK)
        goto bail;

    while((result = displayModeIterator->Next(&m_displayMode)) == S_OK)
    {
        if (idx == 0)
            break;
        --idx;

        m_displayMode->Release();
    }

    if (result != S_OK || m_displayMode == NULL)
    {
        fprintf(stderr, "Unable to get display mode %d\n", m_displayModeIndex);
        goto bail;
    }

    // Get display mode name
    result = m_displayMode->GetName((const char**)&displayModeName);
    if (result != S_OK)
    {
        displayModeName = (char *)malloc(32);
        snprintf(displayModeName, 32, "[index %d]", m_displayModeIndex);
    }

    if (m_videoInputFile == NULL) {
        fprintf(stderr, "-v <video filename> flag required\n");
        exit(1);
    }

    // Provide this class as a delegate to the audio and video output interfaces
    m_deckLinkOutput->SetScheduledFrameCompletionCallback(this);

    success = true;

    // Start
    StartRunning();

    while ( !do_exit ) { 
        usleep(1000);
    }

    m_running = false;

bail:
    if (displayModeName != NULL)
        free(displayModeName);

    if (m_displayMode != NULL)
        m_displayMode->Release();

    if (displayModeIterator != NULL)
        displayModeIterator->Release();

    if (m_deckLinkOutput != NULL)
        m_deckLinkOutput->Release();

    if (deckLinkConfiguration != NULL)
        deckLinkConfiguration->Release();

    if (m_deckLink != NULL)
        m_deckLink->Release();

    if (deckLinkIterator != NULL)
        deckLinkIterator->Release();

    usleep(1<23); // 2^23 microsecondso or ~8 seconds

    return success;
}

const char* Playback::GetPixelFormatName(BMDPixelFormat pixelFormat)
{
    switch (pixelFormat)
    {
        case bmdFormat8BitYUV:
            return "8 bit YUV (4:2:2)";
        case bmdFormat10BitYUV:
            return "10 bit YUV (4:2:2)";
        case bmdFormat10BitRGB:
            return "10 bit RGB (4:4:4)";
    }
    return "unknown";
}

void Playback::StartRunning()
{
    HRESULT                 result;

    m_frameWidth = m_displayMode->GetWidth();
    m_frameHeight = m_displayMode->GetHeight();
    m_displayMode->GetFrameRate(&m_frameDuration, &m_frameTimescale);

    // Calculate the number of frames per second, rounded up to the nearest integer.  For example, for NTSC (29.97 FPS), framesPerSecond == 30.
    m_framesPerSecond = (unsigned long)((m_frameTimescale + (m_frameDuration-1))  /  m_frameDuration);
    assert(m_framesPerSecond == 60);
    m_framesPerSecond = 60;
    std::cout << "m_framesPerSecond: "  << m_framesPerSecond << std::endl;

    // Set the video output mode
    result = m_deckLinkOutput->EnableVideoOutput(m_displayMode->GetDisplayMode(), m_outputFlags);
    if (result != S_OK)
    {
        fprintf(stderr, "Failed to enable video output. Is another application using the card?\n");
        goto bail;
    }

    // Begin video preroll by scheduling a second of frames in hardware
    m_totalFramesScheduled = 0;
    m_totalFramesDropped = 0;
    m_totalFramesCompleted = 0;
    //for (unsigned i = 0; i < m_framesPerSecond; i++)
    //for (unsigned i = 0; i < 9; i++)
    //for (unsigned i = 0; i < (unsigned) framesDelay; i++)
    
    runner = std::move(std::thread(
                                   [this](){ 
                                       while(true){
                                           ScheduleNextFrame(false);
                                           usleep(1000);
                                       }
                                   }
                                   ));
    
    m_deckLinkOutput->StartScheduledPlayback(0, m_frameTimescale, 1.0);

    m_running = true;

    return;

bail:
    // *** Error-handling code.  Cleanup any resources that were allocated. *** //
    StopRunning();
}

void Playback::StopRunning()
{
    // Stop the audio and video output streams immediately
    m_deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
    // debugf.close();
    m_deckLinkOutput->DisableVideoOutput();

    // Success; update the UI
    m_running = false;
}

void Playback::ScheduleNextFrame(bool prerolling)
{
    if (prerolling == false)
    {
        // If not prerolling, make sure that playback is still active
        if (m_running == false)
            return;
    }

    uint8_t* pulledFrame = NULL;
    uint8_t* degradedFrame = NULL;
    std::lock_guard<std::mutex> guard(output_mutex);	
    if (output.size() >= (unsigned) framesDelay) {
      pulledFrame = output.front();
      degradedFrame = new uint8_t[frame_size];
      output.pop_front();
    }
    else {
        return;
    }    
 
    IDeckLinkMutableVideoFrame* newFrame = NULL;
    int bytesPerPixel = GetBytesPerPixel(m_pixelFormat);
    HRESULT result = m_deckLinkOutput->CreateVideoFrame(m_frameWidth, m_frameHeight,
                                                        m_frameWidth * bytesPerPixel,
                                                        m_pixelFormat, bmdFrameFlagDefault, &newFrame);
    
    if (result != S_OK) {
        fprintf(stderr, "Failed to create video frame\n");
        return;
    }
    newFrame->GetBytes(&frameBytes);

    if (pulledFrame && degradedFrame) {
      std::lock_guard<std::mutex> lg(degrader->degrader_mutex);

      auto convert_tot1 = std::chrono::high_resolution_clock::now();
      degrader->bgra2yuv422p((uint8_t*)pulledFrame, degrader->encoder_frame, width, height);
      auto convert_tot2 = std::chrono::high_resolution_clock::now();
      auto convert_totime = std::chrono::duration_cast<std::chrono::duration<double>>(convert_tot2 - convert_tot1);
      //std::cout << "convert_totime " << convert_totime.count() << "\n";
      
      auto degrade_t1 = std::chrono::high_resolution_clock::now();
      degrader->degrade(degrader->encoder_frame, degrader->decoder_frame);
      auto degrade_t2 = std::chrono::high_resolution_clock::now();
      auto degrade_time = std::chrono::duration_cast<std::chrono::duration<double>>(degrade_t2 - degrade_t1);
      //std::cout << "degrade_time " << degrade_time.count() << "\n";
      

      auto convert_fromt1 = std::chrono::high_resolution_clock::now();
      degrader->yuv422p2bgra(degrader->decoder_frame, (uint8_t*)degradedFrame, width, height);
      auto convert_fromt2 = std::chrono::high_resolution_clock::now();
      auto convert_fromtime = std::chrono::duration_cast<std::chrono::duration<double>>(convert_fromt2 - convert_fromt1);
      //std::cout << "convert_fromtime " << convert_fromtime.count() << "\n";
      
      auto memcpyt1 = std::chrono::high_resolution_clock::now();
      std::memcpy(frameBytes, degradedFrame, frame_size);
      std::memcpy(previousFrame, degradedFrame, frame_size);
      auto memcpyt2 = std::chrono::high_resolution_clock::now();
      auto memcpytime = std::chrono::duration_cast<std::chrono::duration<double>>(memcpyt2 - memcpyt1);
      //std::cout << "memcpytime " << memcpytime.count() << "\n";
      //std::cout << "-----frame-----\n";
      
      {
          std::lock_guard<std::mutex> rec_guard(record_mutex);		  
          record.push(std::pair<uint8_t*, uint8_t*> (pulledFrame, degradedFrame));
      }
    }
    else {
      std::memcpy(frameBytes, previousFrame, frame_size);
    }      
      
    const unsigned int frame_time = m_totalFramesScheduled * m_frameDuration;
    if (m_deckLinkOutput->ScheduleVideoFrame(newFrame, frame_time, m_frameDuration, m_frameTimescale) != S_OK){
      return;
    }

    m_totalFramesScheduled += 1;
}

HRESULT Playback::CreateFrame(IDeckLinkVideoFrame** frame, void (*fillFunc)(IDeckLinkVideoFrame*))
{
    HRESULT                     result;
    int                         bytesPerPixel = GetBytesPerPixel(m_pixelFormat);
    IDeckLinkMutableVideoFrame* newFrame = NULL;
    IDeckLinkMutableVideoFrame* referenceFrame = NULL;
    IDeckLinkVideoConversion*   frameConverter = NULL;

    *frame = NULL;

    result = m_deckLinkOutput->CreateVideoFrame(m_frameWidth, m_frameHeight, m_frameWidth * bytesPerPixel, m_pixelFormat, bmdFrameFlagDefault, &newFrame);
    if (result != S_OK)
    {
        fprintf(stderr, "Failed to create video frame\n");
        goto bail;
    }

    if (m_pixelFormat == bmdFormat8BitBGRA)
    {
        fillFunc(newFrame);
    }
    else
    {
        // Create a black frame in 8 bit YUV and convert to desired format
        result = m_deckLinkOutput->CreateVideoFrame(m_frameWidth, m_frameHeight, m_frameWidth * GetBytesPerPixel(bmdFormat8BitBGRA), bmdFormat8BitBGRA, bmdFrameFlagDefault, &referenceFrame);
        if (result != S_OK)
        {
            fprintf(stderr, "Failed to create reference video frame\n");
            goto bail;
        }

        fillFunc(referenceFrame);

        frameConverter = CreateVideoConversionInstance();

        result = frameConverter->ConvertFrame(referenceFrame, newFrame);
        if (result != S_OK)
        {
            fprintf(stderr, "Failed to convert frame\n");
            goto bail;
        }
    }

    *frame = newFrame;
    newFrame = NULL;

bail:
    if (referenceFrame != NULL)
        referenceFrame->Release();

    if (frameConverter != NULL)
        frameConverter->Release();

    if (newFrame != NULL)
        newFrame->Release();

    return result;
}

void Playback::PrintStatusLine(uint32_t queued)
{
    printf("scheduled %-16lu completed %-16lu dropped %-16lu frame level %-16u\n",
        m_totalFramesScheduled, m_totalFramesCompleted, m_totalFramesDropped, queued);
}

/************************* DeckLink API Delegate Methods *****************************/


HRESULT Playback::QueryInterface(REFIID, LPVOID *ppv)
{
    *ppv = NULL;
    return E_NOINTERFACE;
}

ULONG Playback::AddRef()
{
    // gcc atomic operation builtin
    return __sync_add_and_fetch(&m_refCount, 1);
}

ULONG Playback::Release()
{
    // gcc atomic operation builtin
    ULONG newRefValue = __sync_sub_and_fetch(&m_refCount, 1);
    if (!newRefValue)
        delete this;
    return newRefValue;
}

HRESULT Playback::ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result)
{
    /* IMPORTANT: get the time stamps for when a frame is completed */
    //time_point<high_resolution_clock> tp = high_resolution_clock::now();

    BMDTimeValue decklink_hardware_timestamp;
    BMDTimeValue decklink_time_in_frame;
    BMDTimeValue decklink_ticks_per_frame;
    HRESULT ret;

    if (do_exit) {
        ++m_totalFramesCompleted;
        completedFrame->Release();
        return S_OK;
    }

    if ( (ret = m_deckLinkOutput->GetHardwareReferenceClock(ticks_per_second,
                                                            &decklink_hardware_timestamp,
                                                            &decklink_time_in_frame,
                                                            &decklink_ticks_per_frame) ) != S_OK) {
        std::cerr << "ScheduledFrameCompleted: could not get GetHardwareReferenceClock timestamp" << std::endl;
        return ret;
    }

    BMDTimeValue decklink_frame_completed_timestamp;
    if( (ret = m_deckLinkOutput->GetFrameCompletionReferenceTimestamp(completedFrame,
                                                                      ticks_per_second,
                                                                      &decklink_frame_completed_timestamp) ) != S_OK ) {

        std::cerr << "ScheduledFrameCompleted: could not get FrameCompletionReference timestamp" << std::endl;
        return ret;
    }

    void *frameBytes = NULL;
    completedFrame->GetBytes(&frameBytes);

    if (decklink_frame_completed_timestamp - prev_decklink_frame_completed_timestamp > 51000) {
      std::cout << "Warning: Frame " << m_totalFramesCompleted << " Displayed Late. " << std::endl;
      std::cout << "Timestamp delay: " << decklink_frame_completed_timestamp - prev_decklink_frame_completed_timestamp << std::endl;
    }
    else if (decklink_hardware_timestamp - prev_decklink_hardware_timestamp > 51000) {
      std::cout << "Warning: Frame " << m_totalFramesCompleted << " Displayed Late. " << std::endl;
      std::cout << "Hardware timestamp delay: " << decklink_hardware_timestamp - prev_decklink_hardware_timestamp << std::endl;
    }

    completedFrame->Release();
    ++m_totalFramesCompleted;

    prev_decklink_frame_completed_timestamp = decklink_frame_completed_timestamp;
    prev_decklink_hardware_timestamp = decklink_hardware_timestamp;

    //ScheduleNextFrame(false);

    return S_OK;
}

HRESULT Playback::ScheduledPlaybackHasStopped()
{
    return S_OK;
}

/*****************************************/

int GetBytesPerPixel(BMDPixelFormat pixelFormat)
{
    int bytesPerPixel = 2;

    switch(pixelFormat)
    {
    case bmdFormat8BitYUV:
        bytesPerPixel = 2;
        break;
    case bmdFormat8BitARGB:
    case bmdFormat10BitYUV:
    case bmdFormat10BitRGB:
    case bmdFormat8BitBGRA:
        bytesPerPixel = 4;
        break;
    }

    return bytesPerPixel;
}
