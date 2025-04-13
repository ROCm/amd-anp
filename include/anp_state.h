#ifndef ANP_STATE_H_
#define ANP_STATE_H_

#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <boost/version.hpp>
#include "anp_metrics.h"

#ifdef ANP_DEBUG_ENABLED
    #define TELEMETRY_STATUS "enabled"
    #define ANP_DEBUG_EXECUTE(stmt) do { if (anp_state::anp_debug_enable) { stmt; } } while (0)
#else
    #define TELEMETRY_STATUS "disabled"
    #define ANP_DEBUG_EXECUTE(stmt)
#endif

#define IS_POWER_OF_2(number) \
    ((number > 0) && !(number & (number - 1)))

struct qp_status_s {
    bool data_qp;
};

typedef uint64_t counter_t;

struct qp_stats_s {
    counter_t num_wqe_sent;
    counter_t num_wqe_rcvd;
    counter_t num_wqe_completed;
    counter_t num_wqe_errors;
    counter_t num_slot_miss;
    uint64_t  wqe_completion_time_min;
    uint64_t  wqe_completion_time_max;
};

struct qp_info_s {
    qp_info_s(int max_buckets, uint32_t& bucket_sz_log2)
        : stats{},
          completion_metrics(max_buckets, bucket_sz_log2) {}

    qp_stats_s  stats;
    qp_status_s status;
    time_histogram_s completion_metrics;
    std::unordered_map<uint64_t, uint64_t> wqe_id_tracker;
};

using qp_info_ptr_t = std::shared_ptr<qp_info_s>;
// queue-id → queue_pair_info
using queue_pair_map_t = std::unordered_map<int, qp_info_ptr_t>;

struct channel_status_s {
};

struct channel_stats_s {
};

struct channel_s {
    channel_stats_s  stats;
    channel_status_s status;
    queue_pair_map_t queue_pairs;
};

struct device_status_s {
    std::string   eth_device;
    std::string   roce_device;
};

struct device_stats_s {
    device_stats_s()
        : cq_poll_count(0) {}

    std::map<uint32_t, size_t> wqe_size_metrics;
    counter_t                  cq_poll_count;
};

// channel-id → channel_info
using channel_map_t = std::unordered_map<int, channel_s>;

struct device_s {
    device_stats_s  stats;
    device_status_s status;
    channel_map_t   channels;
};
// device-id → device_info
using device_map_t = std::unordered_map<int, device_s>;

static inline size_t power_of_2 (size_t number) {
    if (number == 0) return 1;

    number--;
    number |= number >> 1;
    number |= number >> 2;
    number |= number >> 4;
    number |= number >> 8;
    number |= number >> 16;
#if SIZE_MAX > 0xFFFFFFFF
    // for 64-bit system
    number |= number >> 32;
#endif

    return (number + 1);
}

class anp_state {
public:
    void set_device_name(int device_id, const char *dev_name, const char *roce_dev_name) {
        auto device_it = devices.find(device_id);
        if (device_it == devices.end()) {
            devices[device_id].status.eth_device = dev_name;
            devices[device_id].status.roce_device = roce_dev_name;
            this->device_id = device_id;
        }
    }

    void add_queue_pair(int device_id, int channel_id, int qp_id, bool data_qp) {
        devices[device_id].channels[channel_id].queue_pairs[qp_id] = std::make_shared<qp_info_s>(max_buckets, bucket_sz_log2);
        queue_state[qp_id] = std::make_shared<qp_info_s>(max_buckets, bucket_sz_log2);
        queue_state[qp_id]->status.data_qp = data_qp;
    }

    void remove_queue_pair(int device_id, int channel_id, int qp_id) {
        auto device_it = devices.find(device_id);
        if (device_it != devices.end()) {
            auto& device = device_it->second;
            auto channel_it = device.channels.find(channel_id);
            if (channel_it != device.channels.end()) {
                channel_it->second.queue_pairs.erase(qp_id);
                if (channel_it->second.queue_pairs.empty()) {
                    device.channels.erase(channel_it);
                }
            }
            if (device.channels.empty()) {
                devices.erase(device_it);
            }
        }
    }

