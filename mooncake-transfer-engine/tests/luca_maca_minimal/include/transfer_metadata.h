#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include "common.h"
#include "fake_json.h"
namespace mooncake {
class TransferMetadata {
   public:
    struct BufferDesc {
        std::string name;
        uint64_t addr = 0, length = 0;
        std::vector<uint32_t> lkey, rkey;
        std::string shm_name;
        uint64_t offset = 0;
    };
    struct SegmentDesc {
        std::string name, protocol;
        std::vector<BufferDesc> buffers;
    };
    struct HandShakeDesc {};
    struct NotifyDesc {};
    std::map<uint64_t, std::shared_ptr<SegmentDesc>> segments;
    int publication_failures = 0;
    explicit TransferMetadata(const std::string& = "") {}
    std::shared_ptr<SegmentDesc> getSegmentDescByID(uint64_t id) {
        auto found = segments.find(id);
        return found == segments.end() ? nullptr : found->second;
    }
    int addLocalSegment(uint64_t id, const std::string&,
                        std::shared_ptr<SegmentDesc> desc) {
        segments[id] = std::move(desc);
        return 0;
    }
    int addLocalMemoryBuffer(const BufferDesc& buffer, bool update) {
        segments.at(0)->buffers.push_back(buffer);
        return update ? updateLocalSegmentDesc() : 0;
    }
    int removeLocalMemoryBuffer(void* address, bool update) {
        auto& buffers = segments.at(0)->buffers;
        auto found = std::find_if(
            buffers.begin(), buffers.end(), [&](const BufferDesc& b) {
                return b.addr == reinterpret_cast<uint64_t>(address);
            });
        if (found == buffers.end()) return -3;
        buffers.erase(found);
        return update ? updateLocalSegmentDesc() : 0;
    }
    int updateLocalSegmentDesc() {
        if (publication_failures > 0) {
            --publication_failures;
            return -200;
        }
        return 0;
    }
};
}  // namespace mooncake
