# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

import json
import os
import signal
import tempfile
import time
import threading
import curses


def get_output_dir():
    """Resolve the ANP output directory from the config file, matching the C++ plugin behavior."""
    config_path = os.environ.get("RCCL_ANP_CONFIG_FILE", "")
    if config_path:
        config_path = os.path.realpath(config_path)
        try:
            with open(config_path, "r") as f:
                config = json.load(f)
            output_dir = config.get("output_dir", "")
            if output_dir:
                output_dir = os.path.realpath(output_dir)
                if os.path.isdir(output_dir):
                    return output_dir
        except Exception:
            pass
    return tempfile.gettempdir()


FILES = [os.path.join(get_output_dir(), f"device_status_{i}.json") for i in range(8)]
UPDATE_INTERVAL = 1

device_index = 0

def load_json_file(filename):
    try:
        with open(filename, 'r') as file:
            return json.load(file)
    except Exception as e:
        print(f"Error reading {filename}: {e}")
        return None

def plot_device_status(stdscr):
    global device_index
    curses.curs_set(0)
    stdscr.nodelay(True)
    viewing_wqe_stats = False
    viewing_latency_stats = False
    page = 0

    while True:
        key = stdscr.getch()
        if key == ord('d'):
            device_index = (device_index + 1) % len(FILES)
        elif key == ord(' '):
            page += 1
        elif key == ord('s'):
            viewing_wqe_stats = True
            viewing_latency_stats = False
            page = 0
        elif key == ord('l'):
            viewing_latency_stats = True
            viewing_wqe_stats = False
            page = 0
        elif key == ord('r'):
            viewing_wqe_stats = False
            viewing_latency_stats = False
            page = 0

        device = load_json_file(FILES[device_index])
        if device:
            process_id = device["devices"][0]["status"]["process_id"]
            if os.path.exists(f"/proc/{process_id}"):
                os.kill(int(process_id), signal.SIGUSR1)
                os.sync()
                time.sleep(1)



        stdscr.erase()
        data = load_json_file(FILES[device_index])
        if not data:
            time.sleep(UPDATE_INTERVAL)
            continue

        device = data["devices"][0]
        status = device["status"]
        channels = device["channels"]
        stats = device["stats"]

        header = (
            f"Process: {status['process_name']} | Device ID: {status['device_id']} | RoCE: {status['roce_device']} | Channels: {status['num_channels']}\n"
            f"Legend: C - Cts QP | * - WQE Sent | + - WQE Rcvd\n"
            f"Space - Next Page | d - Next Device | s - WQE Size Stats | l - Latency Histogram | r - Return to Default\n"
        )
        stdscr.addstr(0, 0, header)
        header_lines = header.count('\n') + 2

        if viewing_wqe_stats:
            stdscr.erase()
            max_y, max_x = stdscr.getmaxyx()
            stdscr.addstr(0, 0, header)
            wqe_stats = stats.get("wqe_size_stats", [])
            wqe_start_line = header_lines + 2
            wqe_end_line = wqe_start_line
            if wqe_start_line < max_y:
                stdscr.addstr(wqe_start_line, 0, "WQE Size Stats:")
            for i, wqe_stat in enumerate(wqe_stats):
                wqe_size_bytes = int(wqe_stat.get("wqe_size", "0"))
                if wqe_size_bytes >= 1 << 30:
                    wqe_size = f"{wqe_size_bytes / (1 << 30):.2f} GB"
                elif wqe_size_bytes >= 1 << 20:
                    wqe_size = f"{wqe_size_bytes / (1 << 20):.2f} MB"
                elif wqe_size_bytes >= 1 << 10:
                    wqe_size = f"{wqe_size_bytes / (1 << 10):.2f} KB"
                else:
                    wqe_size = f"{wqe_size_bytes} B"
                num_wqe = int(wqe_stat.get("num_wqe", 0))
                max_wqe = max([int(stat.get('num_wqe', 1)) for stat in wqe_stats])
                label_length = len(f"wqe-size: {wqe_size}  ({num_wqe}) ")
                available_space = max_x - label_length - 1
                scaled_num_wqe = max(1, int((num_wqe / max_wqe) * available_space))
                wqe_line = f"wqe-size: {wqe_size} " + ('+' * scaled_num_wqe) + f" ({num_wqe})"
                if wqe_start_line + i + 1 < max_y - 1:
                    stdscr.addstr(wqe_start_line + i + 1, 0, wqe_line[:max_x - 1])
                    wqe_end_line += 1 

            device_start_line = wqe_end_line + 3

            if device_start_line + 5 < max_y - 1:
                stdscr.addstr(device_start_line, 0, "Device Stats Summary:")
                num_wqe_sent = int(stats.get("num_wqe_sent", 0))
                num_wqe_rcvd = int(stats.get("num_wqe_rcvd", 0))
                num_cts_sent = int(stats.get("num_cts_sent", 0))
                cq_poll_count = int(stats.get("cq_poll_count", 0))

                label_length = len(f"num_wqe_sent:  ({num_wqe_sent}) ")
                available_space = min(5, (max_x - label_length - 1))
                stats_line = f"num_wqe_sent: " + ('*' * available_space) + f" ({num_wqe_sent})"
                stdscr.addstr(device_start_line + 1, 0, stats_line[:max_x - 1])

                label_length = len(f"num_wqe_rcvd: ({num_wqe_rcvd}) ")
                available_space = min(5, (max_x - label_length - 1))
                stats_line = f"num_wqe_rcvd: " + ('+' * available_space) + f" ({num_wqe_rcvd})"
                stdscr.addstr(device_start_line + 2, 0, stats_line[:max_x - 1])

                label_length = len(f"num_cts_sent: ({num_cts_sent}) ")
                available_space = min(5, (max_x - label_length - 1))
                stats_line = f"num_cts_sent: " + ('#' * available_space) + f" ({num_cts_sent})"
                stdscr.addstr(device_start_line + 3, 0, stats_line[:max_x - 1])

                label_length = len(f"cq_poll_count: ({cq_poll_count}) ")
                available_space = min(5, (max_x - label_length - 1))
                stats_line = f"cq_poll_count:: " + ('-' * available_space) + f" ({cq_poll_count})"
                stdscr.addstr(device_start_line + 4, 0, stats_line[:max_x - 1])

            stdscr.refresh()
            time.sleep(UPDATE_INTERVAL)
            continue

        if viewing_latency_stats:
            stdscr.clear()
            max_y, max_x = stdscr.getmaxyx()
            header = "WQE Completion Histogram"
            stdscr.addstr(0, 0, header)

            # Pagination variables
            items_per_page = max_y - 5  # Leave space for header/footer
            current_page = 0

            latency_entries = []
            for channel in channels:
                channel_id = channel['id']
                for qp in channel['queue_pairs']:
                    latency_stats = qp['stats'].get("wqe_completion_metrics", [])
                    if latency_stats:  # Only include non-empty entries
                        latency_entries.append((channel_id, qp['id'], latency_stats))

            total_pages = (len(latency_entries) + items_per_page - 1) // items_per_page  # Round up

            while True:
                stdscr.clear()
                stdscr.addstr(0, 0, header)
                start_idx = current_page * items_per_page
                end_idx = start_idx + items_per_page

                line_offset = 2
                for channel_id, qp_id, latency_stats in latency_entries[start_idx:end_idx]:
                    if line_offset >= max_y - 2:
                        break
                    stdscr.addstr(line_offset, 0, f"Channel-{channel_id:<4} | QP-{qp_id:<6}")
                    line_offset += 1
                    max_latency = max([int(stat.get('num_wqe', 1)) for stat in latency_stats], default=1)

                    for latency_stat in latency_stats:
                        if line_offset >= max_y - 2:
                            break
                        latency_ns = latency_stat.get("latency_in_ns", "Unknown")
                        count = int(latency_stat.get("num_wqe", 0))
                        label_length = len(f"Latency: {latency_ns} ns  ({count}) ")
                        available_space = max_x - label_length - 1
                        scaled_count = max(1, int((count / max_latency) * available_space))
                        latency_line = f"Latency: {latency_ns} ns " + ('#' * scaled_count) + f" ({count})"
                        stdscr.addstr(line_offset, 2, latency_line[:max_x - 1])
                        line_offset += 1

                # Pagination footer
                stdscr.addstr(max_y - 1, 0, f"Page {current_page + 1} of {total_pages} (Arrow keys to navigate, 'q' to exit)")

                stdscr.refresh()
                key = stdscr.getch()

                if key == ord('q'):
                    break
                elif key == curses.KEY_RIGHT or key == curses.KEY_NPAGE:  # Next page
                    if current_page < total_pages - 1:
                        current_page += 1
                elif key == curses.KEY_LEFT or key == curses.KEY_PPAGE:  # Previous page
                    if current_page > 0:
                        current_page -= 1

            stdscr.refresh()
            time.sleep(UPDATE_INTERVAL)
            continue

        x = []
        x_labels = []
        y_sent = []
        y_rcv = []

        for channel in channels:
            channel_id = channel['id']
            for qp in channel['queue_pairs']:
                qp_id = qp['id']
                stats_data = qp['stats']
                is_data_qp = qp['status']['data_qp']
                if int(stats_data['num_wqe_sent']) > 0:
                    label = f"Channel-{channel_id} | QP-{qp_id}"
                    if is_data_qp == "false":
                        label += f" (C)"
                    x_labels.append(label)
                    x.append(len(x))
                    y_sent.append(int(stats_data['num_wqe_sent']))
                    y_rcv.append(int(stats_data['num_wqe_rcvd']))

        max_y, max_x = stdscr.getmaxyx()
        graph_height = max_y - 10

        max_regular_value = max(max(y_sent + y_rcv), 1) if (y_sent + y_rcv) else 1
        normalized_sent = [int((val / max_regular_value) * graph_height) for val in y_sent]
        normalized_rcv = [int((val / max_regular_value) * graph_height) for val in y_rcv]

        lines_per_page = (max_y - 10) // 2
        total_pages = (len(x) + lines_per_page - 1) // lines_per_page
        start_idx = page * lines_per_page
        end_idx = min(start_idx + lines_per_page, len(x))

        for idx in range(start_idx, end_idx):
            sent_line = f"{x_labels[idx].split('|')[0].strip()} {x_labels[idx].split('|')[1].strip()} "
            sent_line += '*' * normalized_sent[idx] + f" ({y_sent[idx]})"
            stdscr.addstr((idx - start_idx) * 2 + header_lines, 0, sent_line[:max_x - 1])

            rcv_line = f"{x_labels[idx].split('|')[0].strip()} {x_labels[idx].split('|')[1].strip()} "
            rcv_line += '+' * normalized_rcv[idx] + f" ({y_rcv[idx]})"
            stdscr.addstr((idx - start_idx) * 2 + header_lines + 1, 0, rcv_line[:max_x - 1])

        stdscr.refresh()
        time.sleep(UPDATE_INTERVAL)

def main():
    curses.wrapper(plot_device_status)

if __name__ == "__main__":
    main()

