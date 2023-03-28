///////////////////////////////////////////////////////////////////////////////
// FILE:          AcquireCamera.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   Micro-manager device adapter wrapper for CZI Acquire module
//
// AUTHOR:        Nenad Amodaj nenad@amodaj.com
//
// COPYRIGHT:     2023 Chan Zuckerberg Initiative (CZI)
// LICENSE:       Licensed under the Apache License, Version 2.0 (the "License");
//                you may not use this file except in compliance with the License.
//                You may obtain a copy of the License at
//                
//                http://www.apache.org/licenses/LICENSE-2.0
//                
//                Unless required by applicable law or agreed to in writing, software
//                distributed under the License is distributed on an "AS IS" BASIS,
//                WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//                See the License for the specific language governing permissions and
//                limitations under the License.

#include "AcquireCamera.h"
#include "cpx.h"
#include "device/device.manager.h"
#include <sstream>
#include <chrono>
#include <thread>


/// Helper for passing size static strings as function args.
/// For a function: `f(char*,size_t)` use `f(SIZED("hello"))`.
/// Expands to `f("hello",5)`.
#define SIZED(str) str, sizeof(str) - 1

using namespace std;

const char* cameraName = "AcquireCamera";
AcquireCamera* AcquireCamera::g_instance = nullptr;
const int DEMO_IMAGE_WIDTH = 640;
const int DEMO_IMAGE_HEIGHT = 480;

AcquireCamera::AcquireCamera() : initialized_(false), multiChannel(true), demo(true)
{
	CreateProperty(MM::g_Keyword_Name, cameraName, MM::String, true);

	// Description
	CreateProperty(MM::g_Keyword_Description, "Records simultaneously from two Hammamatsu cameras", MM::String, true);

	// CameraName
	CreateProperty(MM::g_Keyword_CameraName, "AcquireCamera", MM::String, true);

	// CameraID
	CreateProperty(MM::g_Keyword_CameraID, "V1.0", MM::String, true);

	// demo mode
	CreateProperty(g_prop_Demo, "1", MM::Integer, true);
}

AcquireCamera::~AcquireCamera()
{
	Shutdown();
}

int AcquireCamera::Initialize()
{
	if (initialized_)
		return DEVICE_OK;

	long demoVal;
	GetProperty(g_prop_Demo, demoVal);
	demo = demoVal == 1L ? true : false;

	// binning
	CreateProperty(MM::g_Keyword_Binning, "1", MM::Integer, false);

	std::vector<std::string> binningValues;
	binningValues.push_back("1");
	SetAllowedValues(MM::g_Keyword_Binning, binningValues);

	// mode
	auto pAct = new CPropertyAction(this, &AcquireCamera::OnImageMode);
	CreateProperty(g_prop_Mode, g_prop_Mode_Multi, MM::String, false, pAct);
	AddAllowedValue(g_prop_Mode, g_prop_Mode_Multi);
	AddAllowedValue(g_prop_Mode, g_prop_Mode_Single);
	multiChannel = true; // default

	setupBuffers();

	// test cpx loading
	g_instance = this;
	cpx = cpx_init(AcquireCamera::reporter);
	auto dm = cpx_device_manager(cpx);
	if (!cpx || !dm)
	{
		g_instance = nullptr;
		return ERR_CPX_INIT;
	}

	CpxProperties props = {};
	int ret = getCpxProperties(props);
	if (ret != CpxStatus_Ok)
		return ret;

	// set up simulated cameras
	ret = device_manager_select(dm, DeviceKind_Camera, SIZED("simulated.*random.*"), &props.video[0].camera.identifier);
	if (ret != CpxStatus_Ok)
		return ret;

	ret = device_manager_select(dm, DeviceKind_Camera, SIZED("simulated.*sin.*"), &props.video[1].camera.identifier);
	if (ret != CpxStatus_Ok)
		return ret;


	// we are assuming that cameras are identical
	CreateProperty("LineIntervalUs", to_string(props.video[0].camera.settings.line_interval_us).c_str(), MM::Float, false);

	initialized_ = true;
	return DEVICE_OK;
}

int AcquireCamera::Shutdown()
{
	if (cpx)
	{
		auto ret = cpx_shutdown(cpx);
		if (ret != CpxStatus_Ok)
			LogMessage("cpx_shutdown error: " + ret);
		cpx = nullptr;
		g_instance = nullptr;
	}

	initialized_ = false;

	return DEVICE_OK;
}

void AcquireCamera::GetName(char * name) const
{
	CDeviceUtils::CopyLimitedString(name, cameraName);
}

long AcquireCamera::GetImageBufferSize() const
{
	return imgs[0].Width() * imgs[0].Height() * imgs[0].Depth();
}

unsigned AcquireCamera::GetBitDepth() const
{
	return imgs[0].Depth() * 8;
}

