#include "transport/luca_transport/luca_transport.h"
#include <cuda.h>
#include <lcr/lc_runtime_api.h>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <utility>

namespace mooncake {
namespace {
bool nativeOK(lcError_t rc, const char* operation) {
    if (rc == lcSuccess) return true;
    LOG(ERROR) << "LucaTransport: " << operation << " rc=" << int(rc);
    return false;
}
bool allocationRange(void* ptr, size_t length, uint64_t& base, size_t& size) {
    CUdeviceptr allocation = 0;
    CUresult rc = cuMemGetAddressRange(&allocation, &size,
                                      reinterpret_cast<CUdeviceptr>(ptr));
    if (rc != CUDA_SUCCESS) {
        // Never guess the allocation base: an IPC import is relocated from it.
        LOG(ERROR) << "LucaTransport: cuMemGetAddressRange rc=" << int(rc)
                   << " ptr=" << ptr;
        return false;
    }
    base = static_cast<uint64_t>(allocation);
    const uint64_t address = reinterpret_cast<uint64_t>(ptr);
    if (!base || !size || address < base || address - base > size ||
        length > size - (address - base)) {
        LOG(ERROR) << "LucaTransport: range outside allocation";
        return false;
    }
    return true;
}
std::string toHex(const void* ptr, size_t size) {
    static const char digits[] = "0123456789abcdef";
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    std::string text(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        text[2*i] = digits[bytes[i] >> 4];
        text[2*i+1] = digits[bytes[i] & 15];
    }
    return text;
}
bool fromHex(const std::string& text, void* ptr, size_t size) {
    if (text.size() != size * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto* bytes = static_cast<unsigned char*>(ptr);
    for (size_t i = 0; i < size; ++i) {
        int high = nibble(text[2*i]), low = nibble(text[2*i+1]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}
}  // namespace

int LucaTransport::install(std::string& name,
                          std::shared_ptr<TransferMetadata> meta,
                          std::shared_ptr<Topology>) {
    int count = 0;
    if (!nativeOK(lcGetDeviceCount(&count), "lcGetDeviceCount") || count != 1) {
        LOG(ERROR) << "LucaTransport v1 requires one visible GPU; count=" << count;
        return -1;
    }
    if (!nativeOK(lcSetDevice(0), "lcSetDevice")) return -1;
    const char* override_id = std::getenv("MC_LUCA_HOST_ID");
    if (override_id && *override_id) host_id_ = override_id;
    else {
        std::ifstream boot_id("/proc/sys/kernel/random/boot_id");
        std::getline(boot_id, host_id_);
    }
    if (host_id_.empty()) {
        LOG(ERROR) << "LucaTransport: cannot determine host boot ID";
        return -1;
    }
    metadata_ = std::move(meta);
    local_server_name_ = name;
    auto desc = std::make_shared<SegmentDesc>();
    desc->name = name;
    desc->protocol = "luca";
    LOG(INFO) << "LucaTransport: Cross v2, synchronous D2D, single visible GPU";
    return metadata_->addLocalSegment(LOCAL_SEGMENT_ID, name, std::move(desc));
}

int LucaTransport::registerLocalMemory(void* ptr, size_t length,
                                      const std::string& location,
                                      bool remote_accessible, bool update) {
    std::lock_guard<std::mutex> lock(mutex_);
    (void)remote_accessible;
    if (!ptr || !length || !nativeOK(lcSetDevice(0), "lcSetDevice")) return -1;
    uint64_t base = 0;
    size_t size = 0;
    if (!allocationRange(ptr, length, base, size)) return -1;
    lcIpcCrossMemHandle_t handle{};
    if (!nativeOK(lcIpcGetMemHandleCross_v2(&handle,
                      reinterpret_cast<void*>(base)), "lcIpcGetMemHandleCross_v2"))
        return -1;
    Json::Value envelope;
    envelope["format"] = "luca-cross-v2";
    envelope["abi_size"] = static_cast<Json::UInt64>(sizeof(handle));
    envelope["host_id"] = host_id_;
    envelope["allocation_base"] = static_cast<Json::UInt64>(base);
    envelope["allocation_size"] = static_cast<Json::UInt64>(size);
    envelope["handle_hex"] = toHex(&handle, sizeof(handle));
    BufferDesc buffer{};
    buffer.name = location.empty() ? "cuda:0" : location;
    buffer.addr = reinterpret_cast<uint64_t>(ptr);
    buffer.length = length;
    buffer.shm_name = envelope.toStyledString();
    int rc = metadata_->addLocalMemoryBuffer(buffer, update);
    if (rc) {
        // addLocalMemoryBuffer inserts locally before publishing metadata.
        metadata_->removeLocalMemoryBuffer(ptr, false);
    }
    return rc;
}

int LucaTransport::unregisterLocalMemory(void* ptr, bool update) {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_->removeLocalMemoryBuffer(ptr, update);
}
int LucaTransport::registerLocalMemoryBatch(const std::vector<BufferEntry>& entries,
                                           const std::string& location) {
    std::vector<void*> inserted;
    for (const auto& entry : entries) {
        int rc = registerLocalMemory(entry.addr, entry.length, location, true, false);
        if (rc) {
            for (void* ptr : inserted) unregisterLocalMemory(ptr, false);
            return rc;
        }
        inserted.push_back(entry.addr);
    }
    int rc = metadata_->updateLocalSegmentDesc();
    if (rc) for (void* ptr : inserted) unregisterLocalMemory(ptr, false);
    return rc;
}
int LucaTransport::unregisterLocalMemoryBatch(const std::vector<void*>& entries) {
    int first_error = 0;
    for (void* ptr : entries) {
        int rc = unregisterLocalMemory(ptr, false);
        if (rc && !first_error) first_error = rc;
    }
    int rc = metadata_->updateLocalSegmentDesc();
    return first_error ? first_error : rc;
}

bool LucaTransport::transferOne(const TransferRequest& request) {
    if (request.opcode != TransferRequest::READ &&
        request.opcode != TransferRequest::WRITE) return false;
    if (!request.length) return true;
    if (!request.source || !nativeOK(lcSetDevice(0), "lcSetDevice")) return false;
    // Validate the local pointer as device memory, including suballocation bounds.
    uint64_t local_base = 0;
    size_t local_size = 0;
    if (!allocationRange(request.source, request.length, local_base, local_size))
        return false;
    // Order copies after work previously submitted on this local device.
    if (!nativeOK(lcDeviceSynchronize(), "local pre-copy synchronize")) return false;
    if (request.target_id == LOCAL_SEGMENT_ID) {
        void* peer = reinterpret_cast<void*>(request.target_offset);
        uint64_t peer_base = 0;
        size_t peer_size = 0;
        if (!allocationRange(peer, request.length, peer_base, peer_size)) return false;
        void* dst = request.opcode == TransferRequest::READ ? request.source : peer;
        const void* src = request.opcode == TransferRequest::READ ? peer : request.source;
        bool copied = nativeOK(lcMemcpy(dst, src, request.length,
                                       lcMemcpyDeviceToDevice), "lcMemcpy D2D");
        bool synced = nativeOK(lcDeviceSynchronize(), "post-copy synchronize");
        return copied && synced;
    }
    auto segment = metadata_->getSegmentDescByID(request.target_id);
    if (!segment || segment->protocol != "luca") return false;
    for (const auto& buffer : segment->buffers) {
        const uint64_t address = request.target_offset;
        if (address < buffer.addr || address - buffer.addr > buffer.length ||
            request.length > buffer.length - (address - buffer.addr)) continue;
        Json::Value envelope;
        Json::Reader reader;
        if (!reader.parse(buffer.shm_name, envelope, false) ||
            !envelope.isObject() || !envelope["format"].isString() ||
            envelope["format"].asString() != "luca-cross-v2" ||
            !envelope["abi_size"].isUInt64() ||
            envelope["abi_size"].asUInt64() != sizeof(lcIpcCrossMemHandle_t) ||
            !envelope["host_id"].isString() ||
            envelope["host_id"].asString() != host_id_ ||
            !envelope["allocation_base"].isUInt64() ||
            !envelope["allocation_size"].isUInt64() ||
            !envelope["handle_hex"].isString()) {
            LOG(ERROR) << "LucaTransport: invalid handle envelope, ABI or host mismatch";
            return false;
        }
        const uint64_t base = envelope["allocation_base"].asUInt64();
        const uint64_t size = envelope["allocation_size"].asUInt64();
        if (!base || buffer.addr < base || buffer.addr - base > size ||
            buffer.length > size - (buffer.addr - base) || address < base ||
            address - base > size || request.length > size - (address - base))
            return false;
        lcIpcCrossMemHandle_t handle{};
        if (!fromHex(envelope["handle_hex"].asString(), &handle, sizeof(handle)))
            return false;
        void* mapped = nullptr;
        if (!nativeOK(lcIpcOpenMemHandleCross_v2(&mapped, &handle,
                          lcIpcMemLazyEnablePeerAccess), "lcIpcOpenMemHandleCross_v2"))
            return false;
        const uint64_t mapping = reinterpret_cast<uint64_t>(mapped);
        const uint64_t offset = address - base;
        if (!mapping || offset > std::numeric_limits<uint64_t>::max() - mapping) {
            nativeOK(lcIpcCloseMemHandle(mapped), "lcIpcCloseMemHandle");
            return false;
        }
        void* peer = reinterpret_cast<void*>(mapping + offset);
        void* dst = request.opcode == TransferRequest::READ ? request.source : peer;
        const void* src = request.opcode == TransferRequest::READ ? peer : request.source;
        bool copied = nativeOK(lcMemcpy(dst, src, request.length,
                                       lcMemcpyDeviceToDevice), "lcMemcpy D2D");
        bool synced = nativeOK(lcDeviceSynchronize(), "post-copy synchronize");
        bool closed = nativeOK(lcIpcCloseMemHandle(mapped), "lcIpcCloseMemHandle");
        return copied && synced && closed;
    }
    LOG(ERROR) << "LucaTransport: target range is not registered";
    return false;
}

Status LucaTransport::submitTransferTask(const std::vector<TransferTask*>& tasks) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Prevalidate the whole batch before changing any task state.
    for (const auto* task : tasks)
        if (!task || !task->request || task->slice_count)
            return Status::InvalidArgument("LucaTransport: invalid or reused task");
    for (auto* task : tasks) {
        const auto& request = *task->request;
        task->total_bytes = request.length;
        Slice* slice = getSliceCache().allocate();
        slice->source_addr = request.source;
        slice->length = request.length;
        slice->opcode = request.opcode;
        slice->target_id = request.target_id;
        slice->task = task;
        slice->status = Slice::PENDING;
        slice->ts = 0;
        slice->local.dest_addr = nullptr;
        task->slice_list.push_back(slice);
        __sync_fetch_and_add(&task->slice_count, 1);
        // Every accepted task becomes COMPLETED or FAILED; never leave later
        // tasks with zero slices when one request has an invalid remote range.
        if (transferOne(request)) slice->markSuccess();
        else slice->markFailed();
    }
    // API submission succeeded; copy failures are reported through task status.
    return Status::OK();
}
Status LucaTransport::submitTransfer(BatchID id,
                                    const std::vector<TransferRequest>& requests) {
    auto& batch = *reinterpret_cast<BatchDesc*>(id);
    if (batch.task_list.size() + requests.size() > batch.batch_size)
        return Status::TooManyRequests("LucaTransport: batch capacity exceeded");
    const size_t start = batch.task_list.size();
    batch.task_list.resize(start + requests.size());
    std::vector<TransferTask*> tasks;
    for (size_t i = 0; i < requests.size(); ++i) {
        auto& task = batch.task_list[start + i];
        task.batch_id = id;
        task.request = &requests[i];
        tasks.push_back(&task);
    }
    return submitTransferTask(tasks);
}
Status LucaTransport::getTransferStatus(BatchID id, size_t index,
                                       TransferStatus& status) {
    auto& batch = *reinterpret_cast<BatchDesc*>(id);
    if (index >= batch.task_list.size())
        return Status::InvalidArgument("LucaTransport: task index out of range");
    auto& task = batch.task_list[index];
    status.transferred_bytes = task.transferred_bytes;
    if (task.slice_count && task.success_slice_count + task.failed_slice_count ==
                            task.slice_count) {
        status.s = task.failed_slice_count ? FAILED : COMPLETED;
        task.is_finished = true;
    } else status.s = WAITING;
    return Status::OK();
}
}  // namespace mooncake
