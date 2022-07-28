#include "echo_test.hpp"

#include <csignal>
#include <netdb.h>
#include <string>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <cereal/archives/portable_binary.hpp>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/net_tstamp.h>
#include <netinet/in.h>

std::sig_atomic_t should_stop = false;

void signal_handler(int sig)
{
	if (sig == SIGTERM) {
		fprintf(stderr, "SIGTERM received, exiting\n");
	} else if (sig == SIGINT) {
		fprintf(stderr, "SIGINT received, exiting\n");
	} else {
		fprintf(stderr, "unknown signal received, exiting\n");
	}

	should_stop = true;
}

void pin_thread(int cpu_id)
{
	cpu_set_t cpuset;
	pthread_t thread;

	thread = pthread_self();

	CPU_ZERO(&cpuset);
	CPU_SET(cpu_id, &cpuset);

	auto s = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
	if (s != 0)
		perror("pthread_setaffinity_np");
}

int main(int argc, char** argv)
{
	for (auto signal : {SIGTERM, SIGINT}) {
		std::signal(signal, signal_handler);
	}

	timespec timespec;
	int ret = clock_gettime(CLOCK_REALTIME, &timespec);
	if (ret < 0) {
		perror("clock_gettime");
		exit(EXIT_FAILURE);
	}

	Config config(argc, argv);

	std::atomic<bool> done = false;
	std::atomic<bool> start_sending = false;
	std::atomic<bool> should_save = false;
	const size_t MSG_WIRE_SIZE = config.msg_size + 7 + 1 + 12 + 18 + 20 + 8;
	auto data_provider =
	    RandomDataProvider(config.msg_size, config.no_different_payload, config.random_seed);
	const auto KEEP_OLD_TX_NUM =
	    config.rate * config.after_send_wait_time; // we assume the after send wait time is
	                                               // choosen such that it covers the latency
	auto throttler = Throttler(config.rate);
	auto time_keeper = TimeKeeper();
	auto tx_queue = SPSCQueue<PacketInfo>(10'000);
	auto fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;

	struct addrinfo* result;

	int s = getaddrinfo(config.host.c_str(), config.port.c_str(), &hints, &result);

	if (s != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
		exit(EXIT_FAILURE);
	}

	auto found_addr = false;
	for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) != -1) {
			found_addr = true;
			break;
		}
	}
	freeaddrinfo(result);
	if (!found_addr) {
		fprintf(stderr, "could not find a address to connect to\n");
		exit(EXIT_FAILURE);
	}

	sockaddr_in sockaddr;
	socklen_t sockaddr_len = sizeof(sockaddr);
	ret = getsockname(fd, (struct sockaddr*) &sockaddr, &sockaddr_len);
	if (ret < 0) {
		perror("getsockname");
	}
	int local_port = ntohs(sockaddr.sin_port);
	fprintf(stderr, "local port is %d\n", local_port);

	ret = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &config.sndbuf, sizeof(config.sndbuf));
	if (ret != 0) {
		fprintf(stderr, "error setsockopt SO_SNDBUF : %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	ret = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &config.rcvbuf, sizeof(config.rcvbuf));
	if (ret != 0) {
		fprintf(stderr, "error setsockopt SO_RCVBUF : %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 100;
	ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*) &tv, sizeof tv);
	if (ret != 0) {
		perror("setsockopt SO_RCVTIMEO");
	}

	ret = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*) &tv, sizeof tv);
	if (ret != 0) {
		perror("setsockopt SO_SNDTIMEO");
	}


	if (!config.use_packet_mmap && !config.userspace_timestamp) {
		int opts = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
		ret = setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &opts, sizeof(opts));
		if (ret != 0) {
			perror("setsockopt SO_TIMESTAMPING");
		}
	}

	std::atomic<uint64_t> sent_packets = 0;
	std::mutex udp_drops_start_stop_mutex;
	std::vector<uint64_t> udp_drops_list;

	std::deque<StartStopTimingInfo> start_stop_timing;

	auto sender = std::thread([&]() {
		UdpStats udp_stats(local_port);
		pin_thread(config.tx_cpu_core);
		using cs = std::chrono::high_resolution_clock;
		while (!start_sending)
			__builtin_ia32_pause();
		auto last = cs::now();
		auto last_save = time_keeper.get_ts();
		for (sent_packets = 0; sent_packets < config.packet_count; sent_packets++) {
			throttler.step(sent_packets);
			bool pushed = false;
			// fprintf(stdout, "starting send %ld\n", (uint64_t) sent_packets);
			Timestamp ts = time_keeper.get_ts();
			if ((size_t) send(
			        fd, data_provider.get_packet_data(sent_packets), config.msg_size, 0) !=
			    config.msg_size) {
				perror("send");
				sent_packets = sent_packets - 1;
				continue;
			}
			// fprintf(stdout, "did the send %ld\n", (uint64_t) sent_packets);
			while (!(pushed = tx_queue.push({sent_packets, ts})) && !should_stop) {
				// NOTE(robin): c++ does not have a spin lock loop hint :(
				__builtin_ia32_pause();
			};

			if (pushed) {
				if (((sent_packets + 1) % config.save_interval == 0) ||
				    ((sent_packets + 1) == config.packet_count) || should_stop) {
					{
						std::unique_lock lock(udp_drops_start_stop_mutex);
						udp_drops_list.push_back(udp_stats.read_drops());
						start_stop_timing.push_back(
						    {last_save, last_save = time_keeper.get_ts(), sent_packets + 1});
					}
					// set this after adding the udp drops, so we can use these already when saving
					should_save = true;
					std::chrono::duration<double> dur = cs::now() - last;
					auto elapsed = dur.count();
					auto rate = (sent_packets + 1) / elapsed;
					printf(
					    "\u001b[31m[tx] count = %ld, rate = %lf, bw = %lf\u001b[0m\n",
					    (sent_packets + 1), rate,
					    rate * MSG_WIRE_SIZE * 8.0 / 1000.0 / 1000.0 / 1000.0);
				}
			}
			// printf("sent %ld\n", (uint64_t) sent_packets);
			if (should_stop) {
				break;
			}
		}

		std::this_thread::sleep_for(std::chrono::duration<double>(config.after_send_wait_time));
		{
			std::unique_lock lock(udp_drops_start_stop_mutex);
			start_stop_timing.push_back(
			    {last_save, last_save = time_keeper.get_ts(), sent_packets + 1});
		}
		done = true;
	});

	std::mutex payload_mismatch_mutex;
	std::vector<std::pair<uint64_t, uint8_t*>> payload_mismatch_list;
	auto rx_buffer_output_queue = SPSCQueue<PacketInfo>(10'000);

	auto receiver = std::thread([&]() {
		pin_thread(config.rx_cpu_core);
		if (config.use_packet_mmap) {
			auto mmap_fd = socket(AF_PACKET, SOCK_DGRAM, ETH_P_IP);
			if (mmap_fd < 0) {
				perror("socket(AF_PACKET)");
			}

			sock_filter filter_code[] = {
			    // load IPv4 protcol field
			    {BPF_LD + BPF_B + BPF_ABS, 0, 0, 9},
			    // check for udp
			    {BPF_JMP + BPF_JEQ + BPF_K, 0 /* true offset */, 1 /* false offset */, IPPROTO_UDP},
			    // load UDP destination port
			    {BPF_LD + BPF_H + BPF_ABS, 0, 0, 22},
			    // check for our local port
			    {BPF_JMP + BPF_JEQ + BPF_K, 0 /* true offset */, 1 /* false offset */,
			     (__u32) local_port},
			    // load UDP length
			    {BPF_LD + BPF_H + BPF_ABS, 0, 0, 24},
			    // check for our message size + 8 bytes of UDP header
			    {BPF_JMP + BPF_JEQ + BPF_K, 0 /* true offset */, 1 /* false offset */,
			     (__u32) (config.msg_size + 8)},
			    // return -1 -> do not drop packet
			    {BPF_RET + BPF_K, 0, 0, (__u32) -1},
			    // return 0 -> drop packet
			    {BPF_RET + BPF_K, 0, 0, 0},
			};
			struct sock_fprog filter_prog = {
			    .len = sizeof(filter_code) / sizeof(filter_code[0]), .filter = filter_code};

			auto ret = setsockopt(
			    mmap_fd, SOL_SOCKET, SO_ATTACH_FILTER, &filter_prog, sizeof(filter_prog));
			if (ret < 0) {
				perror("setsockopt SO_ATTACH_FILTER");
				exit(EXIT_FAILURE);
			}

			unsigned int block_size = 1 << 18;
			unsigned int frame_size = 1 << 11;
			unsigned int block_num = 8;

			TPacketRing<TPacketV3, RingDirectionRX> ring{
			    mmap_fd, block_size, frame_size, block_num,
			    sockaddr_ll{
			        .sll_family = PF_PACKET,
			        .sll_protocol = htons(ETH_P_IP),
			        .sll_ifindex = 0, // matches all
			        .sll_hatype = 0,
			        .sll_pkttype = PACKET_HOST, // packets addressed to the local host
			        .sll_halen = 0,
			        .sll_addr = {0}}};

			start_sending = true;
			uint64_t next_packet_no = 0;
			while (true) {
				ring.walk_blocks(
				    [&](uint32_t tp_sec, uint32_t tp_nsec, uint8_t* /* mac */, uint8_t* net) {
					    iphdr* iphdr = (struct iphdr*) net;

					    // UDP header is 2 bytes source port, 2 bytes dest port, 2 bytes lenth, 2
					    // bytes checksum = 8
					    uint8_t* udp_payload = (net + iphdr->ihl * 4 + 8);
					    uint64_t packet_no = 0;
					    memcpy(&packet_no, udp_payload, sizeof(uint64_t));
					    /*
					    if (next_packet_no != packet_no) {
					        fprintf(
					            stderr, "packet_no jumped 0x%08lx <-> 0x%08lx\n", next_packet_no,
					            packet_no);
					    }
					    */

					    if (!data_provider.check_data(udp_payload, packet_no)) {
						    fprintf(stdout, "payload mismatch\n");
					    } else {
						    while (!rx_buffer_output_queue.push(
						        {packet_no, time_keeper.convert_timespec(tp_sec, tp_nsec)})) {
							    __builtin_ia32_pause();
						    }

						    next_packet_no = packet_no + 1;
					    }
				    });

				if (done) {
					tpacket_stats_v3 stats;
					socklen_t len = sizeof(stats);
					auto ret = getsockopt(mmap_fd, SOL_PACKET, PACKET_STATISTICS, &stats, &len);
					if (ret < 0) {
						perror("getsockopt PACKET_STATISTICS");
						exit(EXIT_FAILURE);
					}
					fprintf(
					    stderr, "tp_packets %u, tp_drops %u, tp_freeze_q_cnt %u\n",
					    stats.tp_packets, stats.tp_drops, stats.tp_freeze_q_cnt);
					break;
				}
			}
		} else {
			const auto cmsg_size =
			    CMSG_SPACE(sizeof(scm_timestamping)) + CMSG_SPACE(sizeof(sock_extended_err));
			uint8_t* cmsg_buf = new uint8_t[cmsg_size];
			uint8_t* msg_buf = new uint8_t[config.msg_size];

			iovec iov = {
			    .iov_base = msg_buf,
			    .iov_len = config.msg_size,
			};
			msghdr msg = {
			    .msg_name = 0,
			    .msg_namelen = 0,
			    .msg_iov = &iov,
			    .msg_iovlen = 1,
			    .msg_control = cmsg_buf,
			    .msg_controllen = cmsg_size,
			    .msg_flags = 0,
			};

			uint64_t next_packet_no = 0;
			start_sending = true;
			while (1) {
				msg.msg_controllen = cmsg_size;
				iov.iov_base = msg_buf;

				auto ret = recvmsg(fd, &msg, 0);
				if (ret > 0) {
					Timestamp ts{1};
					if (config.userspace_timestamp) {
						ts = time_keeper.get_ts();
					} else {
						struct cmsghdr* cmsg;
						for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
							if (cmsg->cmsg_level == SOL_SOCKET &&
							    cmsg->cmsg_type == SO_TIMESTAMPING) {
								auto rx_ts = *(__kernel_timespec*) CMSG_DATA(cmsg);
								ts = time_keeper.convert_timespec(rx_ts);
							}
						};
					}

					uint64_t packet_no = 0;
					memcpy(&packet_no, msg_buf, sizeof(uint64_t));
					// fprintf(stderr, "got packet %ld\n", packet_no);
					/*
					if (next_packet_no != packet_no) {
					    fprintf(
					        stderr, "packet_no jumped 0x%08lx <-> 0x%08lx\n",
					        next_packet_no, packet_no);
					}
					*/

					next_packet_no = packet_no + 1;
					if (!data_provider.check_data(msg_buf, packet_no)) {
						{
							std::unique_lock lock(payload_mismatch_mutex);
							payload_mismatch_list.push_back({packet_no, msg_buf});
							msg_buf = new uint8_t[config.msg_size];
						}
						fprintf(stdout, "payload mismatch\n");
						continue;
					}

					while (!rx_buffer_output_queue.push(
					    PacketInfo{.packet_no = packet_no, .timestamp = ts})) {
						__builtin_ia32_pause();
					}
				}
				if (done)
					break;
			}
			delete[] cmsg_buf;
			delete[] msg_buf;
		}
	});

	BufferedChannel<SaveInfo> save_channel(128);

	std::atomic<uint64_t> recv_count = 0;
	auto rx_analysis = std::thread([&]() {
		LatencyHistogram latency_histogram{100'000, config.latency_histogram_log_bucket_width};
		RxTxPacketMatcher matcher(tx_queue, latency_histogram);
		using cs = std::chrono::high_resolution_clock;
		auto last = cs::now();
		while (1) {
			std::optional<PacketInfo> packet;
			while (!(packet = rx_buffer_output_queue.pop()) && !done && !should_save) {
				__builtin_ia32_pause();
			}

			if (packet) {
				recv_count++;
				matcher.add(*packet);
			}

			if (done || should_save) {
				if (!done && (recv_count != (packet->packet_no + 1))) {
					fprintf(
					    stderr, "recv_count packet_no mismatch %ld != %ld\n", (uint64_t) recv_count,
					    (packet->packet_no + 1));
				}

				std::vector<uint64_t> my_udp_drops;
				std::vector<std::pair<uint64_t, uint8_t*>> my_payload_mismatches;
				StartStopTimingInfo first_start_stop_timing;
				{
					std::unique_lock lock(udp_drops_start_stop_mutex);
					std::swap(my_udp_drops, udp_drops_list);
					assert(start_stop_timing.size() >= 1);
					first_start_stop_timing = start_stop_timing.front();
					start_stop_timing.pop_front();
				}
				{
					std::unique_lock lock(payload_mismatch_mutex);
					std::swap(payload_mismatch_list, my_payload_mismatches);
				}

				std::vector<PayloadMismatchInfo> payload_mismatch_info;
				for (auto& [packet_no, received_payload] : my_payload_mismatches) {
					uint8_t* wanted_payload = (uint8_t*) data_provider.get_payload_data(packet_no);
					payload_mismatch_info.push_back(
					    {packet_no,
					     std::vector<uint8_t>(wanted_payload, wanted_payload + config.msg_size - 8),
					     std::vector<uint8_t>(
					         received_payload + 8, received_payload + config.msg_size)});
					delete[] received_payload;
				}


				if (done) {
					save_channel.send(
					    {matcher.flush(), my_udp_drops, payload_mismatch_info, done,
					     first_start_stop_timing});
				} else {
					save_channel.send(
					    {matcher.save_slice(KEEP_OLD_TX_NUM), my_udp_drops, payload_mismatch_info,
					     done, first_start_stop_timing});
				}

				std::chrono::duration<double> dur = cs::now() - last;
				auto elapsed = dur.count();
				if (done) {
					elapsed -= config.after_send_wait_time;
				}
				auto rate = recv_count / elapsed;
				printf(
				    "\u001b[32m[rx] count = %ld, rate = %lf, bw = %lf\u001b[0m\n",
				    (uint64_t) recv_count, rate,
				    rate * MSG_WIRE_SIZE * 8.0 / 1000.0 / 1000.0 / 1000.0);

				should_save = false;
			}

			if (done) {
				break;
			}
		}
	});

	auto saver = std::thread([&]() {
		std::ofstream out_file(config.save_location, std::ios_base::binary);
		boost::iostreams::filtering_ostream out_stream;
		out_stream.push(boost::iostreams::gzip_compressor());
		out_stream.push(out_file);
		cereal::PortableBinaryOutputArchive oarchive(out_stream);
		oarchive(config);
		oarchive(time_keeper.get_base_offset());
		while (1) {
			auto to_save = save_channel.recv();

			oarchive(to_save);

			if (to_save.stop) {
				break;
			}
		}
		oarchive((uint64_t) sent_packets);
		oarchive((uint64_t) recv_count);
	});

	sender.join();
	receiver.join();
	rx_analysis.join();
	saver.join();
}