    void to_json(boost::property_tree::ptree& root) {
        boost::property_tree::ptree empty_object;
        empty_object.put("", "");
        boost::property_tree::ptree devices_node;

        for (auto& [device_id, device] : devices) {
            boost::property_tree::ptree device_entry;
            boost::property_tree::ptree device_status_node;
            boost::property_tree::ptree device_stats_node;
            counter_t num_wqe_sent_per_device = 0;
            counter_t num_wqe_rcvd_per_device = 0;
            counter_t num_data_qp_per_device = 0;
            counter_t num_cts_qp_per_device = 0;
            counter_t num_cts_sent_per_device = 0;

            // populate device status
            device_status_node.put("host_name", host_name);
            device_status_node.put("process_name", process_name);
            device_status_node.put("process_id", process_id);
            device_status_node.put("start_time", time_to_str(start_time));
            device_status_node.put("end_time", time_to_str(end_time));
            device_status_node.put("device_id", device_id);
            device_status_node.put("eth_device", device.status.eth_device);
            device_status_node.put("roce_device", device.status.roce_device);
            device_status_node.put("num_channels", device.channels.size());

            device_entry.add_child("status", device_status_node);

            boost::property_tree::ptree wqe_size_node;
            for (const auto& [wqe_size, count] : device.stats.wqe_size_metrics) {
                boost::property_tree::ptree wqe_size_entry;
                wqe_size_entry.put("wqe_size", wqe_size);
                wqe_size_entry.put("num_wqe", count);
                wqe_size_node.push_back(std::make_pair("", wqe_size_entry));
            }
            device_stats_node.add_child("wqe_size_stats", wqe_size_node);

            boost::property_tree::ptree channels_node;

            // populate channels
            for (const auto& [channel_id, channel] : device.channels) {
                boost::property_tree::ptree channel_entry;
                boost::property_tree::ptree channel_stats_node;
                counter_t num_wqe_sent_per_channel = 0;
                counter_t num_wqe_rcvd_per_channel = 0;
                counter_t num_data_qp_per_channel = 0;
                counter_t num_cts_qp_per_channel = 0;
                counter_t num_cts_sent_per_channel = 0;

                channel_entry.put("id", std::to_string(channel_id));
                channel_entry.put("num_queue_pairs", channel.queue_pairs.size());

                // populate queue pairs
                boost::property_tree::ptree queue_pairs_node;
                for (const auto& [qp_id, qp] : channel.queue_pairs) {
                    boost::property_tree::ptree queue_pair_entry;
                    boost::property_tree::ptree qp_status_node;
                    boost::property_tree::ptree qp_stats_node;
                    queue_pair_entry.put("id", std::to_string(qp_id));
                    if (queue_state.find(qp_id) != queue_state.end()) {
                        qp_status_node.put("data_qp", queue_state[qp_id]->status.data_qp);
                        if (queue_state[qp_id]->status.data_qp) {
                            num_data_qp_per_channel++;
                        } else {
                            num_cts_qp_per_channel++;
                            num_cts_sent_per_channel += queue_state[qp_id]->stats.num_wqe_sent;
                        }

                        qp_stats_node.put("num_wqe_sent", queue_state[qp_id]->stats.num_wqe_sent);
                        qp_stats_node.put("num_wqe_rcvd", queue_state[qp_id]->stats.num_wqe_rcvd);
                        qp_stats_node.put("num_wqe_completed", queue_state[qp_id]->stats.num_wqe_completed);
                        qp_stats_node.put("num_slot_miss", queue_state[qp_id]->stats.num_slot_miss);
                        qp_stats_node.put("wqe_completion_ns_min", queue_state[qp_id]->stats.wqe_completion_time_min);
                        qp_stats_node.put("wqe_completion_ns_max", queue_state[qp_id]->stats.wqe_completion_time_max);
                        num_wqe_sent_per_channel += queue_state[qp_id]->stats.num_wqe_sent;
                        num_wqe_rcvd_per_channel += queue_state[qp_id]->stats.num_wqe_rcvd;

                        boost::property_tree::ptree bucketsNode;
                        uint32_t bucket_sz_log2 = queue_state[qp_id]->completion_metrics.bucket_sz_log2;
                        std::map<uint64_t, size_t> ordered_buckets(queue_state[qp_id]->completion_metrics.buckets.begin(),
                                                                   queue_state[qp_id]->completion_metrics.buckets.end());
                        for (const auto& bucket : ordered_buckets) {
                            boost::property_tree::ptree bucket_node;
                            if (bucket.first < (max_buckets - 1)) {
                                bucket_node.put("latency_in_ns", ((bucket.first + 1) << bucket_sz_log2) - 1);
                            } else {
                                bucket_node.put("latency_in_ns", queue_state[qp_id]->stats.wqe_completion_time_max);
                            }
                            bucket_node.put("num_wqe", bucket.second);
                            bucketsNode.push_back(std::make_pair("", bucket_node));
                        }
                        qp_stats_node.put_child("wqe_completion_metrics", bucketsNode);
                        bucketsNode.clear();
                    }
                    queue_pair_entry.put_child("status", qp_status_node);
                    queue_pair_entry.add_child("stats", qp_stats_node);
                    queue_pairs_node.push_back(std::make_pair("", queue_pair_entry));
                }
                channel_entry.add_child("queue_pairs", queue_pairs_node);
                channel_entry.add_child("status", empty_object);
                num_wqe_sent_per_device += num_wqe_sent_per_channel;
                num_wqe_rcvd_per_device += num_wqe_rcvd_per_channel;
                num_data_qp_per_device += num_data_qp_per_channel;
                num_cts_qp_per_device += num_cts_qp_per_channel;
                num_cts_sent_per_device += num_cts_sent_per_channel;
                // wqe_sent per channel is inclusive of cts sent per channel, exclude it.
                num_wqe_sent_per_channel -= num_cts_sent_per_channel;
                channel_stats_node.put("num_wqe_sent", num_wqe_sent_per_channel);
                channel_stats_node.put("num_wqe_rcvd", num_wqe_rcvd_per_channel);
                channel_stats_node.put("num_cts_sent", num_cts_sent_per_channel);
                channel_stats_node.put("num_data_qp", num_data_qp_per_channel);
                channel_stats_node.put("num_cts_qp", num_cts_qp_per_channel);
                channel_entry.add_child("stats", channel_stats_node);
                channels_node.push_back(std::make_pair("", channel_entry));
            }
            // wqe_sent per device is inclusive of cts sent per device, exclude it.
            num_wqe_sent_per_device -= num_cts_sent_per_device;
            device_entry.add_child("channels", channels_node);
            device_stats_node.put("num_wqe_sent", num_wqe_sent_per_device);
            device_stats_node.put("num_wqe_rcvd", num_wqe_rcvd_per_device);
            device_stats_node.put("num_cts_sent", num_cts_sent_per_device);
            device_stats_node.put("num_data_qp", num_data_qp_per_device);
            device_stats_node.put("num_cts_qp", num_cts_qp_per_device);
            device_stats_node.put("cq_poll_count", device.stats.cq_poll_count);
            device_entry.add_child("stats", device_stats_node);
            devices_node.push_back(std::make_pair("", device_entry));
        }
        root.add_child("devices", devices_node);
    }

