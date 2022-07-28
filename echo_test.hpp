#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <random>
#include <thread>
#include <boost/core/noncopyable.hpp>
#include <boost/program_options.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <linux/errqueue.h>
#include <linux/if_packet.h>
#include <sys/mman.h>
#include <sys/socket.h>

namespace po = boost::program_options;

#define DBG(fmt, name) printf(#name " = " fmt "\n", name);

struct Config
{
	bool userspace_timestamp;
	double after_send_wait_time;
	bool use_packet_mmap;
	size_t msg_size;
	double rate;
	uint64_t packet_count;
	double latency_histogram_log_bucket_width;
	uint64_t no_different_payload;
	uint64_t random_seed;
	int rx_cpu_core;
	int tx_cpu_core;
	uint64_t save_interval;
	std::string host;
	std::string port;
	int rcvbuf;
	int sndbuf;
	std::string save_location;

	Config() = default;

	Config(int argc, char** argv)
	{
		timespec timespec;
		int ret = clock_gettime(CLOCK_REALTIME, &timespec);
		if (ret < 0) {
			perror("clock_gettime");
			exit(EXIT_FAILURE);
		}
		auto default_random_seed = (uint64_t) timespec.tv_sec + (uint64_t) timespec.tv_nsec;

		po::options_description desc("Program Usage");
		desc.add_options()("help", "produce help message")(
		    "host,h", po::value(&host)->required(), "set host")(
		    "port,p", po::value(&port)->required(), "set port")(
		    "userspace_timestamp", po::value(&userspace_timestamp)->default_value(true),
		    "use only userspace timestamping")(
		    "after_send_wait_time", po::value(&after_send_wait_time)->default_value(0.5),
		    "time to wait after stopping sending")(
		    "use_packet_mmap", po::value(&use_packet_mmap)->default_value(false),
		    "use TPACKET mmap (requires root)")(
		    "msg_size,s",
		    po::value(&msg_size)->default_value(1024)->notifier([](uint64_t msg_size) {
			    if (msg_size < 8) {
				    throw po::validation_error(
				        po::validation_error::invalid_option_value, "msg_size",
				        std::to_string(msg_size));
			    }
		    }),
		    "udp payload size (atleast 8)")(
		    "rate", po::value(&rate)->default_value(100'000),
		    "number of packets to send per second")(
		    "packet_count", po::value(&packet_count)->default_value(1'000'000'000),
		    "total number of packet to send")(
		    "latency_histogram_log_bucket_width",
		    po::value(&latency_histogram_log_bucket_width)->default_value(1.03),
		    "log bucket size of latency histogram")(
		    "no_different_payload", po::value(&no_different_payload)->default_value(65536),
		    "number of different payloads to use")(
		    "random_seed", po::value(&random_seed)->default_value(default_random_seed),
		    "random seed to use for random payload generation")(
		    "rx_cpu_core", po::value(&rx_cpu_core)->default_value(-1),
		    "cpu core the sending thread is pinned to")(
		    "tx_cpu_core", po::value(&tx_cpu_core)->default_value(-1),
		    "cpu core the receiving thread is pinned to")(
		    "save_interval", po::value(&save_interval)->default_value(100'000),
		    "number of packets sent between saving results")(
		    "sndbuf", po::value(&sndbuf)->default_value(90'000'000), "send buffer size")(
		    "rcvbuf", po::value(&rcvbuf)->default_value(90'000'000), "receive buffer size")(
		    "save_location,o", po::value(&save_location)->default_value("out.gz"),
		    "save file location");

		po::variables_map vm;
		po::store(po::parse_command_line(argc, argv, desc), vm);

		if (vm.count("help")) {
			std::cout << desc << "\n";
			exit(EXIT_FAILURE);
		}

		po::notify(vm);
	}

private:
	friend cereal::access;

	template <class Archive>
	void serialize(Archive& archive)
	{
		archive(
		    userspace_timestamp, after_send_wait_time, use_packet_mmap, msg_size, rate,
		    packet_count, latency_histogram_log_bucket_width, no_different_payload, random_seed,
		    save_interval, host, port, rcvbuf, sndbuf);
	}
};

class UdpStats
{
public:
	UdpStats(short local_port)
	{
		std::stringstream stream;
		stream << std::setfill('0') << std::setw(sizeof(local_port) * 2) << std::uppercase
		       << std::hex << local_port;
		local_port_hex = std::string(stream.str());
	}

	uint64_t read_drops()
	{
		std::ifstream f("/proc/net/udp");
		std::string line;
		while (std::getline(f, line)) {
			if (line.find(local_port_hex) != line.npos) {
				auto pos = line.find_last_not_of(" \n");
				pos = line.rfind(" ", pos);
				auto drops = line.substr(pos);
				return std::stoull(drops);
			}
		}

		return -1;
	}

private:
	std::string local_port_hex;
};


template <typename T>
class Slice
{
public:
	Slice(T* data, size_t len_) : data(data), len_(len_){};

	size_t len()
	{
		return len_;
	}

	T const& operator[](size_t idx)
	{
		return data[idx];
	}

private:
	T* data;
	size_t len_;
};

template <typename T>
class SPSCQueue : boost::noncopyable
{
public:
	SPSCQueue(size_t capacity) : storage(new T[capacity]), capacity(capacity){};
	SPSCQueue(size_t capacity, bool debug) :
	    storage(new T[capacity]), capacity(capacity), debug(debug){};

	// SAFETY: not thread-safe, only allowed from one thread at a time
	bool push(T t)
	{
		const size_t current_index = write_index;
		const size_t next_index = next_write_index(current_index);
		if (debug) {
			DBG("%ld", current_index);
			DBG("%ld", next_index);
			DBG("%ld", read_index.load());
		}
		if (next_index == read_index) {
			return false;
		}
		storage[current_index] = t;
		write_index = next_index;
		return true;
	}

	// SAFETY: not thread-safe, only allowed from one thread at a time
	// there is no checking on the returned read count
	template <typename Func>
	void peek_and_consume(Func const& func)
	{
		const size_t current_read_index = read_index;
		const size_t current_write_index = write_index;

		auto a = Slice(&storage[0], 0);
		auto b = Slice(&storage[0], 0);
		if (current_write_index < current_read_index) {
			a = Slice(&storage[current_read_index], capacity - current_read_index);
			b = Slice(&storage[0], current_write_index);
		} else if (current_write_index > current_read_index) {
			a = Slice(&storage[current_read_index], current_write_index - current_read_index);
		}
		const auto read = func(a, b);
		read_index = new_read_index(current_read_index, read);
	}

	// SAFETY: not thread-safe, only allowed from one thread at a time
	std::optional<T> pop()
	{
		const size_t current_read_index = read_index;
		const size_t current_write_index = write_index;
		if (debug) {
			DBG("%ld", current_read_index);
			DBG("%ld", current_write_index);
		}
		if (current_read_index != current_write_index) {
			auto ptr = &storage[current_read_index];
			auto ret = T(*ptr);
			read_index = new_read_index(current_read_index, 1);
			return ret;
		} else {
			return std::nullopt;
		}
	}

	size_t size()
	{
		size_t size = 0;
		peek_and_consume([&](auto a, auto b) {
			size = a.len() + b.len();
			return 0;
		});
		return size;
	}

	size_t max_size()
	{
		return capacity;
	}

	~SPSCQueue()
	{
		delete[] storage;
	}

private:
	SPSCQueue(const SPSCQueue&) = delete;
	SPSCQueue& operator=(const SPSCQueue&) = delete;

	size_t next_write_index(size_t current)
	{
		const auto next = current + 1;
		if (next >= capacity) {
			return next - capacity;
		} else {
			return next;
		}
	}

	size_t new_read_index(size_t current, size_t read)
	{
		const auto next = current + read;
		if (next >= capacity) {
			return next - capacity;
		} else {
			return next;
		}
	}

	std::atomic<size_t> write_index = 0;
	std::atomic<size_t> read_index = 0;
	T* storage;
	size_t capacity;
	bool debug = false;
};

template <typename T>
class BufferedChannel
{
public:
	BufferedChannel(size_t max_size) : max_size(max_size){};

	void send(T value)
	{
		{
			std::unique_lock lock(queue_lock);
			cv.wait(lock, [&] { return queue.size() < max_size; });
			queue.push(value);
		}
		cv.notify_all();
	}

	T recv()
	{
		T value = [&] {
			std::unique_lock lock(queue_lock);
			cv.wait(lock, [&] { return !queue.empty(); });
			T value = queue.front();
			queue.pop();
			return value;
		}();
		cv.notify_all();
		return value;
	}

private:
	mutable std::mutex queue_lock;
	std::condition_variable cv;
	std::queue<T> queue;
	size_t max_size;
};

struct RandomDataProvider
{
public:
	// packet_size includes 8 bytes packet number, needs to be multiple of 8
	// no_different specifies how many different packets to generate
	// after no_different packets have been grabbed, they will start to repeat
	RandomDataProvider(size_t packet_size, size_t no_different, uint64_t seed) :
	    packet_size(packet_size / 8), no_different(no_different)
	{
		this->data = std::vector<uint64_t>(no_different * packet_size, 0);
		auto data = (uint32_t*) this->data.data();
		auto packet_words = this->packet_size * 2;
		auto rng = std::minstd_rand(seed);
		for (size_t packet = 0; packet < no_different; packet++) {
			for (size_t word = 2; word < packet_words; word++) {
				data[packet * packet_words + word] = rng();
			}
		}
	}

	RandomDataProvider(RandomDataProvider&&) = default;

	void* get_packet_data(uint64_t packet_no)
	{
		auto packet_data = this->packet_data(packet_no);
		packet_data[0] = packet_no;
		return packet_data;
	}

	void* get_payload_data(uint64_t packet_no)
	{
		auto packet_data = this->packet_data(packet_no);
		return &packet_data[1];
	}

	bool check_data(uint8_t* buf, uint64_t packet_no)
	{
		auto packet_data = this->packet_data(packet_no);
		return memcmp(
		           &buf[sizeof(uint64_t)], &packet_data[1], packet_size * sizeof(uint64_t) - 8) ==
		       0;
	}

	std::vector<uint64_t> data;

private:
	uint64_t* packet_data(uint64_t packet_no)
	{
		auto packet = packet_no % no_different;
		return &data[packet * packet_size];
	}

	RandomDataProvider(const RandomDataProvider&) = delete;
	RandomDataProvider& operator=(const RandomDataProvider&) = delete;

	size_t packet_size;
	size_t no_different;
};

class Throttler
{
	using cs = std::chrono::high_resolution_clock;

public:
	Throttler(double packets_per_second) :
	    packets_per_second(packets_per_second), every_n_packets(1)
	{
		for (uint8_t i = 0; i < std::numeric_limits<uint8_t>::max(); i++) {
			samples.push_back({0, cs::now()});
		}
	}

	void step(size_t packet_no)
	{
		auto last_packet_count = samples[sample_index].last_packet_count;
		auto last = samples[sample_index].last;
		bool slept = false;
		if ((packet_no - samples[last_sample_index].last_packet_count) > every_n_packets) {
			// auto now = cs::now();
			// std::chrono::duration<double> dur = now - samples[last_sample_index].last;
			// auto elapsed = dur.count();
			// if (elapsed < 0.0001) {
			// 	every_n_packets += 1;
			// }
			// if (elapsed > 0.0005) {
			// 	if (every_n_packets > 1) {
			// 		every_n_packets -= 1;
			// 	}
			// }

			while (1) {
				auto now = cs::now();
				std::chrono::duration<double> dur = now - last;
				auto elapsed = dur.count();

				auto rate = (packet_no - last_packet_count) / elapsed;

				if (rate > packets_per_second) {
					auto to_sleep_for = (double) (packet_no - last_packet_count) /
					                        packets_per_second * corr_factor -
					                    elapsed;
					std::this_thread::sleep_for(std::chrono::duration<double>(to_sleep_for * 0.1));
					corr_factor *= 0.99;
					slept = true;
				} else {
					if (slept)
						corr_factor *= 1.01;
					samples[sample_index].last_packet_count = packet_no;
					samples[sample_index].last = now;
					last_sample_index = sample_index;
					sample_index = (sample_index + 1) % samples.size();
					break;
				};
			}
		}
	}

private:
	struct ThrottlerSample
	{
		uint64_t last_packet_count;
		cs::time_point last;
	};
	double corr_factor = 1.0;
	double packets_per_second;
	uint64_t every_n_packets;
	std::vector<ThrottlerSample> samples;
	size_t sample_index = 0;
	size_t last_sample_index = 0;
};

struct Timestamp
{
	constexpr static Timestamp max()
	{
		return {std::numeric_limits<uint64_t>::max()};
	}

	constexpr static Timestamp min()
	{
		return {std::numeric_limits<uint64_t>::min()};
	}
	uint64_t nsecs;

	auto operator<=>(const Timestamp&) const = default;

private:
	friend cereal::access;

	template <class Archive>
	void serialize(Archive& archive)
	{
		archive(nsecs);
	}
};

class TimeKeeper
{
public:
	TimeKeeper()
	{
		auto timespec = get_time();
		base_offset = timespec.tv_sec;
	}

	Timestamp get_ts()
	{
		auto timespec = get_time();
		uint64_t sec = timespec.tv_sec;
		uint64_t nsec = timespec.tv_nsec;
		return {(sec - base_offset) * 1'000'000'000 + nsec};
	}

	Timestamp convert_timespec(__kernel_timespec timespec)
	{
		uint64_t sec = timespec.tv_sec;
		uint64_t nsec = timespec.tv_nsec;
		return {(sec - base_offset) * 1'000'000'000 + nsec};
	}

	Timestamp convert_timespec(uint32_t tp_sec, uint32_t tp_nsec)
	{
		uint64_t sec = tp_sec;
		uint64_t nsec = tp_nsec;
		return {(sec - base_offset) * 1'000'000'000 + nsec};
	}

	time_t get_base_offset()
	{
		return base_offset;
	}

private:
	timespec get_time()
	{
		timespec timespec;
		int ret = clock_gettime(CLOCK_REALTIME, &timespec);
		if (ret < 0) {
			perror("clock_gettime");
			exit(EXIT_FAILURE);
		}

		return timespec;
	}

	time_t base_offset;
};

struct PacketInfo
{
	uint64_t packet_no;
	Timestamp timestamp;

private:
	friend cereal::access;

	template <class Archive>
	void serialize(Archive& archive)
	{
		archive(packet_no, timestamp);
	}
};

class LatencyHistogram
{
public:
	class LatencyHistogramSlice
	{
	public:
		Timestamp start = Timestamp::min();
		Timestamp end = Timestamp::max();
		// buckets start at 1ns (we assume a latency less than 1ns is never achieved)
		// bucket 0 is then [1, 1 * bucket_log_width)
		// bucket 1 is then [1 * bucket_log_width, 1 * bucket_log_width * bucket_log_width)
		std::vector<uint64_t> buckets;
		uint64_t count = 0;
		uint64_t per_slice;
		double log_bucket_width;
		double latency_sum = 0;
		double min = std::numeric_limits<double>::infinity();
		double max = 0;

		// we need a default constructor because cereal sucks
		LatencyHistogramSlice() : per_slice(1000), log_bucket_width(std::log(2.0)){};

		LatencyHistogramSlice(uint64_t per_slice, double log_bucket_width) :
		    per_slice(per_slice), log_bucket_width(std::log(log_bucket_width)){};

		bool add(Timestamp tx, Timestamp rx)
		{
			double delta = rx.nsecs - tx.nsecs;
			size_t bucket = (size_t) (std::log(delta) / log_bucket_width);
			// printf(
			//     "tx nsecs: %ld, rx nsecs: %ld, delta: %lf, bucket: %ld\n", tx.nsecs, rx.nsecs,
			//     delta, bucket);
			buckets.resize(std::max(bucket + 1, buckets.size()), 0);
			buckets[bucket] += 1;
			count += 1;
			start = std::min(start, tx);
			end = std::max(end, tx);
			min = std::min(delta, min);
			max = std::max(delta, max);

			// use kahan summation algorithm for latency_sum
			double y = delta - latency_sum_compensation;
			double t = latency_sum + y;
			latency_sum_compensation = (t - latency_sum) - y;
			latency_sum = t;

			return count >= per_slice;
		}

	private:
		double latency_sum_compensation = 0;

		friend cereal::access;

		template <class Archive>
		void serialize(Archive& archive)
		{
			archive(start, end, buckets, count, per_slice, log_bucket_width, latency_sum, min, max);
		}
	};

	LatencyHistogram(uint64_t per_slice, double log_bucket_width) :
	    per_slice(per_slice),
	    log_bucket_width(log_bucket_width),
	    slices(std::vector(1, LatencyHistogramSlice(per_slice, log_bucket_width))){};

	void add(PacketInfo tx, PacketInfo rx)
	{
		if (slices.back().add(tx.timestamp, rx.timestamp)) {
			slices.push_back(LatencyHistogramSlice(per_slice, log_bucket_width));
		}
	}

	std::vector<LatencyHistogramSlice> drain_full_slices()
	{
		std::vector<LatencyHistogramSlice> full_slices;
		full_slices.push_back(slices.back());
		slices.pop_back();
		full_slices.swap(slices);
		return full_slices;
	}

	std::vector<LatencyHistogramSlice> drain_slices()
	{
		std::vector<LatencyHistogramSlice> all_slices(
		    1, LatencyHistogramSlice(per_slice, log_bucket_width));
		all_slices.swap(slices);
		return all_slices;
	}

private:
	uint64_t per_slice;
	double log_bucket_width;
	std::vector<LatencyHistogramSlice> slices;
};

template <typename T>
void dump_vec(std::vector<T> v)
{
	std::cout << "[";
	for (size_t i = 0; i < v.size(); i++) {
		if (i == 0) {
			std::cout << v[i].packet_no;
		} else {
			std::cout << v[i].packet_no << ", ";
		}
	}
	std::cout << "]" << std::endl;
}

class SaveSlice
{
public:
	std::vector<PacketInfo> old_tx;
	std::vector<PacketInfo> unmatched_rx;
	std::vector<LatencyHistogram::LatencyHistogramSlice> hist_slices;
	uint64_t num_out_of_order;

private:
	friend cereal::access;

	template <class Archive>
	void serialize(Archive& archive)
	{
		archive(old_tx, unmatched_rx, hist_slices, num_out_of_order);
	}
};

class StartStopTimingInfo
{
public:
	Timestamp start;
	Timestamp stop;
	uint64_t sent_packets;

private:
	friend cereal::access;

	template <class Archive>
	void serialize(Archive& archive)
	{
		archive(start, stop, sent_packets);
	}
};


struct PayloadMismatchInfo
{
	uint64_t packet_no;
	std::vector<uint8_t> expected;
	std::vector<uint8_t> got;

private:
	friend cereal::access;

	template <class Archive>
	void serialize(Archive& archive)
	{
		archive(packet_no, expected, got);
	}
};

struct SaveInfo
{
	SaveSlice slice;
	std::vector<uint64_t> udp_drops;
	std::vector<PayloadMismatchInfo> payload_mismatches;
	bool stop;
	StartStopTimingInfo start_stop_timing;

private:
	friend cereal::access;

	template <class Archive>
	void serialize(Archive& archive)
	{
		archive(slice, udp_drops, payload_mismatches, stop, start_stop_timing);
	}
};


class RxTxPacketMatcher
{
public:
	RxTxPacketMatcher(SPSCQueue<PacketInfo>& tx_queue, LatencyHistogram latency_histogram) :
	    tx_queue(tx_queue), latency_histogram(latency_histogram)
	{
	}

	// call with a rx PacketInfo
	void add(PacketInfo rx_info)
	{
		if (rx_info.packet_no < old_tx_lower_bound) {
			unmatched_rx.push_back(rx_info);
		} else if (rx_info.packet_no < old_tx_upper_bound) {
			auto iter = old_tx.find(rx_info.packet_no);
			if (iter != old_tx.end()) {
				auto [packet_no, timestamp] = *iter;
				PacketInfo tx_info{packet_no, timestamp};
				latency_histogram.add(tx_info, rx_info);
				num_out_of_order += 1;
				// fprintf(stdout, "found %ld and erased it\n", packet_no);
				old_tx.erase(iter);
			}
		} else {
			enum class SearchStatus
			{
				Found,
				Continue,
				TooFar
			};

			tx_queue.peek_and_consume([&](Slice<PacketInfo> a, Slice<PacketInfo> b) {
				uint64_t read = 0;
				auto do_work = [&](PacketInfo tx_info) {
					read += 1;
					old_tx_upper_bound = tx_info.packet_no + 1;
					if (tx_info.packet_no == rx_info.packet_no) {
						latency_histogram.add(tx_info, rx_info);
						return SearchStatus::Found;
					} else if (tx_info.packet_no < rx_info.packet_no) {
						/* printf("moving to old_tx: rx = %ld, tx = %ld\n", rx_info.packet_no,
						 * tx_info.packet_no); */
						old_tx.insert({tx_info.packet_no, tx_info.timestamp});
						return SearchStatus::Continue;
					} else {
						read -= 1;
						old_tx_upper_bound = tx_info.packet_no;
						return SearchStatus::TooFar;
					}
				};
				bool found = false;
				for (size_t i = 0; i < a.len(); i++) {
					auto status = do_work(a[i]);
					if (status != SearchStatus::Continue) {
						found = status == SearchStatus::Found;
						break;
					}
				}

				if (!found) {
					for (size_t i = 0; i < b.len(); i++) {
						auto status = do_work(b[i]);
						if (status != SearchStatus::Continue) {
							found = status == SearchStatus::Found;
							break;
						}
					}
				}

				if (!found) {
					unmatched_rx.push_back(rx_info);
				}

				return read;
			});
		}
	}

	SaveSlice save_slice(size_t keep_old_tx_num)
	{
		return save_slice(keep_old_tx_num, false);
	}

	SaveSlice flush()
	{
		tx_queue.peek_and_consume([&](Slice<PacketInfo> a, Slice<PacketInfo> b) {
			uint64_t read = 0;
			for (size_t i = 0; i < a.len(); i++) {
				old_tx.insert({a[i].packet_no, a[i].timestamp});
				read++;
			}
			for (size_t i = 0; i < b.len(); i++) {
				old_tx.insert({b[i].packet_no, b[i].timestamp});
				read++;
			}
			return read;
		});
		return save_slice(0, true);
	}

private:
	SaveSlice save_slice(size_t keep_old_tx_num, bool all_hist_slices)
	{
		// printf("old_tx.size() = %ld, unmatched_rx.size() = %ld, tx_queue.size() = %ld\n",
		// old_tx.size(), unmatched_rx.size(), tx_queue.size());
		auto full_slices = all_hist_slices ? latency_histogram.drain_slices()
		                                   : latency_histogram.drain_full_slices();
		std::vector<PacketInfo> unmatched_rx_for_save;
		unmatched_rx.swap(unmatched_rx_for_save);
		size_t num_old_tx_for_save =
		    std::max((int64_t) 0, (int64_t) old_tx.size() - (int64_t) keep_old_tx_num);
		std::vector<PacketInfo> old_tx_for_save;
		// fprintf(stdout, "reserve size = %ld\n", num_old_tx_for_save);
		old_tx_for_save.reserve(num_old_tx_for_save);
		auto iter = old_tx.begin();
		for (size_t i = 0; i < num_old_tx_for_save; i++) {
			old_tx_for_save.push_back({iter->first, iter->second});
			iter = old_tx.erase(iter);
		}
		if (iter != old_tx.end()) {
			old_tx_lower_bound = iter->first;
		}
		// printf("old_tx.size() = %ld, unmatched_rx.size() = %ld, tx_queue.size() = %ld\n",
		// old_tx.size(), unmatched_rx.size(), tx_queue.size());

		return {old_tx_for_save, unmatched_rx_for_save, full_slices, num_out_of_order};
	}

	SPSCQueue<PacketInfo>& tx_queue;

	// packet_no -> Timestamp
	std::map<uint64_t, Timestamp> old_tx;

	// lowest in old_tx
	// this a conservative estimate, used to avoid many unnecessary std::map lookups.
	uint64_t old_tx_lower_bound = 0;
	// one more than the biggest in old_tx
	uint64_t old_tx_upper_bound = 0;

	uint64_t num_out_of_order = 0;

	std::vector<PacketInfo> unmatched_rx;

	LatencyHistogram latency_histogram;
};

class RingDirectionTX
{
public:
	constexpr static int direction_const = PACKET_TX_RING;
};

class RingDirectionRX
{
public:
	constexpr static int direction_const = PACKET_RX_RING;
};

// unfortunately version and direction are not orthogonal,
// because V3 has V2 like TX but the V3 RX differs from that (block based vs frame based)
template <typename Direction>
class TPacketV3
{
public:
	struct TPacketV3ExtraConfig
	{
		unsigned int retire_blk_tov = 0;
		unsigned int sizeof_priv = 0;
		unsigned int feature_req_word = 0;
	};

	constexpr static int version_const = TPACKET_V3;
	using request_type = tpacket_req3;
	using extra_config = TPacketV3ExtraConfig;
	constexpr static int direction_const = Direction::direction_const;

	static void apply_extra(request_type& req, extra_config extra)
	{
		req.tp_sizeof_priv = extra.sizeof_priv;
		req.tp_retire_blk_tov = extra.retire_blk_tov;
		req.tp_feature_req_word = extra.feature_req_word;
	}

	struct block_desc
	{
		uint32_t version;
		uint32_t offset_to_priv;
		tpacket_hdr_v1 h1;
	};


	static void walk_block(uint8_t* block_ptr, const auto& func) requires(
	    std::is_same_v<Direction, RingDirectionRX>)
	{
		block_desc* block_desc = (struct block_desc*) block_ptr;
		if (block_desc->h1.block_status & TP_STATUS_USER) {
			auto num_packets = block_desc->h1.num_pkts;
			tpacket3_hdr* hdr = (tpacket3_hdr*) (block_ptr + block_desc->h1.offset_to_first_pkt);
			for (size_t pkt = 0; pkt < num_packets; pkt++) {
				func(
				    hdr->tp_sec, hdr->tp_nsec, (uint8_t*) hdr + hdr->tp_mac,
				    (uint8_t*) hdr + hdr->tp_net);
				hdr = (tpacket3_hdr*) ((uint8_t*) hdr + hdr->tp_next_offset);
			}

			// tell the kernel we are done with this block
			block_desc->h1.block_status = TP_STATUS_KERNEL;
		}
	}

	static void walk_block(uint8_t* block_ptr, const auto& func) requires(
	    std::is_same_v<Direction, RingDirectionTX>)
	{
		block_desc* block_desc = (struct block_desc*) block_ptr;
		if (block_desc->h1.block_status & TP_STATUS_USER) {
			auto num_packets = block_desc->h1.num_pkts;
			tpacket3_hdr* hdr = (tpacket3_hdr*) (block_ptr + block_desc->h1.offset_to_first_pkt);
			for (size_t pkt = 0; pkt < num_packets; pkt++) {
				func(
				    hdr->tp_sec, hdr->tp_nsec, (uint8_t*) hdr + hdr->tp_mac,
				    (uint8_t*) hdr + hdr->tp_net);
				hdr = (tpacket3_hdr*) ((uint8_t*) hdr + hdr->tp_next_offset);
			}

			// tell the kernel we are done with this block
			block_desc->h1.block_status = TP_STATUS_KERNEL;
		}
	}
};

template <template <typename> typename Version, typename Direction>
class TPacketRing
{
private:
	using VersionWithDirection = Version<Direction>;
	using extra_config = typename VersionWithDirection::extra_config;

public:
	TPacketRing(
	    int fd,
	    unsigned int block_size,
	    unsigned int frame_size,
	    unsigned int block_num,
	    std::optional<sockaddr_ll> bind_address,
	    extra_config extra_config = {}) requires(std::is_same_v<Direction, RingDirectionRX>) :
	    TPacketRing(fd, block_size, frame_size, block_num, extra_config, bind_address){};

	TPacketRing(
	    int fd,
	    unsigned int block_size,
	    unsigned int frame_size,
	    unsigned int block_num,
	    extra_config extra_config = {}) requires(std::is_same_v<Direction, RingDirectionRX>) :
	    TPacketRing(fd, block_size, frame_size, block_num, extra_config, std::nullopt){};

	TPacketRing(
	    int fd,
	    unsigned int block_size,
	    unsigned int frame_size,
	    unsigned int block_num,
	    sockaddr_ll bind_address,
	    extra_config extra_config = {}) :
	    TPacketRing(
	        fd,
	        block_size,
	        frame_size,
	        block_num,
	        extra_config,
	        std::optional<sockaddr_ll>(bind_address)){};

	void walk_blocks(auto const& func)
	{
		for (auto block : blocks) {
			VersionWithDirection::walk_block(block, func);
		}
	}

	void walk_block(size_t block_num, auto const& func)
	{
		if (block_num < blocks.size()) {
			VersionWithDirection::walk_block(blocks[block_num], func);
		}
	}

private:
	TPacketRing(
	    int fd,
	    unsigned int block_size,
	    unsigned int frame_size,
	    unsigned int block_num,
	    extra_config extra_config,
	    std::optional<sockaddr_ll> bind_address) :
	    fd(fd)
	{
		auto ret = setsockopt(
		    fd, SOL_PACKET, PACKET_VERSION, &VersionWithDirection::version_const,
		    sizeof(VersionWithDirection::version_const));
		if (ret < 0) {
			perror("setsockopt PACKET_VERSION");
			exit(EXIT_FAILURE);
		}

		typename VersionWithDirection::request_type req;
		memset(&req, 0, sizeof(req));
		req.tp_block_size = block_size;
		req.tp_frame_size = frame_size;
		req.tp_block_nr = block_num;
		req.tp_frame_nr = (block_size * block_num) / frame_size;
		VersionWithDirection::apply_extra(req, extra_config);
		ret = setsockopt(fd, SOL_PACKET, VersionWithDirection::direction_const, &req, sizeof(req));
		if (ret < 0) {
			perror("setsockopt ring req");
			exit(EXIT_FAILURE);
		}

		map = (uint8_t*) mmap(
		    NULL, block_size * block_num, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, fd, 0);
		if (map == MAP_FAILED) {
			perror("mmap");
			exit(EXIT_FAILURE);
		}

		blocks = std::vector<uint8_t*>(block_num);
		for (size_t i = 0; i < block_num; i++) {
			blocks[i] = map + i * block_size;
		}

		if (bind_address) {
			auto unwrapped_bind_address = *bind_address;
			ret = bind(
			    fd, (struct sockaddr*) &unwrapped_bind_address, sizeof(unwrapped_bind_address));
			if (ret < 0) {
				perror("bind");
				exit(EXIT_FAILURE);
			}
		}
	}

	// TODO(robin): cleanup in destructor

	int fd;
	uint8_t* map;
	std::vector<uint8_t*> blocks;
};


// TODO(robin): collect payload mismatches and save them
