#define MCAP_IMPLEMENTATION
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#include <MCAPLogger.hpp>

static const char* recordings_dir() {
    const char* path = std::getenv("RECORDINGS_DIR");
    return path ? path : "/home/nixos/recordings";
}

/****************************************************************
 * HELPER METHODS
 ****************************************************************/
static std::string get_logfile_name() {
  std::string dir_path = recordings_dir();
  int max_file_number = 0;

  try {
    for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
      if (entry.is_regular_file()) {
        std::string filename = entry.path().filename().string();
        try {
          int current_number = std::stoi(filename);
          if (current_number > max_file_number) {
              max_file_number = current_number;
          }
        } catch (const std::invalid_argument& e) {
          spdlog::error("Skipping non-numeric file: {}", filename);
        }
      }
    }
  }  catch (const std::filesystem::filesystem_error& e) {
    spdlog::error("Filesystem error");
    return "uhoh.mcap";
  }

  return std::to_string(max_file_number + 1) + ".mcap";
}

static std::vector<const google::protobuf::FileDescriptor *> get_pb_descriptors(const std::vector<std::string> &filenames) {
    std::vector<const google::protobuf::FileDescriptor *> descriptors;

    for (const auto &name : filenames) {
        const google::protobuf::FileDescriptor *file_descriptor = google::protobuf::DescriptorPool::generated_pool()->FindFileByName(name);

        if (!file_descriptor) {
            spdlog::error("File descriptor not found: {}", name);
        }
        else {
            descriptors.push_back(file_descriptor);
        }
    }
    return descriptors;
}

static std::string serialize_fd_set(const google::protobuf::Descriptor *toplevelDescriptor) {
    google::protobuf::FileDescriptorSet fdSet;
    std::queue<const google::protobuf::FileDescriptor *> toAdd;
    toAdd.push(toplevelDescriptor->file());
    std::unordered_set<std::string> seenDependencies;
    while (!toAdd.empty())
    {
        const google::protobuf::FileDescriptor *next = toAdd.front();
        toAdd.pop();
        next->CopyTo(fdSet.add_file());
        for (int i = 0; i < next->dependency_count(); ++i)
        {
            const auto &dep = next->dependency(i);
            if (seenDependencies.find(dep->name()) == seenDependencies.end())
            {
                seenDependencies.insert(dep->name());
                toAdd.push(dep);
            }
        }
    }
    return fdSet.SerializeAsString();
}

/****************************************************************
 * PUBLIC CLASS METHOD IMPLEMENTATIONS
 ****************************************************************/
void core::MCAPLogger::create(const mcap::McapWriterOptions &options) {
    MCAPLogger* expected = nullptr;
    MCAPLogger* local = new MCAPLogger(options);
    if(!_s_instance.compare_exchange_strong(expected, local, std::memory_order_release, std::memory_order_relaxed)) {
        // Already initialized, delete local instance
        delete local;
    }
}

core::MCAPLogger& core::MCAPLogger::instance() {
    MCAPLogger* instance = _s_instance.load(std::memory_order_acquire);
    assert(instance != nullptr && "MCAPLogger has not been initialized");
    return *instance;
}

void core::MCAPLogger::destroy() {
    MCAPLogger* instance = _s_instance.exchange(nullptr, std::memory_order_acq_rel);
    if (instance) {
        delete instance;
    }
}

int core::MCAPLogger::open_new_mcap() {
    std::filesystem::create_directories(recordings_dir());
    std::string mcap_name = get_logfile_name();
    spdlog::info("Attempting to open new MCAP file: {}", mcap_name);
    _log_name = std::string(recordings_dir()) + "/" + mcap_name;

    const auto res = _writer.open(_log_name, _options);
    if (!res.ok()) {
        spdlog::error("Failed to open {} for writing: {}", _log_name, res.message);
        return -1;
    }

    std::vector<std::string> proto_names = {"drivebrain_msgs.proto"};

    auto descriptors = get_pb_descriptors(proto_names);

    for (const auto &file_descriptor : descriptors) {
        for (int i = 0; i < file_descriptor->message_type_count(); i++) {
            const google::protobuf::Descriptor *message_descriptor = file_descriptor->message_type(i);
            mcap::Schema schema(message_descriptor->full_name(), "protobuf", serialize_fd_set(message_descriptor));
            _writer.addSchema(schema);
            mcap::Channel channel(message_descriptor->name(), "protobuf", schema.id);
            _writer.addChannel(channel);
            _name_to_id_map[message_descriptor->name()] = channel.id;
            _type_to_schema_id_map[message_descriptor->name()] = schema.id;
        }
    }

    spdlog::info("Successfully added message descriptions to mcap");

    return 0;
}

void core::MCAPLogger::init_logging() {
    {
        std::lock_guard lock(_input_buffer_mutex);
        _logging = true;
    }
    _msg_log_thread = std::thread([this]() { _handle_log_to_file(); });
    spdlog::info("Msg log thread spawned");
}

int core::MCAPLogger::log_msg(core::MsgType message, const std::string &topic) {
    mcap::Timestamp log_time = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    RawMessage_s new_message;
    new_message.log_time = log_time;
    new_message.serialized_data = message->SerializeAsString();
    new_message.type_name = message->GetDescriptor()->name();
    new_message.message_name = topic.empty() ? new_message.type_name : topic;
    {
        std::unique_lock lock(_input_buffer_mutex);
        if (!_logging) return -1; // Logging is not active, return an error code
        _input_buffer.push_back(std::move(new_message));
        _input_buffer_cv.notify_one();
   }

    return 0;
}

/****************************************************************
 * PRIVATE CLASS METHOD IMPLEMENTATIONS
 ****************************************************************/
core::MCAPLogger::MCAPLogger(const mcap::McapWriterOptions &options) : _options(options) {
}

void core::MCAPLogger::_handle_log_to_file() {
    static std::deque<RawMessage_s> write_buffer; // The buffer to be copied over to

    while (true) {
        {
            std::unique_lock lock(_input_buffer_mutex);
            _input_buffer_cv.wait(lock, [this](){ return !_input_buffer.empty() || !_logging;});
            if (!_logging && _input_buffer.empty()) {
                break;
            }

            write_buffer = std::move(_input_buffer);
        }

        for (auto &msg : write_buffer) {
            mcap::Message msg_to_log;
            msg_to_log.data = reinterpret_cast<const std::byte *>(msg.serialized_data.data());
            msg_to_log.dataSize = msg.serialized_data.size();

            msg_to_log.logTime = msg.log_time;
            msg_to_log.publishTime = msg.log_time;

            auto channel = _name_to_id_map.find(msg.message_name);
            if (channel == _name_to_id_map.end()) {
                // first message on this topic, add a channel for it with its message type's schema
                mcap::Channel new_channel(msg.message_name, "protobuf", _type_to_schema_id_map[msg.type_name]);
                _writer.addChannel(new_channel);
                channel = _name_to_id_map.emplace(msg.message_name, new_channel.id).first;
            }
            msg_to_log.channelId = channel->second;
            auto write_res = _writer.write(msg_to_log);
        }
        write_buffer.clear();
    }
}