    void update_wqe_send_metrics(const int& qp_id,
                                 const uint64_t& wqe_id,
                                 const uint64_t& start_time) {
        auto& qp_info = queue_state[qp_id];
        if (!qp_info) {
            ANP_LOG_ERROR("invalid qp_id %d", qp_id);
            return;
        }
        qp_info->stats.num_wqe_sent++;
        qp_info->wqe_id_tracker[wqe_id] = start_time;
    }

    void update_wqe_rcvd_metrics(const int& qp_id,
                                 const uint64_t& wqe_id,
                                 const uint64_t& end_time) {
        auto& qp_info = queue_state[qp_id];
        if (!qp_info) {
            ANP_LOG_ERROR("invalid qp_id %d", qp_id);
            return;
        }
        qp_info->stats.num_wqe_rcvd++;
        if (qp_info->wqe_id_tracker.find(wqe_id) != qp_info->wqe_id_tracker.end()) {
            qp_info->stats.num_wqe_completed++;
            auto completion_time = end_time - qp_info->wqe_id_tracker[wqe_id];
            qp_info->stats.wqe_completion_time_max =
                (qp_info->stats.wqe_completion_time_max < completion_time) ?
                    completion_time : qp_info->stats.wqe_completion_time_max;

            qp_info->stats.wqe_completion_time_min =
                ((qp_info->stats.wqe_completion_time_min > completion_time) ||
                 !qp_info->stats.wqe_completion_time_min) ?
                    completion_time : qp_info->stats.wqe_completion_time_min;
            qp_info->completion_metrics.log_time(completion_time);
            qp_info->wqe_id_tracker.erase(wqe_id);
        }
    }

