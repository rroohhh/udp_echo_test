#!/usr/bin/env python3
import analysis
import numpy as np
import matplotlib
import matplotlib.style
import matplotlib.pyplot as plt
from tqdm import tqdm
import sys

matplotlib.use('QtAgg')
matplotlib.style.use('fast')

def save_all_as_pdf(name):
    from matplotlib.backends.backend_pdf import PdfPages
    pdf = PdfPages(name)
    for n in plt.get_fignums():
        plt.figure(n).savefig(pdf, format="pdf")
    pdf.close()

def fmt_config(config):
    return f"""userspace_timestamp = {config.userspace_timestamp}
after_send_wait_time = {config.after_send_wait_time}
use_packet_mmap = {config.use_packet_mmap}
msg_size = {config.msg_size}
rate = {config.rate}
packet_count = {config.packet_count}
latency_histogram_log_bucket_width = {config.latency_histogram_log_bucket_width}
no_different_payload = {config.no_different_payload}
random_seed = {config.random_seed}"""


if len(sys.argv) == 2:
    filename = sys.argv[1]
else:
    filename = "out.gz"

dump = analysis.load_file(filename)

latency_hists = []
loss_event = []
udp_drops = []

hist_max = 0
latency_mean = 0.0

start_time = 1e10
sent_packets = 0

min_s = []
max_s = []
num_out_of_order_s = []
num_mismatch_s = []

ll = 61
# for i in range(dump.config.no_different_payload):
#     print(f"{i:016x}")
#     for k in range(dump.config.msg_size // ll):
#         for b in dump.get_payload()[i * dump.config.msg_size + k * ll + 8: i * dump.config.msg_size + (k + 1) * ll + 8]:
#             print(f"{b:02x}", end="")
#         print()


for i, s in enumerate(tqdm(dump.slices)):
    timing = s.start_stop_timing
    mismatches = s.payload_mismatches

    num_mismatch_s.append(len(mismatches))

    # for loss in np.array(s.slice.old_tx.packet_nos()):
        # print(f"loss: {loss:016x}")

    # for mismatch in s.payload_mismatches:
    #     print()
    #     print(f"{mismatch.packet_no:016x}")

    #     for part in range(len(mismatch.expected) // ll):
    #         for b in mismatch.expected[ll * part: ll * (part + 1)]:
    #             print(f"{b:02x}", end="")
    #         print()
    #         for b in mismatch.got[ll * part: ll * (part + 1)]:
    #             print(f"{b:02x}", end="")
    #         print()

    #     mismatch.expected, mismatch.got

    if sent_packets != timing.sent_packets:
        sent_packets = timing.sent_packets
        start_time = min(start_time, timing.start.nsecs)
        stop_time = timing.stop.nsecs

    sl = s.slice
    num_out_of_order_s.append(sl.num_out_of_order)
    if len(sl.unmatched_rx) != 0:
        print(f"warning, no handling for unmatched_rx currently: {list(sl.unmatched_rx.packet_nos())}")
    for hs in sl.hist_slices:
        min_s.append(hs.min)
        max_s.append(hs.max)
        latency_hists.append(hs.buckets)
        hist_max = max(hist_max, len(hs.buckets))
        log_bucket_width = hs.log_bucket_width
        latency_mean += hs.latency_sum / hs.count

    loss_event.append(np.array(sl.old_tx.packet_nos()))
    udp_drops += s.udp_drops

latency_mean = latency_mean / len(latency_hists) / 1e6
udp_drops = np.array(udp_drops)

latency_hist_buckets = np.zeros((len(latency_hists), hist_max))
for i, b in enumerate(latency_hists):
    latency_hist_buckets[i,:len(b)] = b
hist = np.sum(latency_hist_buckets, axis=0)

min_bucket = 0
while min_bucket < len(hist) and hist[min_bucket] == 0:
    min_bucket += 1

log_bucket_width = np.exp(log_bucket_width)
tick_value = np.ones(hist_max + 1)
tick_value[1:] = log_bucket_width
tick_value = np.cumprod(tick_value)

N = np.sum(hist)
n_low = N * 0.001
n_high = N * 0.999

idx_low = None
idx_high = None

hist_tot = np.cumsum(hist)
for i in range(len(hist)):
    if hist_tot[i] > n_low and idx_low is None:
        idx_low = i - 1

    if hist_tot[i] > n_high and idx_high is None:
        idx_high = i - 1
        break


ll_e = np.concatenate(loss_event)
print(filename, np.sum(num_mismatch_s), np.sum(num_out_of_order_s), len(ll_e), 100 * len(ll_e) / dump.sent_count, 1e9 * sent_packets / (stop_time - start_time))
print(np.min(min_s) / 1e3, latency_mean * 1e3, np.max(max_s) / 1e3, tick_value[idx_low] / 1e3, tick_value[idx_low + 1] / 1e3, tick_value[idx_high] / 1e3, tick_value[idx_high + 1] / 1e3)

hist = hist[min_bucket:]
latency_hist_buckets = latency_hist_buckets[min_bucket:]


tick_value = tick_value[min_bucket:] / 1e6 # ns to ms


total = int(sum(hist))
plt.figure(figsize=(18,12))
plt.vlines(latency_mean, 0, np.max(hist) * 1.1, color="black", label="mean")
plt.title(f"N = {total}, μ = {latency_mean} ms")
plt.bar(tick_value[:-1], hist, align='edge', width=np.diff(tick_value), color="blue")
plt.yscale("log")
plt.xscale("log")
plt.xlabel("latency [ms]")
plt.ylabel("no packets")
plt.text(0.05, 0.75, fmt_config(dump.config), transform=plt.gca().transAxes)
plt.tight_layout()


if True:
    loss_event = np.concatenate(loss_event)

    if len(loss_event) > 0:
        bins = np.arange(
            0,
            (np.max(loss_event) + dump.config.save_interval - 1),
            dump.config.save_interval
        )
        hist, _ = np.histogram(loss_event, bins)
        d = np.diff(bins)
        hist = hist / d * 100

        udp_drops[1:] -= udp_drops[:-1]
        plt.figure(figsize=(18,12))
        plt.bar(bins[:-1], hist, color="blue", align="edge", width=d, label="total drops")
        # plt.bar(bins[:-1], udp_drops / d * 100, color="orange", align="edge", width=d, label="network stack drops")
        plt.legend()
        plt.xlabel("number of sent packets")
        plt.ylabel("lost packets [%]")
        plt.title(f"lost packets, sent = {dump.sent_count}, received = {dump.recv_count}, rate = {1e9 * sent_packets / (stop_time - start_time)} / s, total loss = {100 * len(loss_event) / dump.sent_count}%")
        plt.text(0.05, 0.75, fmt_config(dump.config), transform=plt.gca().transAxes)
        plt.tight_layout()


save_all_as_pdf(filename + ".pdf")
#    plt.show()

if False:
    plt.pcolor(latency_hist_buckets, norm=matplotlib.colors.LogNorm())
    plt.xlabel("latency [ms]")
    plt.xticks(np.arange(0, hist_max), tick_value)
    plt.colorbar()
    plt.show()

    # print("unmatched_rx", s.slice.unmatched_rx)
    # print("old_tx", s.slice.old_tx)
    # print("old_tx.len()", len(s.slice.old_tx))
    # print("unmatched_rx", s.slice.unmatched_rx)
    # for hist_slice in s.slice.hist_slices:
    #     print("hist_slice", hist_slice.buckets)