int AcquireCamera::GetBinning() const
{
	return 1;
}

int AcquireCamera::SetBinning(int)
{
	return DEVICE_OK;
}

void AcquireCamera::SetExposure(double exposure)
{
	CpxProperties props = {};
	int ret = getCpxProperties(props);
	if (ret != DEVICE_OK)
		LogMessage("Error obtaining properties: code=" + ret);
	props.video[0].camera.settings.exposure_time_us = (float)(exposure * 1000);
	props.video[1].camera.settings.exposure_time_us = props.video[0].camera.settings.exposure_time_us;
	ret = setCpxProperties(props);
	if (ret != DEVICE_OK)
		LogMessage("Error setting exposure: code=" + ret);

}

double AcquireCamera::GetExposure() const
{
	CpxProperties props = {};
	int ret = getCpxProperties(props);
	if (ret != DEVICE_OK)
		LogMessage("Error obtaining properties: code=" + ret);
	return props.video[0].camera.settings.exposure_time_us / 1000.0;
}

int AcquireCamera::SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
{
	return DEVICE_OK;
}

int AcquireCamera::GetROI(unsigned & x, unsigned & y, unsigned & xSize, unsigned & ySize)
{

	return DEVICE_OK;
}

int AcquireCamera::ClearROI()
{
	return DEVICE_OK;
}

int AcquireCamera::IsExposureSequenceable(bool & isSequenceable) const
{
	isSequenceable = false;

	return DEVICE_OK;
}

const unsigned char * AcquireCamera::GetImageBuffer()
{
	return imgs[0].GetPixels();
}

const unsigned char* AcquireCamera::GetImageBuffer(unsigned channel)
{
	if (channel > imgs.size() - 1)
		return nullptr;

	return imgs[channel].GetPixels();
}

unsigned AcquireCamera::GetNumberOfComponents() const
{
	return 1;
}

unsigned AcquireCamera::GetNumberOfChannels() const
{
	return (unsigned)imgs.size();
}

int AcquireCamera::GetChannelName(unsigned channel, char* name)
{
	if (channel > imgs.size()-1)
		return DEVICE_NONEXISTENT_CHANNEL;

	string chName = (channel == 0 ? "Camera-1" : "Camera-2");
	CDeviceUtils::CopyLimitedString(name, chName.c_str());
	return DEVICE_OK;
}

unsigned AcquireCamera::GetImageWidth() const
{
	return imgs[0].Width();
}

unsigned AcquireCamera::GetImageHeight() const
{
	return imgs[0].Height();
}

unsigned AcquireCamera::GetImageBytesPerPixel() const
{
	return imgs[0].Depth();
}

int AcquireCamera::SnapImage()
{
	CpxProperties props = {};
	getCpxProperties(props);
	auto dm = cpx_device_manager(cpx);

	device_manager_select(dm,
		DeviceKind_Camera,
		SIZED("simulated.*random.*"),
		&props.video[0].camera.identifier);

	device_manager_select(dm,
		DeviceKind_Camera,
		SIZED("simulated.*sin.*"),
		&props.video[1].camera.identifier);

	device_manager_select(dm,
		DeviceKind_Storage,
		SIZED("Trash"),
		&props.video[0].storage.identifier);

	device_manager_select(dm,
		DeviceKind_Storage,
		SIZED("Trash"),
		&props.video[1].storage.identifier);

	props.video[0].camera.settings.binning = 1;
	props.video[0].camera.settings.pixel_type = imgs[0].Depth() == 2 ? SampleType_u16 : SampleType_u8;
	props.video[0].camera.settings.shape = {imgs[0].Width(), imgs[0].Height()};
	props.video[0].max_frame_count = 1;

	props.video[1].camera.settings.binning = 1;
	props.video[1].camera.settings.pixel_type = imgs[0].Depth() == 2 ? SampleType_u16 : SampleType_u8;
	props.video[1].camera.settings.shape = { imgs[0].Width(), imgs[0].Height() };
	props.video[1].max_frame_count = 1;

	int ret = cpx_configure(cpx, &props);
	if (ret != CpxStatus_Ok)
	{
		LogMessage("cpx_configure failed");
		return ERR_CPX_CONFIURE_FAILED;
	}

	// start single frame
	ret = cpx_start(cpx);
	if (ret != CpxStatus_Ok)
		throw std::exception("cpx_start failed");

	readFrames(props);

	cpx_stop(cpx);

	return DEVICE_OK;
}

int AcquireCamera::StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow)
{
	return CCameraBase::StartSequenceAcquisition(numImages, interval_ms, stopOnOverflow);
}

int AcquireCamera::StopSequenceAcquisition()
{
	return CCameraBase::StopSequenceAcquisition();
}