    void update_slot_miss_metrics(const int& qp_id) {
        auto& qp_info = queue_state[qp_id];
        if (!qp_info) {
            //ANP_LOG_ERROR("invalid qp_id %d", qp_id);
            return;
        }
        qp_info->stats.num_slot_miss++;
    }

    void update_cts_send_metrics(const int& qp_id) {
        auto& qp_info = queue_state[qp_id];
        if (!qp_info) {
            //ANP_LOG_ERROR("invalid qp_id %d", qp_id);
            return;
        }
        qp_info->stats.num_wqe_sent++;
    }

    void update_wqe_size_metrics(const uint32_t& wqe_length) {
        if (!devices.empty()) {
            devices.begin()->second.stats.wqe_size_metrics[wqe_length]++;
        }
    }

    void update_cq_poll_metrics() {
        if (!devices.empty()) {
            devices.begin()->second.stats.cq_poll_count++;
        }
    }

    // function to load the configuration from JSON
    void load_histogram_config() {
        boost::property_tree::ptree pt;

        try {
            if (anp_config_file_path.empty()) {
                ANP_LOG_ERROR("anp_config json not specified");
                return;
            }
            boost::property_tree::read_json(anp_config_file_path, pt);

            // loop through the metrics array
            for (const auto& metrics_node : pt.get_child("metrics")) {
                std::string name = metrics_node.second.get<std::string>("name");
                bucket_s<uint64_t> entry;

                // process buckets based on the type
                for (const auto& bucket_node : metrics_node.second.get_child("buckets")) {
                    entry.min = bucket_node.second.get<uint32_t>("min");
                    entry.max = bucket_node.second.get<uint32_t>("max");
                }
            }
        } catch (const std::exception& e) {
            ANP_LOG_ERROR("error parsing JSON: %s", e.what());
        }
    }

    std::string time_to_str(std::time_t& value) {
        std::tm* localTime = std::localtime(&value);
        std::ostringstream oss;
        oss << std::put_time(localTime, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    void update_process_name() {
        char buffer[256];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer)-1);
        if (len != -1) {
            buffer[len] = '\0';
            process_name = std::string(buffer);
        } else {
            ANP_LOG_ERROR("failed to retrieve process name");
            process_name = "unknown";
        }
    }

    void update_host_name() {
        char name[HOST_NAME_MAX];
        if (gethostname(name, HOST_NAME_MAX) == 0) {
            name[HOST_NAME_MAX - 1] ='\0';
            host_name = std::string(name);
        } else {
            host_name = "unknown";
        }
    }

