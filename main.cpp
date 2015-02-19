#include <opus.h>
#include <iostream>
#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>

#include <deque>
#include <vector>
#include <cstdint>
#include <utility>
#include <algorithm>

int main()
{
	CoInitialize(0);

	const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
	const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

	IMMDeviceEnumerator * pEnumerator;
	CoCreateInstance(
		CLSID_MMDeviceEnumerator, NULL,
		CLSCTX_ALL, IID_IMMDeviceEnumerator,
		(void**)&pEnumerator);

	HANDLE hEvent = CreateEvent(0, FALSE, FALSE, 0);
	HANDLE hRenderEvent = CreateEvent(0, FALSE, FALSE, 0);

	WAVEFORMATEX wfe;
	wfe.wFormatTag = WAVE_FORMAT_PCM;
	wfe.nChannels = 1;
	wfe.nSamplesPerSec = 16000;
	wfe.wBitsPerSample = 16;
	wfe.nBlockAlign = wfe.wBitsPerSample * wfe.nChannels / 8;
	wfe.nAvgBytesPerSec = wfe.nSamplesPerSec * wfe.nBlockAlign;
	wfe.cbSize = 0;

	IMMDevice * mic;
	IAudioClient * mic_client;
	IAudioCaptureClient * capture;
	pEnumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &mic);
	{
		IPropertyStore * mic_props;
		mic->OpenPropertyStore(STGM_READ, &mic_props);

		PROPVARIANT val;
		if (SUCCEEDED(mic_props->GetValue(PKEY_Device_FriendlyName, &val)))
		{
			std::wcout << val.bstrVal << std::endl;
			PropVariantClear(&val);
		}

		mic->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&mic_client);

		mic_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_RATEADJUST, 2000000, 0, &wfe, 0);
		mic_client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);

		mic_client->SetEventHandle(hEvent);
	}

	IMMDevice * speaker;
	IAudioClient * speaker_client;
	IAudioRenderClient * render;
	pEnumerator->GetDefaultAudioEndpoint(eRender, eCommunications, &speaker);
	{
		IPropertyStore * props;
		speaker->OpenPropertyStore(STGM_READ, &props);

		PROPVARIANT val;
		if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &val)))
		{
			std::wcout << val.bstrVal << std::endl;
			PropVariantClear(&val);
		}

		speaker->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&speaker_client);

		speaker_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_RATEADJUST, 2000000, 0, &wfe, 0);
		speaker_client->GetService(__uuidof(IAudioRenderClient), (void**)&render);

		speaker_client->SetEventHandle(hRenderEvent);
	}

	int err;
	OpusEncoder * enc = opus_encoder_create(wfe.nSamplesPerSec, wfe.nChannels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
	opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));
	opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

	OpusDecoder * dec = opus_decoder_create(wfe.nSamplesPerSec, wfe.nChannels, &err);

	mic_client->Start();
	speaker_client->Start();


#define COMPRESS

#ifdef COMPRESS
	std::deque<std::vector<uint8_t> > opus_packets;
#else
	std::deque<std::vector<int16_t> > opus_packets;
#endif

	const size_t frame_size = 320;
	int16_t frame_bufs[frame_size];
	size_t current_frame_size = 0;

	for (;;)
	{
		DWORD r;

		if (opus_packets.size())
		{
			HANDLE handles[2] = { hEvent, hRenderEvent };
			r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
		}
		else
		{
			r = WaitForSingleObject(hEvent, INFINITE);
		}

		if (r == WAIT_OBJECT_0)
		{
			for (;;)
			{
				int16_t * buf;
				UINT32 frame_count;
				DWORD flags;
				UINT64 pos;
				HRESULT hr = capture->GetBuffer((BYTE **)&buf, &frame_count, &flags, &pos, 0);
				if (hr == AUDCLNT_S_BUFFER_EMPTY || frame_count == 0)
					break;

				/*if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
					std::cout << "disc\n";*/

#ifdef COMPRESS
				UINT32 orig_frame_count = frame_count;
				while (frame_count)
				{
					size_t rem = (std::min)(frame_size - current_frame_size, frame_count);
					std::copy(buf, buf + rem, frame_bufs + current_frame_size);
					current_frame_size += rem;
					buf += rem;
					frame_count -= rem;

					if (current_frame_size == frame_size)
					{
						uint8_t p[4000];
						int32_t packet_size = opus_encode(enc, frame_bufs, frame_size, p, sizeof p);
						if (packet_size > 0)
							opus_packets.push_back(std::vector<uint8_t>(p, p + packet_size));
						current_frame_size = 0;
					}
				}
#else

				if (frame_count)
				{
					opus_packets.push_back(std::vector<int16_t>(buf, buf + frame_count));
				}
#endif

				capture->ReleaseBuffer(orig_frame_count);
			}
		}

#ifdef COMPRESS
		while (opus_packets.size())
		{
			int16_t * buf;
			HRESULT hr = render->GetBuffer(frame_size, (BYTE **)&buf);
			if (hr == AUDCLNT_E_BUFFER_TOO_LARGE)
				break;

			std::vector<uint8_t> & p = opus_packets.front();
			int frames = opus_decode(dec, p.data(), p.size(), buf, frame_size, 0);

			render->ReleaseBuffer(frames, 0);
			opus_packets.pop_front();
		}
#else
		while (opus_packets.size())
		{
			std::vector<int16_t> & p = opus_packets.front();

			int16_t * buf;
			HRESULT hr = render->GetBuffer(p.size(), (BYTE **)&buf);
			if (hr == AUDCLNT_E_BUFFER_TOO_LARGE)
				break;

			std::copy(p.data(), p.data() + p.size(), buf);

			render->ReleaseBuffer(p.size(), 0);
			opus_packets.pop_front();
		}
#endif
	}


	//opus_encoder_destroy(enc);
}