int AcquireCamera::getCpxProperties(CpxProperties& props) const
{
	props = {};
	return cpx_get_configuration(cpx, &props);
}

int AcquireCamera::setCpxProperties(CpxProperties& props)
{
	return cpx_configure(cpx, &props);
}

// Send message to micro-manager log
void AcquireCamera::reporter(int is_error, const char* file, int line, const char* function, const char* msg)
{
	const int maxLength(6000);
	char buffer[maxLength];
	snprintf(buffer, maxLength,
		"%s%s(%d) - %s: %s",
		is_error ? "ERROR " : "",
		file,
		line,
		function,
		msg);
	if (g_instance)
	{
		g_instance->LogMessage(buffer);
	}
}

int AcquireCamera::readFrame(int stream, CpxProperties& props)
{
	const auto next = [](VideoFrame* cur) -> VideoFrame* {
		return (VideoFrame*)(((uint8_t*)cur) + cur->bytes_of_frame);
	};

	const auto consumed_bytes = [](const VideoFrame* const cur,
		const VideoFrame* const end) -> size_t {
			return (uint8_t*)end - (uint8_t*)cur;
	};

	// resize buffer
	const int pixelSizeBytes = 1; // TODO: this has to be variable depending on the pixel type

	VideoFrame* beg, * end;
	cpx_map_read(cpx, stream, &beg, &end);
	while (beg == end)
	{
		cpx_map_read(cpx, stream, &beg, &end);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		// TODO: timeout
	}
	memcpy(imgs[stream].GetPixelsRW(), beg->data, beg->bytes_of_frame - sizeof(VideoFrame));
	uint32_t n = (uint32_t) consumed_bytes(beg, end);
	cpx_unmap_read(cpx, stream, n);

	return 0;
}

int AcquireCamera::readFrames(CpxProperties& props)
{
	const auto next = [](VideoFrame* cur) -> VideoFrame* {
		return (VideoFrame*)(((uint8_t*)cur) + cur->bytes_of_frame);
	};

	const auto consumed_bytes = [](const VideoFrame* const cur,
		const VideoFrame* const end) -> size_t {
			return (uint8_t*)end - (uint8_t*)cur;
	};

	VideoFrame* beg, * end;
	// read first frame and place it in the first image buffer
	cpx_map_read(cpx, 0, &beg, &end);
	while (beg == end)
	{
		cpx_map_read(cpx, 0, &beg, &end);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	memcpy(imgs[0].GetPixelsRW(), beg->data, beg->bytes_of_frame - sizeof(VideoFrame));
	uint32_t n = (uint32_t)consumed_bytes(beg, end);
	cpx_unmap_read(cpx, 0, n);

	// read second frame and place it in the second buffer or append to the first
	// depending on the multiChannel flag
	cpx_map_read(cpx, 1, &beg, &end);
	while (beg == end)
	{
		cpx_map_read(cpx, 1, &beg, &end);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (multiChannel)
		memcpy(imgs[1].GetPixelsRW(), beg->data, beg->bytes_of_frame - sizeof(VideoFrame));
	else
		memcpy(imgs[0].GetPixelsRW() + (uintptr_t)imgs[0].Width() * imgs[0].Height() * imgs[0].Depth() / 2,
				beg->data, beg->bytes_of_frame - sizeof(VideoFrame));

	n = (uint32_t)consumed_bytes(beg, end);
	cpx_unmap_read(cpx, 1, n);

	return 0;
}

void AcquireCamera::setupBuffers()
{
	// TODO: obtain pixel depth
	const int pixDepth = 1;
	if (demo)
	{
		imgs.clear();
		if (multiChannel)
		{
			imgs.resize(2); // two images
			imgs[0].Resize(DEMO_IMAGE_WIDTH, DEMO_IMAGE_HEIGHT, pixDepth);
			imgs[1].Resize(DEMO_IMAGE_WIDTH, DEMO_IMAGE_HEIGHT, pixDepth);
		}
		else
		{
			imgs.resize(1); // single image
			imgs[0].Resize(DEMO_IMAGE_WIDTH, DEMO_IMAGE_HEIGHT * 2, pixDepth);
		}
	}
	else
	{
		// TODO
	}

}


//////////////////////////////////////////////////////////////////////////////////////////////
// Property Handlers
//////////////////////////////////////////////////////////////////////////////////////////////
int AcquireCamera::OnImageMode(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(multiChannel ? g_prop_Mode_Multi : g_prop_Mode_Single);
	}
	else if (eAct == MM::AfterSet)
	{
		string mode;
		pProp->Get(mode);
		if (mode.compare(g_prop_Mode_Multi) == 0)
		{
			multiChannel = true;
		}
		else
		{
			multiChannel = false;
		}

		setupBuffers();
	}

	return DEVICE_OK;
}
