// license:BSD-3-Clause
// copyright-holders:Liam Erven
/***************************************************************************

    ui/uispeech.cpp

    Screen reader/text-to-speech output for the internal MAME UI.

    On Windows, announcements are sent to the NVDA screen reader when
    its controller client library (nvdaControllerClient64.dll or
    nvdaControllerClient.dll, placed next to the MAME executable) can be
    loaded and NVDA is running.  Otherwise they are spoken with the
    Microsoft Speech API (SAPI 5) text-to-speech voices built into
    Windows, so speech works out of the box with no screen reader
    installed.

    All speech is produced from a dedicated worker thread so the
    emulator and UI never block waiting for the speech subsystem.

    Other platforms currently have no speech backend and all calls are
    silent no-ops.

***************************************************************************/

#include "emu.h"
#include "uispeech.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <sapi.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace ui::speech {

namespace {

// CLSID_SpVoice and IID_ISpVoice, defined locally so no extra import
// library is needed on any toolchain
CLSID const f_clsid_spvoice = { 0x96749377, 0x3391, 0x11d2, { 0x9e, 0xe3, 0x00, 0xc0, 0x4f, 0x79, 0x73, 0x96 } };
IID const f_iid_ispvoice = { 0x6c44df74, 0x72b9, 0x4992, { 0xa1, 0xec, 0xef, 0x99, 0x6e, 0x04, 0x22, 0xd4 } };

// NVDA controller client entry points
typedef unsigned long (__stdcall *nvda_test_fn)();
typedef unsigned long (__stdcall *nvda_cancel_fn)();
typedef unsigned long (__stdcall *nvda_speak_fn)(wchar_t const *text);


//-------------------------------------------------
//  make_speakable - convert UTF-8 text to a wide
//  string suitable for a speech engine, dropping
//  private use area glyphs used for UI icons and
//  normalising control characters to spaces
//-------------------------------------------------

std::wstring make_speakable(std::string_view text)
{
	if (text.empty())
		return std::wstring();

	int const len = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.length()), nullptr, 0);
	if (len <= 0)
		return std::wstring();

	std::wstring wide(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.length()), wide.data(), len);

	std::wstring result;
	result.reserve(wide.length());
	for (wchar_t const ch : wide)
	{
		if ((ch >= 0xe000) && (ch <= 0xf8ff))
			continue; // private use area - UI icon glyphs mean nothing to a speech engine
		else if ((ch < 0x20) || (ch == 0x7f))
			result.append(L" ");
		else
			result.append(1, ch);
	}
	return result;
}


//-------------------------------------------------
//  speech_engine - worker thread owning the NVDA
//  client library and the SAPI voice
//-------------------------------------------------

class speech_engine
{
public:
	speech_engine() : m_thread(&speech_engine::process, this)
	{
	}

	~speech_engine()
	{
		{
			std::lock_guard<std::mutex> guard(m_mutex);
			m_exiting = true;
			m_queue.clear();
		}
		m_condition.notify_all();
		m_thread.join();
	}

	// an empty string with interrupt set silences speech in progress
	void post(std::wstring &&text, bool interrupt)
	{
		{
			std::lock_guard<std::mutex> guard(m_mutex);
			if (interrupt)
				m_queue.clear();
			m_queue.emplace_back(std::move(text), interrupt);
		}
		m_condition.notify_all();
	}

private:
	void process()
	{
		HRESULT const comres = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

		// try to load the NVDA controller client if the user has dropped it next to the executable
		HMODULE nvda = LoadLibraryW(L"nvdaControllerClient64.dll");
		if (!nvda)
			nvda = LoadLibraryW(L"nvdaControllerClient.dll");
		if (!nvda)
			nvda = LoadLibraryW(L"nvdaControllerClient32.dll");
		nvda_test_fn nvda_test = nullptr;
		nvda_cancel_fn nvda_cancel = nullptr;
		nvda_speak_fn nvda_speak = nullptr;
		if (nvda)
		{
			nvda_test = reinterpret_cast<nvda_test_fn>(GetProcAddress(nvda, "nvdaController_testIfRunning"));
			nvda_cancel = reinterpret_cast<nvda_cancel_fn>(GetProcAddress(nvda, "nvdaController_cancelSpeech"));
			nvda_speak = reinterpret_cast<nvda_speak_fn>(GetProcAddress(nvda, "nvdaController_speakText"));
		}

		// the SAPI voice is created lazily the first time it's needed
		ISpVoice *voice = nullptr;

		while (true)
		{
			std::pair<std::wstring, bool> item;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_condition.wait(lock, [this] () { return m_exiting || !m_queue.empty(); });
				if (m_exiting)
					break;
				item = std::move(m_queue.front());
				m_queue.pop_front();
			}

			// prefer a running NVDA copy - the user hears their own screen reader voice
			bool handled = false;
			if (nvda_test && nvda_speak && (0 == nvda_test()))
			{
				if (item.second && nvda_cancel)
					nvda_cancel();
				if (item.first.empty())
					handled = true;
				else
					handled = (0 == nvda_speak(item.first.c_str()));
			}

			// otherwise fall back to SAPI text-to-speech
			if (!handled && SUCCEEDED(comres))
			{
				if (!voice)
					CoCreateInstance(f_clsid_spvoice, nullptr, CLSCTX_ALL, f_iid_ispvoice, reinterpret_cast<void **>(&voice));
				if (voice)
				{
					DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML;
					if (item.second)
						flags |= SPF_PURGEBEFORESPEAK;
					if (!item.first.empty() || item.second)
						voice->Speak(item.first.c_str(), flags, nullptr);
				}
			}
		}

		if (voice)
		{
			voice->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
			voice->Release();
		}
		if (nvda)
			FreeLibrary(nvda);
		if (SUCCEEDED(comres))
			CoUninitialize();
	}

	std::mutex                                  m_mutex;
	std::condition_variable                     m_condition;
	std::deque<std::pair<std::wstring, bool> >  m_queue;
	bool                                        m_exiting = false;
	std::thread                                 m_thread;
};


speech_engine &engine()
{
	static speech_engine s_engine;
	return s_engine;
}

} // anonymous namespace


bool available()
{
	return true;
}

void speak(std::string_view text, bool interrupt)
{
	std::wstring speakable = make_speakable(text);
	if (speakable.empty() && !interrupt)
		return;
	engine().post(std::move(speakable), interrupt);
}

void stop()
{
	engine().post(std::wstring(), true);
}

} // namespace ui::speech

#else // !defined(_WIN32)

namespace ui::speech {

bool available()
{
	return false;
}

void speak(std::string_view text, bool interrupt)
{
}

void stop()
{
}

} // namespace ui::speech

#endif // defined(_WIN32)
