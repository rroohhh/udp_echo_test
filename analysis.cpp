#include "echo_test.hpp"

#include <fstream>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <cereal/archives/portable_binary.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

struct DumpFile
{
	Config config;
	time_t offset_seconds_from_epoch;
	std::vector<SaveInfo> slices;
	uint64_t sent_count;
	uint64_t recv_count;
	RandomDataProvider random_data;
};

struct PacketNoIterator
{
	const std::vector<PacketInfo>& packets;
};


PYBIND11_MAKE_OPAQUE(std::vector<PacketInfo>);

namespace py = pybind11;

PYBIND11_MODULE(analysis, m)
{
	m.def("load_file", [](std::string& filename) {
		std::ifstream in_file(filename, std::ios_base::binary);
		boost::iostreams::filtering_istream in_stream;
		in_stream.push(boost::iostreams::gzip_decompressor());
		in_stream.push(in_file);
		cereal::PortableBinaryInputArchive iarchive(in_stream);
		std::vector<SaveInfo> slices;
		Config config;
		iarchive(config);
		time_t offset_seconds_from_epoch;
		iarchive(offset_seconds_from_epoch);
		while (1) {
			SaveInfo info;

			iarchive(info);
			slices.push_back(info);

			if (info.stop) {
				break;
			}
		}

		uint64_t sent_count;
		uint64_t recv_count;
		iarchive(sent_count);
		iarchive(recv_count);

		auto data_provider =
		    RandomDataProvider(config.msg_size, config.no_different_payload, config.random_seed);

		return DumpFile{
		    config,     offset_seconds_from_epoch,
		    slices,     sent_count,
		    recv_count, {config.msg_size, config.no_different_payload, config.random_seed}};
	});

	py::class_<std::vector<PacketInfo>>(m, "PacketInfoVector")
	    .def(py::init<>())
	    .def(
	        "packet_nos",
	        [](const std::vector<PacketInfo>& v) {
		        std::vector<uint64_t> packet_nos;

		        for (const auto& info : v) {
			        packet_nos.push_back(info.packet_no);
		        }
		        return packet_nos;
	        })
	    .def("__len__", [](const std::vector<PacketInfo>& v) { return v.size(); })
	    .def(
	        "__iter__",
	        [](std::vector<PacketInfo>& v) { return py::make_iterator(v.begin(), v.end()); },
	        py::keep_alive<0, 1>());

	py::class_<Config>(m, "Config")
	    .def_readonly("userspace_timestamp", &Config::userspace_timestamp)
	    .def_readonly("after_send_wait_time", &Config::after_send_wait_time)
	    .def_readonly("use_packet_mmap", &Config::use_packet_mmap)
	    .def_readonly("msg_size", &Config::msg_size)
	    .def_readonly("rate", &Config::rate)
	    .def_readonly("packet_count", &Config::packet_count)
	    .def_readonly(
	        "latency_histogram_log_bucket_width", &Config::latency_histogram_log_bucket_width)
	    .def_readonly("no_different_payload", &Config::no_different_payload)
	    .def_readonly("random_seed", &Config::random_seed)
	    .def_readonly("save_interval", &Config::save_interval);

	py::class_<DumpFile>(m, "DumpFile")
	    .def_readonly("offset_seconds_from_epoch", &DumpFile::offset_seconds_from_epoch)
	    .def_readonly("slices", &DumpFile::slices)
	    .def_readonly("config", &DumpFile::config)
	    .def_readonly("sent_count", &DumpFile::sent_count)
	    .def_readonly("recv_count", &DumpFile::recv_count)
	    .def("get_payload", [](DumpFile& dumpfile) {
		    return py::memoryview::from_memory(
		        (uint8_t*) dumpfile.random_data.data.data(), // buffer pointer
		        {dumpfile.config.no_different_payload * dumpfile.config.msg_size *
		         sizeof(uint8_t)});
	    });

	py::class_<SaveInfo>(m, "SaveInfo")
	    .def_readonly("stop", &SaveInfo::stop)
	    .def_readonly("slice", &SaveInfo::slice)
	    .def_readonly("udp_drops", &SaveInfo::udp_drops)
	    .def_readonly("payload_mismatches", &SaveInfo::payload_mismatches)
	    .def_readonly("start_stop_timing", &SaveInfo::start_stop_timing);


	py::class_<PayloadMismatchInfo>(m, "PayloadMismatchInfo")
	    .def_readonly("packet_no", &PayloadMismatchInfo::packet_no)
	    .def_readonly("expected", &PayloadMismatchInfo::expected)
	    .def_readonly("got", &PayloadMismatchInfo::got);

	py::class_<StartStopTimingInfo>(m, "StartStopTimingInfo")
	    .def_readonly("start", &StartStopTimingInfo::start)
	    .def_readonly("stop", &StartStopTimingInfo::stop)
	    .def_readonly("sent_packets", &StartStopTimingInfo::sent_packets);

	py::class_<SaveSlice>(m, "SaveSlice")
	    .def_readonly("old_tx", &SaveSlice::old_tx)
	    .def_readonly("unmatched_rx", &SaveSlice::unmatched_rx)
	    .def_readonly("hist_slices", &SaveSlice::hist_slices)
	    .def_readonly("num_out_of_order", &SaveSlice::num_out_of_order);

	py::class_<PacketInfo>(m, "PacketInfo")
	    .def_readonly("packet_no", &PacketInfo::packet_no)
	    .def_readonly("timestamp", &PacketInfo::timestamp)
	    .def("__repr__", [](const PacketInfo& self) {
		    return "{ " + std::to_string(self.packet_no) + ", " +
		           std::to_string(self.timestamp.nsecs) + " }";
	    });

	using LatencyHistogramSlice = LatencyHistogram::LatencyHistogramSlice;

	py::class_<LatencyHistogramSlice>(m, "LatencyHistogramSlice")
	    .def_readonly("start", &LatencyHistogramSlice::start)
	    .def_readonly("end", &LatencyHistogramSlice::end)
	    .def_readonly("buckets", &LatencyHistogramSlice::buckets)
	    .def_readonly("count", &LatencyHistogramSlice::count)
	    .def_readonly("per_slice", &LatencyHistogramSlice::per_slice)
	    .def_readonly("log_bucket_width", &LatencyHistogramSlice::log_bucket_width)
	    .def_readonly("latency_sum", &LatencyHistogramSlice::latency_sum)
	    .def_readonly("min", &LatencyHistogramSlice::min)
	    .def_readonly("max", &LatencyHistogramSlice::max);

	py::class_<Timestamp>(m, "Timestamp").def_readonly("nsecs", &Timestamp::nsecs);
}
