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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include "Config.hh"

BMDConfig::BMDConfig() :
    m_deckLinkIndex(0),
    m_displayModeIndex(-2),
    m_maxFrames(-1),
    m_inputFlags(bmdVideoInputFlagDefault),
    m_pixelFormat(bmdFormat8BitBGRA),
    m_framesDelay(0),
    m_bitrate(1 << 20),
    m_framerate(2),
    m_quantization(32),
    m_videoOutputFile(),
    m_logFilename(),
    m_deckLinkName(),
    m_displayModeName()
{
}

BMDConfig::~BMDConfig()
{
    if (m_deckLinkName)
        free(m_deckLinkName);

    if (m_displayModeName)
        free(m_displayModeName);
}

bool BMDConfig::ParseArguments(int argc,  char** argv)
{
    int     ch;
    bool    displayHelp = false;

    while ((ch = getopt(argc, argv, "d:hm:p:l:D:b:f:q:B:A:")) != -1)
    {
        switch (ch)
        {
            case 'd':
                m_deckLinkIndex = atoi(optarg);
                break;

            case 'm':
                m_displayModeIndex = atoi(optarg);
                break;

            case 'p':
                switch(atoi(optarg))
                {
                    case 0: m_pixelFormat = bmdFormat8BitYUV; break;
                    case 1: m_pixelFormat = bmdFormat10BitYUV; break;
                    case 2: m_pixelFormat = bmdFormat10BitRGB; break;
                    case 3: m_pixelFormat = bmdFormat8BitBGRA; break;

                    default:
                        fprintf(stderr, "Invalid argument: Pixel format %d is not valid", atoi(optarg));
                        return false;
                }
                break;

            case 'l':
                m_logFilename = optarg;
                break;

            case 'h':
                displayHelp = true;
		break;
	    case 'D':
	      m_framesDelay = atoi(optarg);
	      break;
	    case 'b':
	      m_bitrate = atoi(optarg);
	      break;
  	    case 'f':
	      m_framerate = atoi(optarg);
	      break;
  	    case 'q':
	      m_quantization = atoi(optarg);
	      break;
  	    case 'B':
	      m_beforeFilename = optarg;
	      break;
	    case 'A':
	      m_afterFilename = optarg;
	      break;
        }
    }

    if (m_deckLinkIndex < 0)
    {
        fprintf(stderr, "You must select a device\n");
        DisplayUsage(1);
    }

    if (m_displayModeIndex < -1)
    {
        fprintf(stderr, "You must select a display mode\n");
        DisplayUsage(1);
    }

    if (displayHelp)
        DisplayUsage(0);

    // Get device and display mode names
    IDeckLink* deckLink = GetDeckLink(m_deckLinkIndex);
    if (deckLink != NULL)
    {
        if (m_displayModeIndex != -1)
        {
            IDeckLinkDisplayMode* displayMode = GetDeckLinkDisplayMode(deckLink, m_displayModeIndex);
            if (displayMode != NULL)
            {
                displayMode->GetName((const char**)&m_displayModeName);
                displayMode->Release();
            }
            else
            {
                m_displayModeName = strdup("Invalid");
            }
        }
        else
        {
            m_displayModeName = strdup("Format Detection");
        }

        deckLink->GetModelName((const char**)&m_deckLinkName);
        deckLink->Release();
    }
    else
    {
        m_deckLinkName = strdup("Invalid");
    }

    return true;
}

IDeckLink* BMDConfig::GetDeckLink(int idx)
{
    HRESULT             result;
    IDeckLink*          deckLink;
    IDeckLinkIterator*  deckLinkIterator = CreateDeckLinkIteratorInstance();
    int                 i = idx;

    while((result = deckLinkIterator->Next(&deckLink)) == S_OK)
    {
        if (i == 0)
            break;
        --i;

        deckLink->Release();
    }

    deckLinkIterator->Release();

    if (result != S_OK)
        return NULL;

    return deckLink;
}

IDeckLinkDisplayMode* BMDConfig::GetDeckLinkDisplayMode(IDeckLink* deckLink, int idx)
{
    HRESULT                         result;
    IDeckLinkDisplayMode*           displayMode = NULL;
    IDeckLinkInput*                 deckLinkInput = NULL;
    IDeckLinkDisplayModeIterator*   displayModeIterator = NULL;
    int                             i = idx;

    result = deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&deckLinkInput);
    if (result != S_OK)
        goto bail;

    result = deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK)
        goto bail;

    while ((result = displayModeIterator->Next(&displayMode)) == S_OK)
    {
        if (i == 0)
            break;
        --i;

        displayMode->Release();
    }

    if (result != S_OK)
        goto bail;

bail:
    if (displayModeIterator)
        displayModeIterator->Release();

    if (deckLinkInput)
        deckLinkInput->Release();

    return displayMode;
}

