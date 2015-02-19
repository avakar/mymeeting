#include <opus.h>
#include <iostream>
#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>

#include <zlib.h>

#include <deque>
#include <vector>
#include <cstdint>
#include <utility>
#include <algorithm>

#include <fstream>

struct display
{
	std::wstring device;
	std::wstring name;
	int32_t cx;
	int32_t cy;
	int32_t width;
	int32_t height;
	size_t bits_per_pixel;
};

class video_encoder
{
public:
	video_encoder()
		: z()
	{
		buf.resize(1024 * 1024);
		deflateInit(&z, Z_DEFAULT_COMPRESSION);
	}

	~video_encoder()
	{
		deflateEnd(&z);
	}

	void push_frame(int width, int height, uint8_t * data)
	{
		z.next_in = data;
		z.avail_in = width * height * 4;
		z.next_out = buf.data();
		z.avail_out = buf.size();

		size_t compressed_size = 0;

		while (z.avail_in)
		{
			if (z.avail_out == 0)
			{
				compressed_size += buf.size();
				z.next_out = buf.data();
				z.avail_out = buf.size();
			}

			deflate(&z, Z_NO_FLUSH);
		}

		for (;;)
		{
			if (z.avail_out == 0)
			{
				compressed_size += buf.size();
				z.next_out = buf.data();
				z.avail_out = buf.size();
			}

			int r = deflate(&z, Z_FINISH);
			if (r == Z_STREAM_END)
				break;
		}

		compressed_size += buf.size() - z.avail_out;

		deflateReset(&z);
	}

private:
	z_stream z;
	std::vector<uint8_t> buf;
};

struct window
{
	display m_disp;
	BITMAPINFOHEADER m_bih;
	HDC m_disp_dc;
	HDC m_bmp_dc;
	HBITMAP m_bmp;
	std::vector<uint8_t> m_dib_buffer;

	video_encoder venc;

	window()
		: m_bih()
	{
		std::vector<display> displays;

		DWORD display_index = 0;
		for (;; ++display_index)
		{
			DISPLAY_DEVICE dd = { sizeof dd };
			if (!EnumDisplayDevices(0, display_index, &dd, 0))
				break;

			DEVMODE dm = {};
			dm.dmSize = sizeof dm;
			if (EnumDisplaySettingsEx(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0))
			{
				display d;
				d.device = dd.DeviceName;
				d.name = dd.DeviceString;
				d.cx = dm.dmPosition.x;
				d.cy = dm.dmPosition.y;
				d.width = dm.dmPelsWidth;
				d.height = dm.dmPelsHeight;
				d.bits_per_pixel = dm.dmBitsPerPel;
				displays.push_back(std::move(d));
			}
		}

		m_disp = displays[1];

		m_disp_dc = CreateDC(m_disp.device.c_str(), 0, 0, 0);
		m_bmp_dc = CreateCompatibleDC(0);
		m_bmp = CreateCompatibleBitmap(m_disp_dc, m_disp.width, m_disp.height);
		SelectObject(m_bmp_dc, m_bmp);

		m_bih.biSize = sizeof(BITMAPINFOHEADER);
		m_bih.biWidth = m_disp.width;
		m_bih.biHeight = m_disp.height;
		m_bih.biPlanes = 1;
		m_bih.biBitCount = 32;
		m_bih.biCompression = BI_RGB;

		m_dib_buffer.resize(m_disp.width * m_disp.height * m_disp.bits_per_pixel / 8);
	}

	void capture()
	{
		BitBlt(m_bmp_dc, 0, 0, m_disp.width, m_disp.height, m_disp_dc, 0, 0, SRCCOPY);

		CURSORINFO ci = { sizeof ci };
		if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING))
			DrawIconEx(m_bmp_dc, ci.ptScreenPos.x - m_disp.cx, ci.ptScreenPos.y - m_disp.cy, ci.hCursor, 0, 0, 0, 0, DI_NORMAL);

		GetDIBits(m_bmp_dc, m_bmp, 0, m_disp.height, m_dib_buffer.data(), (BITMAPINFO *)&m_bih, 0);
		venc.push_frame(m_disp.width, m_disp.height, m_dib_buffer.data());
	}

	void blit_to(HDC dc, int cx, int cy, int width, int height)
	{
		StretchBlt(dc, cx, cy, width, height, m_bmp_dc, 0, 0, m_disp.width, m_disp.height, SRCCOPY);
	}
};

LRESULT CALLBACK wndproc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	window * w = (window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	if (!w)
	{
		w = new window();
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
	}

	switch (uMsg)
	{
	case WM_CREATE:
		SetTimer(hwnd, 1, 16, 0);
		return TRUE;
	case WM_TIMER:
		if (wParam == 1)
		{
			w->capture();
			InvalidateRect(hwnd, 0, FALSE);
			return TRUE;
		}
		break;
	case WM_PAINT:
		{
			HDC dc = GetDC(hwnd);

			RECT client;
			GetClientRect(hwnd, &client);
			w->blit_to(dc, 0, 0, client.right, client.bottom);

			ReleaseDC(hwnd, dc);
			ValidateRect(hwnd, 0);
		}
		return TRUE;
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return TRUE;
	case WM_DESTROY:
		PostQuitMessage(0);
		return TRUE;
	}

	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int main()
{
	CoInitialize(0);

	WNDCLASSEXW wce = {};
	wce.cbSize = sizeof wce;
	wce.style = CS_DBLCLKS;
	wce.hCursor = LoadCursor(0, IDC_ARROW);
	wce.lpfnWndProc = &wndproc;
	wce.lpszClassName = L"MainWindow";
	ATOM cls = RegisterClassExW(&wce);

	HWND hWnd = CreateWindowExW(WS_EX_WINDOWEDGE, MAKEINTATOM(cls), L"MyMeeting", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, 0, 0);

	ShowWindow(hWnd, SW_SHOWDEFAULT);

	MSG msg;
	while (GetMessageW(&msg, 0, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	return msg.wParam;

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
