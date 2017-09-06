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

#ifndef __PLAYBACK_HH__
#define __PLAYBACK_HH__

#include "DeckLinkAPI.h"
#include "file.hh"
#include <atomic>
#include <fstream>
#include <list>
#include <chrono>
#include <mutex>
#include <random>
#include <queue>
#include <utility>
#include <thread>
#include "h264_degrader.hh"

using std::chrono::time_point;
using std::chrono::high_resolution_clock;
using std::chrono::time_point_cast;
using std::chrono::microseconds;

class Playback : public IDeckLinkVideoOutputCallback {
private:
    int32_t                 m_refCount;
    //BMDConfig*              m_config;
    bool                    m_running;
    IDeckLink*              m_deckLink;
    IDeckLinkOutput*        m_deckLinkOutput;
    IDeckLinkDisplayMode*   m_displayMode;

    unsigned long           m_frameWidth;
    unsigned long           m_frameHeight;
    BMDTimeValue            m_frameDuration;
    BMDTimeScale            m_frameTimescale;
    unsigned long           m_framesPerSecond;
    unsigned long           m_totalFramesScheduled;
    unsigned long           m_totalFramesDropped;
    unsigned long           m_totalFramesCompleted;

    int m_deckLinkIndex;
    int m_displayModeIndex;
    BMDVideoOutputFlags m_outputFlags;
    BMDPixelFormat m_pixelFormat;
    const char* m_videoInputFile;

    std::list<uint8_t*>             &output;
    std::mutex                      &output_mutex;

    std::queue<std::pair<uint8_t*, uint8_t*> > record;
    std::mutex record_mutex;
    std::thread t;

    std::ofstream           m_logfile;
  //File                    m_infile;

    int beforeFile;
    int afterFile;
    
    std::list<time_point<high_resolution_clock>> scheduled_timestamp_cpu;
    std::list<BMDTimeValue> scheduled_timestamp_decklink;
    

    // Signal Generator Implementation
    void            StartRunning();
    void            StopRunning();
    void            ScheduleNextFrame(bool prerolling);

    const char*     GetPixelFormatName(BMDPixelFormat pixelFormat);
    void            PrintStatusLine(uint32_t queued);

    void WriteToDisk();

public:
    int frame_rate; 
    int framesDelay; 
    H264_degrader* degrader;
    bool end;

    ~Playback();
    Playback(int m_deckLinkIndex,
	     int m_displayModeIndex,
	     BMDVideoOutputFlags m_outputFlags,
	     BMDPixelFormat m_pixelFormat,
	     const char* m_videoInputFile,
	     std::list<uint8_t*> &output,
	     std::mutex &output_mutex,
	     int frames_rate,
	     int framesDelay,
         int bitrate,
         int quantization,
	     char* beforeFilename,
	     char* afterFilename);

    bool Run();

    // *** DeckLink API implementation of IDeckLinkVideoOutputCallback IDeckLinkAudioOutputCallback *** //
    // IUnknown
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv);
    virtual ULONG STDMETHODCALLTYPE AddRef();
    virtual ULONG STDMETHODCALLTYPE Release();

    virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result);
    virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped();

    HRESULT CreateFrame(IDeckLinkVideoFrame** theFrame, void (*fillFunc)(IDeckLinkVideoFrame*));

    Playback( const Playback & other ) = delete;
    Playback & operator=( const Playback & other ) = delete;
};

int GetBytesPerPixel(BMDPixelFormat pixelFormat);

#endif
