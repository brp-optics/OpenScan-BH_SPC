#include "DataStream.hpp"

#include <FLIMEvents/BHDeviceEvent.hpp>
#include <FLIMEvents/Histogram.hpp>
#include <FLIMEvents/LineClockPixellator.hpp>
#include <FLIMEvents/StreamBuffer.hpp>

#include <array>
#include <functional>
#include <memory>


using SampleType = uint16_t;


namespace {
	class IntensityImageSink : public HistogramProcessor<SampleType> {
		OScDev_Acquisition* acquisition;
		std::shared_ptr<AcquisitionCompletion> downstream;

	public:
		explicit IntensityImageSink(OScDev_Acquisition* acquisition,
			std::shared_ptr<AcquisitionCompletion> downstream) :
			acquisition(acquisition),
			downstream(downstream)
		{
			if (downstream) {
				downstream->AddProcess("IntensityImage");
			}
		}

		void HandleError(std::string const& message) override {
			if (downstream) {
				downstream->HandleError("Stopping intensity images due to error: " + message, "IntensityImage");
				downstream.reset();
			}
		}

		void HandleFrame(Histogram<SampleType> const& histogram) override {
			// TODO OScDev_Acquisition_CallFrameCallback() parameter should be const
			OScDev_Acquisition_CallFrameCallback(acquisition, 0,
				const_cast<void*>(reinterpret_cast<void const*>(histogram.Get())));
		}

		void HandleFinish(Histogram<SampleType>&&, bool) override {
			if (downstream) {
				downstream->HandleFinish("IntensityImage");
				downstream.reset();
			}
		}
	};

	class HistogramSink : public HistogramProcessor<SampleType> {
		unsigned channel;
		std::shared_ptr<SDTWriter> sdtWriter;

	public:
		HistogramSink(unsigned channel, std::shared_ptr<SDTWriter> sdtWriter) :
			channel(channel),
			sdtWriter(sdtWriter)
		{}

		void HandleError(std::string const& message) override {
			if (sdtWriter) {
				sdtWriter->HandleError(message);
				sdtWriter.reset();
			}
		}

		void HandleFrame(Histogram<SampleType> const& histogram) override {
			// Nothing to do until we finish
		}

		void HandleFinish(Histogram<SampleType>&& histogram, bool isCompleteFrame) override {
			// isCompleteFrame is always true because our upstream guarantees it
			if (sdtWriter) {
				sdtWriter->SetHistogram(channel, std::move(histogram));
				sdtWriter.reset();
			}
		}
	};
}


template <typename E, unsigned N>
static void PumpDeviceEvents(std::shared_ptr<EventStream<E>> stream,
	std::array<std::shared_ptr<DeviceEventProcessor>, N> processors)
{
	for (;;) {
		std::shared_ptr<EventBuffer<E>> buffer;
		try {
			buffer = stream->ReceiveBlocking();
		}
		catch (std::exception const& e) {
			for (auto& p : processors) {
				p->HandleError(e.what());
			}
			break;
		}

		if (!buffer) {
			for (auto& p : processors) {
				p->HandleFinish();
			}
			break;
		}

		char const* data = reinterpret_cast<char const*>(buffer->GetData());
		for (auto& p : processors) {
			p->HandleDeviceEvents(data, buffer->GetSize());
		}
	}
}


template <typename T>
static std::shared_ptr<PixelPhotonProcessor> MakeCumulativeHistogrammer(
	uint32_t histoBits, uint32_t inputBits, uint32_t width, uint32_t height,
	std::shared_ptr<HistogramProcessor<T>> downstream)
{
	Histogram<T> frameHisto(histoBits, inputBits, true, width, height);
	Histogram<T> cumulHisto(histoBits, inputBits, true, width, height);
	cumulHisto.Clear();
	return std::make_shared<Histogrammer<T>>(std::move(frameHisto),
		std::make_shared<HistogramAccumulator<T>>(std::move(cumulHisto),
			downstream));
}


// Returns stream to which events should be sent
// Second retval is completion of event pumping, which needs to be stored
// until processing finishes (or else destructor will block).
std::tuple<std::shared_ptr<EventStream<BHSPCEvent>>, std::future<void>>
SetUpProcessing(uint32_t width, uint32_t height, uint32_t maxFrames,
	int32_t lineDelay, uint32_t lineTime, uint32_t lineMarkerBit,
	OScDev_Acquisition* acquisition,
	std::shared_ptr<DeviceEventProcessor> additionalProcessor,
	std::shared_ptr<SDTWriter> histogramWriter,
	std::shared_ptr<AcquisitionCompletion> completion)
{
	uint32_t inputBits = 12;
	uint32_t intensityBits = 0; // Intensity image is 0-bit histogram
	uint32_t histoBits = 8; // TODO Configurable

	// Construct our processing graph starting at downstream.

	auto intensitySink = std::make_shared<IntensityImageSink>(acquisition, completion);

	auto histoSink = std::make_shared<HistogramSink>(0, histogramWriter);

	auto intensityProc = MakeCumulativeHistogrammer<SampleType>(
		intensityBits, inputBits, width, height, intensitySink);

	auto histoProc = MakeCumulativeHistogrammer<SampleType>(
		histoBits, inputBits, width, height, histoSink);

	auto histogrammers = std::make_shared<BroadcastPixelPhotonProcessor<2>>(
		intensityProc, histoProc);

	auto pixellator = std::make_shared<LineClockPixellator>(
		width, height, maxFrames, lineDelay, lineTime, lineMarkerBit,
		histogrammers);

	auto decoder = std::make_shared<BHSPCEventDecoder>(pixellator);

	std::array<std::shared_ptr<DeviceEventProcessor>, 2> procs{
		decoder, additionalProcessor };

	auto stream = std::make_shared<EventStream<BHSPCEvent>>();

	auto done = std::async(std::launch::async, [stream, procs] {
		PumpDeviceEvents(stream, procs);
	});

	return std::make_tuple(stream, std::move(done));
}