    void load_config() {
        // check the debug_en environment variable; if set to "1", enable debugging
        const char* debug_env = std::getenv("AMD_ANP_DEBUG_EN");

        if (debug_env != NULL && std::string(debug_env) == "1") {
            anp_debug_enable = true;
        } else {
            anp_debug_enable = false;
        }

        // check the debug_en environment variable; if set to "1", enable debugging
        const char* config_file_env = std::getenv("AMD_ANP_CONFIG_FILE");

        if (config_file_env != NULL) {
            anp_config_file_path = std::string(config_file_env);
        }
        ANP_LOG_VERBOSE("config_json %s", anp_config_file_path.c_str());
        // try to open the config file
        std::ifstream file(anp_config_file_path);
        if (file.good()) {
            try {
                boost::property_tree::ptree pt;
                boost::property_tree::read_json(file, pt);

                // if the json has debug enabled, use it (default false if not found)
                bool json_flag = pt.get("debug_enabled", false);
                anp_debug_enable = anp_debug_enable || json_flag;
                std::string level = pt.get<std::string>("log_level", "ERROR"); // Default is ERROR
                if (level == "NONE")
                    anp_logger::log_level = LOG_NONE;
                else if (level == "INFO")
                    anp_logger::log_level = LOG_INFO;
                else if (level == "DEBUG")
                    anp_logger::log_level = LOG_DEBUG;
                else if (level == "VERBOSE")
                    anp_logger::log_level = LOG_VERBOSE;
                else
                    anp_logger::log_level = LOG_ERROR;
                // read the output_dir
                output_dir = pt.get("output_dir", "/tmp");
                // histogram bucket_interval_ns is expected to be in power of 2.
                // read the value and if it input is not a power of 2, adjust it to next power of 2.
                size_t bucket_interval_ns = pt.get<size_t>("bucket_interval_ns", 1024);
                if (!IS_POWER_OF_2(bucket_interval_ns)) {
                    bucket_interval_ns = power_of_2(bucket_interval_ns);
                }
                bucket_sz_log2 = (uint32_t) log2(bucket_interval_ns);
                max_buckets = pt.get<size_t>("max_buckets", 5);
                ANP_LOG_VERBOSE("config_json %s", anp_config_file_path.c_str());
                ANP_LOG_VERBOSE("log_level %d, input level %s", anp_logger::log_level, level.c_str());
                ANP_LOG_VERBOSE("output_dir %s, bucket_interval_ns(bucket_sz_log2) %lu(%u), max_buckets %d",
                                output_dir.c_str(), bucket_interval_ns, bucket_sz_log2, max_buckets);
            } catch (const std::exception& e) {
                ANP_LOG_ERROR("error parsing JSON: %s", e.what());
            }
        }
        ANP_LOG_VERBOSE("Process ID: %d, Thread ID: %lu", getpid(), pthread_self());
        ANP_LOG_VERBOSE("Boost version: %d", BOOST_VERSION);
    }

    void shutdown() {
        process_id = getpid();
        update_host_name();
        update_process_name();
        //load_histogram_config();
        if (!devices.empty()) {
            device_id = devices.begin()->first;
        }
        end_time = std::time(nullptr);
        write_json_to_file();
    }

    bool file_exists(const std::string& filename) {
        return access(filename.c_str(), F_OK) == 0;
    }

    void write_json_to_file() {
        if (output_dir.empty()) {
            ANP_LOG_ERROR("json output directory not specified");
            return;
        }
        std::string filename = output_dir + "/device_status_" + std::to_string(device_id) + ".json";
        std::string tmp_filename = filename + ".tmp";
        std::ofstream ofs(tmp_filename);
        boost::property_tree::ptree root;

        to_json(root);
        if (ofs.is_open()) {
            boost::property_tree::write_json(ofs, root);
            ofs.flush();
            ofs.close();
            if (!file_exists(tmp_filename)) {
                ANP_LOG_ERROR("temporary file %s does not exist", tmp_filename.c_str());
                return;
            }
            if (rename(tmp_filename.c_str(), filename.c_str()) != 0) {
                ANP_LOG_ERROR("failed to rename file %s to %s, err %d, %s",
                              tmp_filename.c_str(), filename.c_str(),
                              errno, strerror(errno));
            }
        } else {
            ANP_LOG_ERROR("failed to open file %s", filename.c_str());
        }
    }

    anp_state() {
        start_time = std::time(nullptr);
        load_config();
    }

    ~anp_state() {
        if (!anp_debug_enable) {
            return;
        }
        shutdown();
    }

private:
    int                    device_id;
    int                    process_id;
    std::string            host_name;
    std::string            output_dir;
    std::string            process_name;
    std::string            anp_config_file_path;
    std::time_t            start_time;
    std::time_t            end_time;
    int                    max_buckets;
    uint32_t               bucket_sz_log2;
    device_map_t           devices;
    queue_pair_map_t       queue_state;
    histogram_config_s     histogram_config;

public:
    static bool            anp_debug_enable;
};

#endif
