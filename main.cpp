#include <iostream>
#include <fstream>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <Windows.h>
#include <winerror.h>
#include <vector>
#include <thread>

using namespace std;

bool RECORDING = false;
HHOOK hhook;
bool holds_alt = false;
bool holds_ctrl = false;
HDC screen;

vector<thread> recorders;
ofstream temp;

#define REFTIMES_PER_SEC  10000000
#define REFTIMES_PER_MILLISEC  10000

#define EXIT_ON_ERROR(hres)  \
              if (FAILED(hres)) { goto Exit; }
#define SAFE_RELEASE(punk)  \
              if ((punk) != NULL)  \
                { (punk)->Release(); (punk) = NULL; }

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

struct MyAudioSink {
    vector<UINT8> pData;

    HRESULT WriteWavData(WAVEFORMATEX* pwfx) {
        ofstream out("output.wav", ios::binary);

        UINT32 file_size = pData.size() + 36;
        UINT32 bloc_size = 16;
        UINT16 byte_per_bloc = pwfx->nChannels * pwfx->wBitsPerSample / 8;

        // Write WAV header

        temp.close();   
        ifstream in("temp", ios::binary);

        while (in.get() != EOF)
            file_size++;

        in.close();

        out.write("RIFF", 4);
        out.write((const char*)&file_size, 4);
        out.write("WAVE", 4);

        out.write("fmt ", 4);
        out.write((const char*)&bloc_size, 4);
        out.write((const char*)&pwfx->wFormatTag, 2);
        out.write((const char*)&pwfx->nChannels, 2);
        out.write((const char*)&pwfx->nSamplesPerSec, 4);
        out.write((const char*)&pwfx->nAvgBytesPerSec, 4);
        out.write((const char*)&byte_per_bloc, 2);
        out.write((const char*)&pwfx->wBitsPerSample, 2);

        // Write data
        out.write("data", 4);

        int data_size = file_size - 36;
        out.write((const char*)&data_size, 4);

        in = ifstream("temp", ios::binary);

        // process raw audio in blocks, instead of byte by byte
        char block[256];

        while (in.read(block, sizeof(block)))
            out.write((const char*)block, in.gcount());

        in.close();
        
        out.write((const char*)pData.data(), pData.size());

        out.close();
        pData = vector<UINT8>(); // release the built up "music"
        return 0;
    }
};

void DumpAudio(MyAudioSink* pMySink) {
    temp.write((const char*)pMySink->pData.data(), pMySink->pData.size());
    pMySink->pData.clear();
}

HRESULT RecordAudioStream(MyAudioSink* pMySink)
{
    HRESULT hr;
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;
    REFERENCE_TIME hnsActualDuration;
    UINT32 bufferFrameCount;
    UINT32 numFramesAvailable;
    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;
    IAudioClient* pAudioClient = NULL;
    IAudioCaptureClient* pCaptureClient = NULL;
    WAVEFORMATEX* pwfx = NULL;
    UINT32 packetLength = 0;
    BOOL bDone = FALSE;
    BYTE* pData;
    DWORD flags;
    WAVEFORMATEXTENSIBLE* mixFormat;

    int timer = 0;

    hr = CoInitialize(NULL);
    EXIT_ON_ERROR(hr)

    hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator, NULL,
        CLSCTX_ALL, IID_IMMDeviceEnumerator,
        (void**)&pEnumerator);
    EXIT_ON_ERROR(hr)

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    EXIT_ON_ERROR(hr)

    hr = pDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pAudioClient);
    EXIT_ON_ERROR(hr)

    hr = pAudioClient->GetMixFormat(&pwfx);
    EXIT_ON_ERROR(hr)

    hr = pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            hnsRequestedDuration,
            0,
            pwfx,
            NULL);
    EXIT_ON_ERROR(hr)

    mixFormat = (WAVEFORMATEXTENSIBLE*)pwfx;

    if (mixFormat->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
        pwfx->wFormatTag = 0x0003;
    }

    // Get the size of the allocated buffer.
    hr = pAudioClient->GetBufferSize(&bufferFrameCount);
    EXIT_ON_ERROR(hr)

    hr = pAudioClient->GetService(IID_IAudioCaptureClient, (void**)&pCaptureClient);
    EXIT_ON_ERROR(hr)

    // Calculate the actual duration of the allocated buffer.
    hnsActualDuration = (double)REFTIMES_PER_SEC * bufferFrameCount / pwfx->nSamplesPerSec;

    hr = pAudioClient->Start();  // Start recording.
    EXIT_ON_ERROR(hr)

    while (RECORDING) {
        // Sleep for 1/4 of the buffer duration.
        Sleep(hnsActualDuration / REFTIMES_PER_MILLISEC / 4);

        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        EXIT_ON_ERROR(hr)

        while (packetLength != 0) {
            // Get the available data in the shared buffer.
            hr = pCaptureClient->GetBuffer(
                &pData,
                &numFramesAvailable,
                &flags, NULL, NULL);
            EXIT_ON_ERROR(hr)

            // Copy the available capture data to the audio sink.
            for (UINT32 i = 0; i < numFramesAvailable * pwfx->nBlockAlign; i++)
                pMySink->pData.push_back(pData[i]);

            // Avoid using too much memory
            if (pMySink->pData.size() >= 1024 * 1024)
                DumpAudio(pMySink);

            hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            EXIT_ON_ERROR(hr)

            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            EXIT_ON_ERROR(hr)
        }
    }

    hr = pAudioClient->Stop();  // Stop recording.
    EXIT_ON_ERROR(hr)

    pMySink->WriteWavData(pwfx);

Exit:
    CoTaskMemFree(pwfx);
    SAFE_RELEASE(pEnumerator)
    SAFE_RELEASE(pDevice)
    SAFE_RELEASE(pAudioClient)
    SAFE_RELEASE(pCaptureClient)

    return hr;
}

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

int main() {
    temp = ofstream("temp", ios::binary);
    temp.close();

    temp = ofstream("temp", ios::binary | ios::app);

    hhook = SetWindowsHookExA(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    for (thread& t : recorders)
        t.join();

    temp.close();
    system("del temp");

    return 0;
}

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) {
        return CallNextHookEx(hhook, nCode, wParam, lParam);
    }

    KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
    
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            if (p->vkCode == VK_LCONTROL) {
                holds_ctrl = true;
            }
            else if (p->vkCode == VK_LMENU) {
                holds_alt = true;
            }
            else if (holds_alt && holds_ctrl) {
                if (p->vkCode == 'R') {
                    RECORDING = !RECORDING;
                    
                    if (RECORDING) {
                        printf("Recording started...\n");
                        MyAudioSink* sink = new MyAudioSink;
                        recorders.push_back(std::thread(RecordAudioStream, sink));
                    } else {
                        printf("Recording stopped\n");
                    }
                }
                else if (p->vkCode == 'Q') {
                    PostQuitMessage(0);
                }
            }
        }
        else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
            if (p->vkCode == VK_LCONTROL) {
                holds_ctrl = false;
            }
            else if (p->vkCode == VK_LMENU) {
                holds_alt = false;
            }
        }
    }
    
    return CallNextHookEx(hhook, nCode, wParam, lParam);
}