void BMDConfig::DisplayUsage(int status)
{
    HRESULT                         result = E_FAIL;
    IDeckLinkIterator*              deckLinkIterator = CreateDeckLinkIteratorInstance();
    IDeckLinkDisplayModeIterator*   displayModeIterator = NULL;

    IDeckLink*                      deckLink = NULL;
    IDeckLink*                      deckLinkSelected = NULL;
    int                             deckLinkCount = 0;
    char*                           deckLinkName = NULL;

    IDeckLinkAttributes*            deckLinkAttributes = NULL;
    bool                            formatDetectionSupported;

    IDeckLinkInput*                 deckLinkInput = NULL;
    IDeckLinkDisplayMode*           displayModeUsage;
    int                             displayModeCount = 0;
    char*                           displayModeName;

    fprintf(stderr,
        "Usage: Capture -d <device id> -m <mode id> [OPTIONS]\n"
        "\n"
        "    -d <device id>:\n"
    );

    // Loop through all available devices
    while (deckLinkIterator->Next(&deckLink) == S_OK)
    {
        result = deckLink->GetModelName((const char**)&deckLinkName);
        if (result == S_OK)
        {
            fprintf(stderr,
                "        %2d: %s%s\n",
                deckLinkCount,
                deckLinkName,
                deckLinkCount == m_deckLinkIndex ? " (selected)" : ""
            );

            free(deckLinkName);
        }

        if (deckLinkCount == m_deckLinkIndex)
            deckLinkSelected = deckLink;
        else
            deckLink->Release();

        ++deckLinkCount;
    }

    if (deckLinkCount == 0)
        fprintf(stderr, "        No DeckLink devices found. Is the driver loaded?\n");

    deckLinkName = NULL;

    if (deckLinkSelected != NULL)
        deckLinkSelected->GetModelName((const char**)&deckLinkName);

    fprintf(stderr,
        "    -m <mode id>: (%s)\n",
        deckLinkName ? deckLinkName : ""
    );

    if (deckLinkName != NULL)
        free(deckLinkName);

    // Loop through all available display modes on the delected DeckLink device
    if (deckLinkSelected == NULL)
    {
        fprintf(stderr, "        No DeckLink device selected\n");
        goto bail;
    }

    result = deckLinkSelected->QueryInterface(IID_IDeckLinkAttributes, (void**)&deckLinkAttributes);
    if (result == S_OK)
    {
        result = deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &formatDetectionSupported);
        if (result == S_OK && formatDetectionSupported)
            fprintf(stderr, "        -1:  auto detect format\n");
    }

    result = deckLinkSelected->QueryInterface(IID_IDeckLinkInput, (void**)&deckLinkInput);
    if (result != S_OK)
        goto bail;

    result = deckLinkInput->GetDisplayModeIterator(&displayModeIterator);
    if (result != S_OK)
        goto bail;

    while (displayModeIterator->Next(&displayModeUsage) == S_OK)
    {
        result = displayModeUsage->GetName((const char **)&displayModeName);
        if (result == S_OK)
        {
            BMDTimeValue frameRateDuration;
            BMDTimeValue frameRateScale;

            displayModeUsage->GetFrameRate(&frameRateDuration, &frameRateScale);

            fprintf(stderr,
                "        %2d:  %-20s \t %li x %li \t %g FPS\n",
                displayModeCount,
                displayModeName,
                displayModeUsage->GetWidth(),
                displayModeUsage->GetHeight(),
                (double)frameRateScale / (double)frameRateDuration
            );

            free(displayModeName);
        }

        displayModeUsage->Release();
        ++displayModeCount;
    }

bail:
    fprintf(stderr,
        "    -p <pixelformat>\n"
        "         0:  8 bit YUV (4:2:2) (default)\n"
        "         1:  10 bit YUV (4:2:2)\n"
        "         2:  10 bit RGB (4:4:4)\n"
        "         3:  8 bit BGRA (4:4:4:x)\n"
        "         4:  8 bit ARGB (4:4:4:4)\n"
        "    -v <filename>        Filename raw video will be written to\n"
        "    -n <frames>          Number of frames to capture (default is unlimited)\n"
        "\n"
        "Capture video to a file. Raw video can be viewed with mplayer eg:\n"
        "\n"
        "    Capture -d 0 -m 2 -n 50 -v video.raw\n"
        "    mplayer video.raw -demuxer rawvideo -rawvideo h=1280:w=720:format=bgra:fps=60\n"
    );

    if (deckLinkIterator != NULL)
        deckLinkIterator->Release();

    if (displayModeIterator != NULL)
        displayModeIterator->Release();

    if (deckLinkInput != NULL)
        deckLinkInput->Release();

    if (deckLinkAttributes != NULL)
        deckLinkAttributes->Release();

    if (deckLinkSelected != NULL)
        deckLinkSelected->Release();

    exit(status);
}

void BMDConfig::DisplayConfiguration()
{
    fprintf(stderr, "Capturing with the following configuration:\n"
        " - Capture device: %s\n"
        " - Video mode: %s\n"
        " - Pixel format: %s\n",
        m_deckLinkName,
        m_displayModeName,
        GetPixelFormatName(m_pixelFormat));
}

const char* BMDConfig::GetPixelFormatName(BMDPixelFormat pixelFormat)
{
    switch (pixelFormat)
    {
        case bmdFormat8BitYUV:
            return "8 bit YUV (4:2:2)";
        case bmdFormat10BitYUV:
            return "10 bit YUV (4:2:2)";
        case bmdFormat10BitRGB:
            return "10 bit RGB (4:4:4)";
        case bmdFormat8BitBGRA:
            return "8 bit BGRA (4:4:4:x)";
        case bmdFormat8BitARGB:
            return "8 bit ARGB (4:4:4:4)";
    }
    return "unknown";
}
