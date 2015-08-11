#include <audio_io/audio_io.hpp>
#include <audio_io/private/audio_outputs.hpp>
#include <audio_io/private/sample_format_converter.hpp>
#include <logger_singleton.hpp>
#include <functional>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <utility>
#include <mutex>
#include <atomic>
#include <map>
#include <string.h>
#include <algorithm>
#include <thread>
#include <chrono>
#include <windows.h>
#include <mmreg.h> //WAVEFORMATEXTENSIBLE

namespace audio_io {
namespace implementation {

WAVEFORMATEXTENSIBLE makeFormat(unsigned int channels, unsigned int sr, bool isExtended) {
	//lookup table so we can easily pull out masks.
	unsigned int chanmasks[] = {
		0,
		0,
		SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT,
		0,
		0,
		0,
		SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT,
		0,
		SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT|SPEAKER_FRONT_CENTER|SPEAKER_LOW_FREQUENCY|SPEAKER_BACK_LEFT|SPEAKER_BACK_RIGHT|SPEAKER_SIDE_LEFT|SPEAKER_SIDE_RIGHT,
	};
	WAVEFORMATEXTENSIBLE format;
	format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	format.Format.nSamplesPerSec = sr;
	format.Format.wBitsPerSample = 16;
	format.Format.cbSize = 22; //this comes directly from msdn, which gives no further explanation.
	format.Samples.wValidBitsPerSample = 16;
	format.Format.nAvgBytesPerSec = channels*2*sr;
	format.Format.nBlockAlign = channels*2;
	format.Format.nChannels = channels;
	format.dwChannelMask = chanmasks[channels];
	format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
	if(isExtended == false) {
		format.Format.cbSize = 0;
		format.Format.wFormatTag = WAVE_FORMAT_PCM;
	}
	return format;
}

class WinmmOutputDevice: public  OutputDeviceImplementation {
	public:
	//channels is what user requested, maxChannels is what the device can support at most.
	//maxChannels comes from the DeviceFactory subclass and is cached; thus the parameter here.
	WinmmOutputDevice(std::function<void(float*, int)> getBuffer, unsigned int blockSize, unsigned int channels, unsigned int maxChannels, unsigned int mixAhead, UINT_PTR which, unsigned int sourceSr, unsigned int targetSr);
	~WinmmOutputDevice();
	void stop() override;
	void winmm_mixer();
	HWAVEOUT winmm_handle;
	HANDLE buffer_state_changed_event;
	std::thread winmm_mixing_thread;
	std::vector<WAVEHDR> winmm_headers;
	std::vector<short*> audio_data;
	std::atomic_flag winmm_mixing_flag;
};

WinmmOutputDevice::WinmmOutputDevice(std::function<void(float*, int)> getBuffer, unsigned int blockSize, unsigned int channels, unsigned int maxChannels, unsigned int mixAhead, UINT_PTR which, unsigned int sourceSr, unsigned int targetSr) {
	WAVEFORMATEXTENSIBLE format = {0};
	mixAhead += 1;
	winmm_headers.resize(mixAhead);
	audio_data.resize(mixAhead);
	buffer_state_changed_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	if(buffer_state_changed_event == NULL) {
		std::ostringstream format;
		format<<GetLastError();
		throw AudioIOError(std::string("Winmm: Could not create buffer_state_changed_event.  Windows error: ")+format.str());
	}
	//we try all the channels until we get a device, and then bale out if we've still failed.
	unsigned int chancounts[] = {8, 6, 2};
	MMRESULT res = 0;
	winmm_handle = nullptr;
	bool gotAudio = false;
	unsigned int neededChannels = channels < 2 ? 2 : channels;
	unsigned int inChannels = neededChannels, outChannels = 0;
	for(unsigned int i = 0; i < 3; i++) {
		if(chancounts[i] > neededChannels) continue;
		if(chancounts[i] > maxChannels) continue;
		format = makeFormat(chancounts[i], targetSr, true);
		res = waveOutOpen(&winmm_handle, which, (WAVEFORMATEX*)&format, (DWORD)buffer_state_changed_event, NULL, CALLBACK_EVENT);
		if(res == MMSYSERR_NOERROR) {
			gotAudio = true;
			outChannels = chancounts[i];
			break;
		}
	}
	if(gotAudio == false) { //we can still maybe get something by falling back to a last resort.
		//we make this back into a waveformatex and request stereo.
		format = makeFormat(2, targetSr, false);
		res = waveOutOpen(&winmm_handle, which, (WAVEFORMATEX*)&format, (DWORD)buffer_state_changed_event, NULL, CALLBACK_EVENT);
		if(res != MMSYSERR_NOERROR) throw AudioIOError("Could not open Winmm device with any attempted channel count.");
		outChannels = 2;
	}
	init(getBuffer, blockSize, channels, sourceSr, outChannels, targetSr);
	for(unsigned int i = 0; i < audio_data.size(); i++) audio_data[i] = new short[output_frames*output_channels];
	//we can go ahead and set up the headers.
	for(unsigned int i = 0; i < winmm_headers.size(); i++) {
		winmm_headers[i].lpData = (LPSTR)audio_data[i];
		winmm_headers[i].dwBufferLength = sizeof(short)*output_frames*output_channels;
		winmm_headers[i].dwFlags = WHDR_DONE;
	}
	winmm_mixing_flag.test_and_set();
	winmm_mixing_thread = std::thread([this]() {winmm_mixer();});
}

WinmmOutputDevice::~WinmmOutputDevice() {
	stop();
}

void WinmmOutputDevice::stop() {
	logger_singleton::getLogger()->logInfo("audio_io", "Winmm device shutting down.");
	//Shut down the thread.
	if(winmm_mixing_thread.joinable()) {
		winmm_mixing_flag.clear();
		winmm_mixing_thread.join();
	}
}

void WinmmOutputDevice::winmm_mixer() {
	float* workspace = new float[output_frames*output_channels];
	logger_singleton::getLogger()->logDebug("audio_io", "Winmm mixing thread started.");
	while(winmm_mixing_flag.test_and_set()) {
		while(1) {
			short* nextBuffer = nullptr;
			WAVEHDR* nextHeader = nullptr;
			for(unsigned int i = 0; i < winmm_headers.size(); i++) {
				if(winmm_headers[i].dwFlags & WHDR_DONE) {
					nextBuffer = audio_data[i];
					nextHeader = &winmm_headers[i];
					break;
				}
			}
			if(nextHeader == nullptr || nextBuffer == nullptr) break;
			sample_format_converter->write(output_frames, workspace);
			waveOutUnprepareHeader(winmm_handle, nextHeader, sizeof(WAVEHDR));
			for(unsigned int i = 0; i < output_frames*output_channels; i++) nextBuffer[i] = (short)(workspace[i]*32767);
			nextHeader->dwFlags = 0;
			nextHeader->dwBufferLength = sizeof(short)*output_frames*output_channels;
			nextHeader->lpData = (LPSTR)nextBuffer;
			waveOutPrepareHeader(winmm_handle, nextHeader, sizeof(WAVEHDR));
			waveOutWrite(winmm_handle, nextHeader, sizeof(WAVEHDR));
		}
		WaitForSingleObject(buffer_state_changed_event, 5); //the timeout is to let us detect that we've been requested to die.
	}
	//we prepared these, we need to also kill them.  If we don't, very very bad things happen.
	//This call ends playback.
	waveOutReset(winmm_handle);
	for(auto i= winmm_headers.begin(); i != winmm_headers.end(); i++) {
		auto *header = &*i;
		while((header->dwFlags & WHDR_DONE) == 0) {
			std::this_thread::yield();
		}
		waveOutUnprepareHeader(winmm_handle, header, sizeof(WAVEHDR));
	}
	waveOutClose(winmm_handle);
	winmm_handle=nullptr;
	logger_singleton::getLogger()->logDebug("audio_io", "Winmm mixing thread exiting normally.");
}

class WinmmOutputDeviceFactory: public OutputDeviceFactoryImplementation {
	public:
	WinmmOutputDeviceFactory();
	virtual std::vector<std::string> getOutputNames() override;
	virtual std::vector<int> getOutputMaxChannels() override;
	virtual std::shared_ptr<OutputDevice> createDevice(std::function<void(float*, int)> getBuffer, int index, unsigned int channels, unsigned int sr, unsigned int blockSize, float minLatency, float startLatency, float maxLatency) override;
	virtual unsigned int getOutputCount() override;
	virtual bool scan();
	std::string getName() override;
	private:
	std::vector<std::string> names;
	std::vector<int> max_channels;
	std::vector<unsigned int> srs; //we need this, because these are not easy to query.
	unsigned int mapper_max_channels = 2, mapper_sr = 44100;
};

WinmmOutputDeviceFactory::WinmmOutputDeviceFactory() {
}

std::vector<std::string> WinmmOutputDeviceFactory::getOutputNames() {
	return names;
}

std::vector<int> WinmmOutputDeviceFactory::getOutputMaxChannels() {
	return max_channels;
}

std::shared_ptr<OutputDevice> WinmmOutputDeviceFactory::createDevice(std::function<void(float*, int)> getBuffer, int index, unsigned int channels, unsigned int sr, unsigned int blockSize, float minLatency, float startLatency, float maxLatency) {
	unsigned int mixAhead = 0;
	while(mixAhead*blockSize/(float)sr <= startLatency) mixAhead += 1;
	std::shared_ptr<OutputDeviceImplementation> device = std::make_shared<WinmmOutputDevice>(getBuffer, blockSize, channels, index != -1 ? max_channels[index] : mapper_max_channels, mixAhead, index == -1 ? WAVE_MAPPER : index, sr, index == -1 ? mapper_sr : srs[index]);
	created_devices.push_back(device);
	return device;
}

unsigned int WinmmOutputDeviceFactory::getOutputCount() {
	return names.size();
}

std::string WinmmOutputDeviceFactory::getName() {
	return "Winmm";
}

struct WinmmCapabilities {
	unsigned int sr;
	std::string name;
	unsigned int channels;
};

WinmmCapabilities getWinmmCapabilities(UINT index) {
	WAVEFORMATEXTENSIBLE format;
	WAVEOUTCAPS caps;
	waveOutGetDevCaps(index, &caps, sizeof(caps));
	WinmmCapabilities retval;
	retval.sr = 44100;
	retval.channels = 2;
	//Winmm is old enough that it uses TChar, which changes depending on this define.
	#ifdef UNICODE
	auto name = std::wstring(caps.szPname);
	//We use this function twice. First time to get the size.
	int length = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, NULL, 0, NULL, NULL);
	//It's unclear if we're responsible for reserving the null character or not, so we do it anyway for safety.
	char* buffer = new char[length+1]();
	WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, buffer, length, NULL, NULL);
	buffer[length] = '\0';
	retval.name = std::string(buffer);
	delete[] buffer;
	#else
	retval.name = std::string(caps.szPname);
	#endif
	unsigned int srs[] = {48000, 44100, 22050};
	unsigned int srsCount = 3;
	unsigned int channels[] = {8, 6, 2};
	unsigned int channelsCount = 3;
	for(unsigned int i = 0; i < channelsCount; i++) {
		for(unsigned int j = 0; j < srsCount; j++) {
			format = makeFormat(channels[i], srs[j], true);
			auto res = waveOutOpen(NULL, index, (WAVEFORMATEX*)&format, NULL, NULL, WAVE_FORMAT_QUERY);
			if(res == MMSYSERR_NOERROR) {
				retval.sr = srs[j];
				retval.channels = channels[i];
				goto done;
			}
		}
	}
	done:
	return retval;
}

bool WinmmOutputDeviceFactory::scan() {
	std::vector<std::string> newNames;
	std::vector<int> newMaxChannels;
	std::vector<unsigned int> newSrs; //we need this, because these are not easy to query.
	UINT devs = waveOutGetNumDevs();
	WinmmCapabilities caps;
	for(UINT i = 0; i < devs; i++) {
		caps = getWinmmCapabilities(i);
		std::string name(caps.name);
		//channels.
		unsigned int channels = caps.channels;
		unsigned int sr = caps.sr;
		newMaxChannels.push_back(channels);
		newNames.push_back(name);
		newSrs.push_back(sr);
	}
	this->max_channels = newMaxChannels;
	this->names = newNames;
	this->srs = newSrs;
	caps = getWinmmCapabilities(WAVE_MAPPER);
	mapper_max_channels = caps.channels;
	mapper_sr = caps.sr;
	return true;
}

OutputDeviceFactory* createWinmmOutputDeviceFactory() {
	WinmmOutputDeviceFactory* fact = new WinmmOutputDeviceFactory();
	if(fact->scan() == false) {
		delete fact;
		return nullptr;
	}
	return fact;
}

} //end namespace implementation
} //end namespace audio_io